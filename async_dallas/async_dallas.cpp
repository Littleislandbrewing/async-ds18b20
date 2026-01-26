#include "async_dallas.h"
#include "esphome/core/log.h"

namespace esphome {
namespace async_dallas {

static const char *TAG = "async_dallas";

AsyncDallasSensor::AsyncDallasSensor(uint8_t pin, uint32_t interval) 
    : PollingComponent(interval), pin_(pin) {}

void AsyncDallasSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Async Dallas on Pin %u...", pin_);
  
  // 1. Initialize Driver
  one_wire_ = new OneWire(pin_);
  sensors_ = new DallasTemperature(one_wire_);
  sensors_->begin();
  
  // 2. RIGID: Disable blocking wait in the library.
  sensors_->setWaitForConversion(false); 

  // 3. Create Mutex
  result_mutex_ = xSemaphoreCreateMutex();

  // 4. Spawn Worker on CORE 0 (Pro Core)
  BaseType_t res = xTaskCreatePinnedToCore(
      this->task_worker, "dallas_0", 4096, this, 1, &task_handle_, 0
  );

  if (res != pdPASS) {
    ESP_LOGE(TAG, "CRITICAL FAILURE: Could not spawn worker task on Core 0");
    this->mark_failed();
  }
}

void AsyncDallasSensor::dump_config() {
  LOG_SENSOR("", "Async Dallas Sensor", this);
  ESP_LOGCONFIG(TAG, "  Pin: %u", pin_);
  ESP_LOGCONFIG(TAG, "  Architecture: Dual-Core (Worker Pinned to Core 0)");
}

void AsyncDallasSensor::update() {
  // Triggered by Main Loop (Core 1)
  if (!request_pending_) {
    request_pending_ = true;
    xTaskNotifyGive(task_handle_); // Wake up Core 0
  }
}

void AsyncDallasSensor::task_worker(void *pvParameters) {
  AsyncDallasSensor *this_sensor = (AsyncDallasSensor *)pvParameters;

  for (;;) {
    // Sleep until notified (0% CPU)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // --- HEAVY LIFTING START (Core 0) ---
    this_sensor->sensors_->requestTemperatures();
    
    // Non-blocking wait (Yields Core 0 to WiFi/System)
    vTaskDelay(750 / portTICK_PERIOD_MS); 

    // Read result
    float temp = this_sensor->sensors_->getTempCByIndex(0);

    // Save result thread-safely
    if (xSemaphoreTake(this_sensor->result_mutex_, 10) == pdTRUE) {
      this_sensor->latest_temp_ = temp;
      xSemaphoreGive(this_sensor->result_mutex_);
    }
    // --- HEAVY LIFTING END ---
  }
}

void AsyncDallasSensor::loop() {
  // Collector (Core 1)
  float new_val = NAN;
  
  if (xSemaphoreTake(result_mutex_, 0) == pdTRUE) { 
    if (!isnan(latest_temp_)) {
        new_val = latest_temp_;
        latest_temp_ = NAN; // Clear it
    }
    xSemaphoreGive(result_mutex_);
  }

  if (!isnan(new_val) && new_val > -55 && new_val < 125) {
    publish_state(new_val);
    request_pending_ = false;
  }
}

} // namespace async_dallas
} // namespace esphome
