#include "async_dallas.h"
#include "esphome/core/log.h"

namespace esphome {
namespace async_dallas {

static const char *TAG = "async_dallas";

AsyncDallasSensor::AsyncDallasSensor(uint8_t pin, uint32_t interval) 
    : PollingComponent(interval), pin_(pin) {}

void AsyncDallasSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Async Dallas on Pin %u...", pin_);
  
  one_wire_ = new OneWire(pin_);
  sensors_ = new DallasTemperature(one_wire_);
  sensors_->begin();

  if (sensors_->getDeviceCount() == 0) {
      ESP_LOGW(TAG, "No DS18B20 sensors found on Pin %u!", pin_);
  }
  
  sensors_->setWaitForConversion(false); 
  result_mutex_ = xSemaphoreCreateMutex();

  BaseType_t res;
  #if portNUM_PROCESSORS > 1
      ESP_LOGI(TAG, "Dual Core detected. Pinning worker to Core 0.");
      res = xTaskCreatePinnedToCore(this->task_worker, "dallas_0", 4096, this, 1, &task_handle_, 0);
  #else
      ESP_LOGI(TAG, "Single Core detected. Spawning unpinned worker.");
      res = xTaskCreate(this->task_worker, "dallas_w", 4096, this, 1, &task_handle_);
  #endif

  if (res != pdPASS) {
    ESP_LOGE(TAG, "CRITICAL FAILURE: Could not spawn worker task!");
    this->mark_failed();
  }
}

void AsyncDallasSensor::dump_config() {
  LOG_SENSOR("", "Async Dallas Sensor", this);
  ESP_LOGCONFIG(TAG, "  Pin: %u", pin_);
  ESP_LOGCONFIG(TAG, "  Update Interval: %.1fs", this->get_update_interval() / 1000.0f);
}

void AsyncDallasSensor::update() {
  if (!request_pending_) {
    request_pending_ = true;
    xTaskNotifyGive(task_handle_);
  }
}

void AsyncDallasSensor::task_worker(void *pvParameters) {
  AsyncDallasSensor *this_sensor = (AsyncDallasSensor *)pvParameters;
  
  uint8_t error_count = 0;
  const uint8_t MAX_ERRORS = 10;
  bool just_reset = false; // Flag to skip data immediately after a reset

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // --- 1. THE DOCTOR: Bus Recovery Logic ---
    if (error_count >= MAX_ERRORS) {
        ESP_LOGW(TAG, "Pin %u: Bus Latch-Up (10x Fail). Performing Hard Reset...", this_sensor->pin_);
        
        // A. Teardown
        delete this_sensor->sensors_;
        delete this_sensor->one_wire_;
        
        // B. Physical Flush (The "Slap")
        pinMode(this_sensor->pin_, OUTPUT);
        digitalWrite(this_sensor->pin_, LOW);
        vTaskDelay(10 / portTICK_PERIOD_MS);
        digitalWrite(this_sensor->pin_, HIGH);
        pinMode(this_sensor->pin_, INPUT);
        
        // C. Rebuild
        this_sensor->one_wire_ = new OneWire(this_sensor->pin_);
        this_sensor->sensors_ = new DallasTemperature(this_sensor->one_wire_);
        this_sensor->sensors_->begin();
        this_sensor->sensors_->setWaitForConversion(false);
        
        error_count = 0;
        just_reset = true; // Mark this cycle as "Recovery"
    }

    // --- 2. THE WORK ---
    this_sensor->sensors_->requestTemperatures();
    vTaskDelay(750 / portTICK_PERIOD_MS); 
    float temp = this_sensor->sensors_->getTempCByIndex(0);

    // --- 3. HEALTH CHECK ---
    // RIGID FIX: Removed "temp == 85.0" from error check. 
    // 85C is a valid brewing temp. We cannot treat it as a crash.
    if (temp <= -100) {
        error_count++;
    } else {
        if (error_count > 0) {
             ESP_LOGI(TAG, "Pin %u: Signal recovered.", this_sensor->pin_);
        }
        error_count = 0;
    }

    // --- 4. DELIVERY ---
    if (just_reset) {
        // Skip delivery once to let the sensor settle after the hard reset
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
  float new_val = NAN;
  bool received_data = false;
  
  if (xSemaphoreTake(result_mutex_, 0) == pdTRUE) { 
    if (!isnan(latest_temp_)) {
        new_val = latest_temp_;
        latest_temp_ = NAN;
        received_data = true;
    }
    xSemaphoreGive(result_mutex_);
  }

  if (received_data) {
    // RIGID: Range sanity check. 
    // If it's -127 (Disconnect), we publish NAN to trigger Safety Logic.
    if (new_val < -50 || new_val > 150) {
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
