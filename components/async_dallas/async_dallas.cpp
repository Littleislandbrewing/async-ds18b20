#include "async_dallas.h"
#include "esphome/core/log.h"
#include <new>

namespace esphome {
namespace async_dallas {

static const char *TAG = "async_dallas";

void AsyncDallasSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Nuclear-Hard Dallas on Pin %u...", pin_);
  
  // ESP32-S3 GPIO initialization - CRITICAL for external OneWire library
  pinMode(pin_, INPUT_PULLUP);
  delay(50);
  
  // 1. ALLOCATE MEMORY
  one_wire_ = new (std::nothrow) OneWire(pin_);
  if (!one_wire_) {
      ESP_LOGE(TAG, "FATAL: Heap Exhaustion (OneWire).");
      this->mark_failed();
      return;
  }

  delay(100);

  sensors_ = new (std::nothrow) DallasTemperature(one_wire_);
  if (!sensors_) {
      ESP_LOGE(TAG, "FATAL: Heap Exhaustion (DallasTemp).");
      this->mark_failed();
      return;
  }

  // 2. INITIALIZE HARDWARE
  sensors_->begin();
  
  // 3. SET RESOLUTION
  sensors_->setResolution(12);
  sensors_->setWaitForConversion(false); 

  uint8_t current_res = sensors_->getResolution();
  if (current_res != 12) {
      ESP_LOGW(TAG, "SETUP WARNING: Sensor on Pin %u stuck at %d-bit! CHECK POWER.", pin_, current_res);
  } else {
      ESP_LOGI(TAG, "Pin %u: Configured for 12-bit resolution.", pin_);
  }

  // 4. VERIFY TOPOLOGY
  uint8_t device_count = sensors_->getDeviceCount();
  if (device_count == 0) {
      ESP_LOGW(TAG, "SETUP WARNING: No sensors found on Pin %u!", pin_);
  } else if (device_count > 1) {
      ESP_LOGE(TAG, "FATAL: Multiple sensors (%d) on Pin %u! This component requires 1 sensor per pin.", device_count, pin_);
      this->mark_failed();
      return;
  } else {
      // LOG ADDRESS
      DeviceAddress device_addr;
      if (sensors_->getAddress(device_addr, 0)) {
          char addr_str[24];
          snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
              device_addr[0], device_addr[1], device_addr[2], device_addr[3],
              device_addr[4], device_addr[5], device_addr[6], device_addr[7]);
          ESP_LOGI(TAG, "Found Sensor on Pin %u. Address: %s", pin_, addr_str);
      }
  }

  // 5. SAFE THREADING
  result_mutex_ = xSemaphoreCreateMutex();
  if (result_mutex_ == NULL) {
      ESP_LOGE(TAG, "FATAL: Mutex creation failed.");
      this->mark_failed();
      return;
  }

  BaseType_t res;
  #if portNUM_PROCESSORS > 1
      res = xTaskCreatePinnedToCore(this->task_worker, "dallas_0", 8192, this, 1, &task_handle_, 0);
  #else
      res = xTaskCreate(this->task_worker, "dallas_w", 8192, this, 1, &task_handle_);
  #endif

  if (res != pdPASS) {
    ESP_LOGE(TAG, "FATAL: Worker Task creation failed.");
    this->mark_failed();
  }
}

void AsyncDallasSensor::dump_config() {
  LOG_SENSOR("", "Async Dallas Sensor", this);
  ESP_LOGCONFIG(TAG, "  Pin: %u", pin_);
  ESP_LOGCONFIG(TAG, "  Mode: Nuclear-Hard (Static Alloc + Forensics)");
}

void AsyncDallasSensor::update() {
  if (this->is_failed() || task_handle_ == nullptr) {
      return;
  }

  if (!request_pending_) {
    request_pending_ = true;
    xTaskNotifyGive(task_handle_);
  }
}

void AsyncDallasSensor::task_worker(void *pvParameters) {
  AsyncDallasSensor *this_sensor = (AsyncDallasSensor *)pvParameters;
  
  uint8_t error_count = 0;
  const uint8_t MAX_ERRORS = 10;
  bool just_reset = false;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // --- 1. THE DOCTOR: Safe Bus Recovery ---
    if (error_count >= MAX_ERRORS) {
        ESP_LOGW(TAG, "Pin %u: Bus Critical (10x Fail). Resetting Driver...", this_sensor->pin_);
        
        this_sensor->sensors_->begin();
        this_sensor->sensors_->setResolution(12); 
        this_sensor->sensors_->setWaitForConversion(false);
        
        error_count = 0;
        just_reset = true;
    }

    // --- 2. THE WORK (Async) ---
    this_sensor->sensors_->requestTemperatures();
    
    vTaskDelay(750 / portTICK_PERIOD_MS); 
    
    float temp = this_sensor->sensors_->getTempCByIndex(0);

    // --- 3. FORENSIC DIAGNOSTICS ---
    if (temp < -50 || temp > 125) { 
        error_count++;
        
        if (temp <= -100) {
            if (this_sensor->sensors_->getDS18Count() == 0) {
                 ESP_LOGD(TAG, "Pin %u Error: PHYSICAL DISCONNECT (No sensors found).", this_sensor->pin_);
            } else {
                 ESP_LOGD(TAG, "Pin %u Error: CRC/NOISE (Sensor present, data corrupted). Check 3.3V/Cable.", this_sensor->pin_);
            }
        } else {
             ESP_LOGD(TAG, "Pin %u Error: OUT OF RANGE. Value: %.2f", this_sensor->pin_, temp);
        }

    } else {
        if (error_count > 0) {
             ESP_LOGI(TAG, "Pin %u: Signal recovered after %d errors.", this_sensor->pin_, error_count);
        }
        error_count = 0;
    }

    // --- 4. DELIVERY ---
    if (this_sensor->is_failed()) {
        vTaskDelete(NULL); 
    }

    if (just_reset) {
        just_reset = false; 
    } else {
        if (xSemaphoreTake(this_sensor->result_mutex_, portMAX_DELAY) == pdTRUE) {
          this_sensor->latest_temp_ = temp;
          xSemaphoreGive(this_sensor->result_mutex_);
        }
    }
  }
}

void AsyncDallasSensor::loop() {
  if (this->is_failed()) return;

  float new_val = NAN;
  bool received_data = false;
  
  if (result_mutex_ != NULL && xSemaphoreTake(result_mutex_, 0) == pdTRUE) { 
    if (!isnan(latest_temp_)) {
        new_val = latest_temp_;
        latest_temp_ = NAN;
        received_data = true;
    }
    xSemaphoreGive(result_mutex_);
  }

  if (received_data) {
    if (new_val < -50 || new_val > 125) {
        ESP_LOGW(TAG, "Invalid reading on Pin %u: %.2f", pin_, new_val);
        publish_state(NAN); 
    } else {
        publish_state(new_val);
    }
    request_pending_ = false; 
  }
}

} // namespace async_dallas
} // namespace esphome
