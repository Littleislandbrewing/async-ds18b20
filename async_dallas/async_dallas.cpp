#include "async_dallas.h"
#include "esphome/core/log.h"
#include <new> // Required for std::nothrow

namespace esphome {
namespace async_dallas {

static const char *TAG = "async_dallas";

AsyncDallasSensor::AsyncDallasSensor(uint8_t pin, uint32_t interval) 
    : PollingComponent(interval), pin_(pin) {}

void AsyncDallasSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Nuclear-Hard Dallas on Pin %u...", pin_);
  
  // 1. ALLOCATE ONCE (Boot Time Only)
  // We use std::nothrow to prevent 'abort' on OOM, allowing us to handle it safely.
  one_wire_ = new (std::nothrow) OneWire(pin_);
  if (!one_wire_) {
      ESP_LOGE(TAG, "FATAL: Heap Exhaustion (OneWire).");
      this->mark_failed();
      return;
  }

  sensors_ = new (std::nothrow) DallasTemperature(one_wire_);
  if (!sensors_) {
      ESP_LOGE(TAG, "FATAL: Heap Exhaustion (DallasTemp).");
      this->mark_failed();
      return;
  }

  // 2. INITIALIZE HARDWARE
  sensors_->begin();
  
  // 3. ENFORCE PHYSICS (The "Grounding" Rule)
  // We explicitly set 12-bit resolution so our 750ms delay is physically correct.
  sensors_->setResolution(12);
  sensors_->setWaitForConversion(false); 

  // 4. VERIFY TOPOLOGY
  // Determinism check: If >1 sensor exists, Index(0) is risky.
  if (sensors_->getDeviceCount() > 1) {
      ESP_LOGE(TAG, "FATAL: Multiple sensors on Pin %u! This component requires 1 sensor per pin.", pin_);
      this->mark_failed();
      return;
  }
  if (sensors_->getDeviceCount() == 0) {
      ESP_LOGW(TAG, "Warning: No sensor detected on Pin %u.", pin_);
  }

  // 5. SAFE THREADING
  result_mutex_ = xSemaphoreCreateMutex();
  if (result_mutex_ == NULL) {
      ESP_LOGE(TAG, "FATAL: Mutex creation failed.");
      this->mark_failed();
      return;
  }

  BaseType_t res;
  // 8192 Bytes stack (Safe margin for logging overhead)
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
  ESP_LOGCONFIG(TAG, "  Mode: Nuclear-Hard (Static Allocation)");
}

void AsyncDallasSensor::update() {
  // AUDIT FIX: Guard against accessing a dead task
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
        ESP_LOGW(TAG, "Pin %u: Bus unstable (10x Fail). Re-syncing...", this_sensor->pin_);
        
        // AUDIT FIX: No 'delete/new'. We re-use the existing memory.
        // Calling begin() forces the library to re-scan the bus and reset the state machine.
        this_sensor->sensors_->begin();
        this_sensor->sensors_->setResolution(12); // Enforce resolution again
        this_sensor->sensors_->setWaitForConversion(false);
        
        error_count = 0;
        just_reset = true;
    }

    // --- 2. THE WORK (Async) ---
    this_sensor->sensors_->requestTemperatures();
    
    // AUDIT FIX: 750ms matches our forced 12-bit resolution
    vTaskDelay(750 / portTICK_PERIOD_MS); 
    
    float temp = this_sensor->sensors_->getTempCByIndex(0);

    // --- 3. HEALTH CHECK ---
    // Range: -55 to +125 is valid.
    // -127 is Disconnect.
    // We allow 85.0 (Brewing Valid), but catch true hardware failures.
    if (temp < -50 || temp > 125) { 
        error_count++;
    } else {
        if (error_count > 0) {
             ESP_LOGI(TAG, "Pin %u: Signal recovered.", this_sensor->pin_);
        }
        error_count = 0;
    }

    // --- 4. DELIVERY ---
    // If the component failed elsewhere, stop processing
    if (this_sensor->is_failed()) {
        vTaskDelete(NULL); // Only suicide if the parent object is dead
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
  // AUDIT FIX: Don't run if failed
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
