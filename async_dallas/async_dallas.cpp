#include "async_dallas.h"
#include "esphome/core/log.h"

namespace esphome {
namespace async_dallas {

static const char *TAG = "async_dallas";

AsyncDallasSensor::AsyncDallasSensor(uint8_t pin, uint32_t interval) 
    : PollingComponent(interval), pin_(pin) {}

void AsyncDallasSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Async Dallas on Pin %u...", pin_);
  
  // FIX #4: Safe Allocation
  one_wire_ = new OneWire(pin_);
  if (!one_wire_) {
      ESP_LOGE(TAG, "FATAL: Failed to allocate OneWire!");
      this->mark_failed();
      return;
  }

  sensors_ = new DallasTemperature(one_wire_);
  if (!sensors_) {
      ESP_LOGE(TAG, "FATAL: Failed to allocate DallasTemperature!");
      this->mark_failed();
      return;
  }

  sensors_->begin();
  if (sensors_->getDeviceCount() == 0) {
      ESP_LOGW(TAG, "No DS18B20 sensors found on Pin %u!", pin_);
  }
  
  sensors_->setWaitForConversion(false); 

  // FIX #1: NULL Mutex Check
  result_mutex_ = xSemaphoreCreateMutex();
  if (result_mutex_ == NULL) {
      ESP_LOGE(TAG, "CRITICAL: Failed to create mutex!");
      this->mark_failed();
      return;
  }

  BaseType_t res;
  // FIX #3: Stack Overflow Risk -> Increased to 8192 (8KB)
  #if portNUM_PROCESSORS > 1
      ESP_LOGI(TAG, "Dual Core detected. Pinning worker to Core 0.");
      res = xTaskCreatePinnedToCore(this->task_worker, "dallas_0", 8192, this, 1, &task_handle_, 0);
  #else
      ESP_LOGI(TAG, "Single Core detected. Spawning unpinned worker.");
      res = xTaskCreate(this->task_worker, "dallas_w", 8192, this, 1, &task_handle_);
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
  bool just_reset = false;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // --- 1. THE DOCTOR: Bus Recovery Logic ---
    if (error_count >= MAX_ERRORS) {
        ESP_LOGW(TAG, "Pin %u: Bus Latch-Up (10x Fail). Resetting Driver...", this_sensor->pin_);
        
        // FIX #2: Removed Unsafe GPIO Manipulation (pinMode/digitalWrite).
        // We rely on destroying and re-creating the library objects. 
        // The OneWire constructor handles the physical bus reset safely.

        // Safe Teardown
        if (this_sensor->sensors_) { delete this_sensor->sensors_; this_sensor->sensors_ = nullptr; }
        if (this_sensor->one_wire_) { delete this_sensor->one_wire_; this_sensor->one_wire_ = nullptr; }
        
        // Brief pause to let electrical transients settle
        vTaskDelay(20 / portTICK_PERIOD_MS);
        
        // FIX #4: Safe Re-allocation
        this_sensor->one_wire_ = new OneWire(this_sensor->pin_);
        if (!this_sensor->one_wire_) {
            ESP_LOGE(TAG, "FATAL: Heap exhaustion during reset!");
            vTaskDelete(NULL); // Suicide to prevent crash
        }

        this_sensor->sensors_ = new DallasTemperature(this_sensor->one_wire_);
        if (!this_sensor->sensors_) {
             ESP_LOGE(TAG, "FATAL: Heap exhaustion during reset!");
             vTaskDelete(NULL);
        }

        this_sensor->sensors_->begin(); // This performs the Reset Pulse safely
        this_sensor->sensors_->setWaitForConversion(false);
        
        error_count = 0;
        just_reset = true;
    }

    // --- 2. THE WORK ---
    this_sensor->sensors_->requestTemperatures();
    vTaskDelay(750 / portTICK_PERIOD_MS); 
    float temp = this_sensor->sensors_->getTempCByIndex(0);

    // --- 3. HEALTH CHECK ---
    // FIX #6 & #7: Standardized Thresholds (-55 to +125 is valid range)
    // -127 is Disconnect, 85 is Power-On (Valid for Brewing).
    // We strictly catch -127 or clearly impossible negative values.
    if (temp < -50 || temp > 150) { 
        error_count++;
    } else {
        if (error_count > 0) {
             ESP_LOGI(TAG, "Pin %u: Signal recovered.", this_sensor->pin_);
        }
        error_count = 0;
    }

    // --- 4. DELIVERY ---
    if (just_reset) {
        just_reset = false; 
    } else {
        // FIX #1: The mutex was checked at setup, but we check here for sanity
        if (this_sensor->result_mutex_ != NULL) {
            if (xSemaphoreTake(this_sensor->result_mutex_, portMAX_DELAY) == pdTRUE) {
              this_sensor->latest_temp_ = temp;
              xSemaphoreGive(this_sensor->result_mutex_);
            }
        }
    }
  }
}

void AsyncDallasSensor::loop() {
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
    // FIX #7: Consistent Validation in Loop
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
