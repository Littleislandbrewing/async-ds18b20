#include "async_dallas.h"
#include "esphome/core/log.h"
#include <new>

namespace esphome {
namespace async_dallas {

static const char *TAG = "async_dallas";

// ============================================================================
// AsyncDallasComponent (HUB)
// ============================================================================

void AsyncDallasComponent::setup() {
  ESP_LOGE(TAG, "🔥🔥🔥 SETUP ENTERED 🔥🔥🔥");
  
  // Let ESPHome handle the pin - NO pinMode()
  pin_->setup();
  
  uint8_t pin_num = pin_->get_pin();
  ESP_LOGE(TAG, "🔥 Pin number: %u", pin_num);
  
  delay(100);
  
  // Allocate OneWire
  one_wire_ = new (std::nothrow) OneWire(pin_num);
  if (!one_wire_) {
    ESP_LOGE(TAG, "FATAL: Heap Exhaustion (OneWire).");
    this->mark_failed();
    return;
  }
  ESP_LOGE(TAG, "🔥 OneWire created");

  delay(100);

  // Allocate DallasTemperature
  sensors_ = new (std::nothrow) DallasTemperature(one_wire_);
  if (!sensors_) {
    ESP_LOGE(TAG, "FATAL: Heap Exhaustion (DallasTemp).");
    this->mark_failed();
    return;
  }
  ESP_LOGE(TAG, "🔥 DallasTemperature created");

  // Initialize hardware
  sensors_->begin();
  sensors_->setWaitForConversion(false);
  ESP_LOGE(TAG, "🔥 Hardware initialized");

  // Discover devices
  uint8_t device_count = sensors_->getDeviceCount();
  ESP_LOGE(TAG, "🔥🔥🔥 Found %d device(s) on Pin %u 🔥🔥🔥", device_count, pin_num);
  
  for (uint8_t i = 0; i < device_count; i++) {
    DeviceAddress device_addr;
    if (sensors_->getAddress(device_addr, i)) {
      uint64_t addr = 0;
      for (uint8_t j = 0; j < 8; j++) {
        addr |= ((uint64_t)device_addr[j]) << (j * 8);
      }
      
      char addr_str[24];
      snprintf(addr_str, sizeof(addr_str), "0x%02X%02X%02X%02X%02X%02X%02X%02X",
        device_addr[7], device_addr[6], device_addr[5], device_addr[4],
        device_addr[3], device_addr[2], device_addr[1], device_addr[0]);
      
      ESP_LOGI(TAG, "  Device %d: %s", i, addr_str);
      temp_cache_[addr] = NAN;
    }
  }

  // Create mutex
  result_mutex_ = xSemaphoreCreateMutex();
  if (result_mutex_ == NULL) {
    ESP_LOGE(TAG, "FATAL: Mutex creation failed.");
    this->mark_failed();
    return;
  }
  ESP_LOGE(TAG, "🔥 Mutex created");

  // Create FreeRTOS task
  BaseType_t res;
  #if portNUM_PROCESSORS > 1
    res = xTaskCreatePinnedToCore(this->task_worker, "dallas_hub", 8192, this, 1, &task_handle_, 0);
  #else
    res = xTaskCreate(this->task_worker, "dallas_hub", 8192, this, 1, &task_handle_);
  #endif

  if (res != pdPASS) {
    ESP_LOGE(TAG, "FATAL: Worker Task creation failed.");
    this->mark_failed();
  } else {
    ESP_LOGE(TAG, "🔥🔥🔥 SETUP COMPLETE - TASK RUNNING 🔥🔥🔥");
  }
}

void AsyncDallasComponent::dump_config() {
  uint8_t pin_num = pin_->get_pin();
  ESP_LOGCONFIG(TAG, "Async Dallas Hub:");
  ESP_LOGCONFIG(TAG, "  Pin: %u", pin_num);
  ESP_LOGCONFIG(TAG, "  Sensors: %d", sensors_list_.size());
}

void AsyncDallasComponent::register_sensor(AsyncDallasSensor *sensor) {
  sensors_list_.push_back(sensor);
}

void AsyncDallasComponent::task_worker(void *pvParameters) {
  AsyncDallasComponent *hub = (AsyncDallasComponent *)pvParameters;
  const uint8_t MAX_ERRORS = 10;
  bool just_reset = false;

  ESP_LOGE(TAG, "🔥 WORKER TASK STARTED 🔥");

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (hub->error_count_ >= MAX_ERRORS) {
      uint8_t pin_num = hub->pin_->get_pin();
      ESP_LOGW(TAG, "Pin %u: Bus Critical. Resetting...", pin_num);
      
      hub->sensors_->begin();
      hub->sensors_->setWaitForConversion(false);
      
      hub->error_count_ = 0;
      just_reset = true;
    }

    hub->sensors_->requestTemperatures();
    vTaskDelay(750 / portTICK_PERIOD_MS);

    if (xSemaphoreTake(hub->result_mutex_, portMAX_DELAY) == pdTRUE) {
      uint8_t device_count = hub->sensors_->getDeviceCount();
      
      for (uint8_t i = 0; i < device_count; i++) {
        DeviceAddress device_addr;
        if (hub->sensors_->getAddress(device_addr, i)) {
          uint64_t addr = 0;
          for (uint8_t j = 0; j < 8; j++) {
            addr |= ((uint64_t)device_addr[j]) << (j * 8);
          }
          
          float temp = hub->sensors_->getTempC(device_addr);
          
          if (temp < -50 || temp > 125) {
            hub->error_count_++;
          } else {
            if (hub->error_count_ > 0) {
              ESP_LOGI(TAG, "Device %d: Signal recovered", i);
            }
            hub->error_count_ = 0;
          }
          
          if (!just_reset) {
            hub->temp_cache_[addr] = temp;
          }
        }
      }
      
      xSemaphoreGive(hub->result_mutex_);
    }

    just_reset = false;
    
    if (hub->is_failed()) {
      vTaskDelete(NULL);
    }
  }
}

void AsyncDallasComponent::loop() {
  if (this->is_failed()) return;

  // Trigger update every 1 second
  uint32_t now = millis();
  if (now - last_update_ >= 1000) {
    last_update_ = now;
    
    if (!request_pending_ && task_handle_ != nullptr) {
      request_pending_ = true;
      xTaskNotifyGive(task_handle_);
    }
    
    // Update sensors
    for (auto *sensor : sensors_list_) {
      sensor->update();
    }
    
    request_pending_ = false;
  }
}

float AsyncDallasComponent::get_temperature_c(uint64_t address) {
  float result = NAN;
  
  if (result_mutex_ != NULL && xSemaphoreTake(result_mutex_, 0) == pdTRUE) {
    auto it = temp_cache_.find(address);
    if (it != temp_cache_.end()) {
      result = it->second;
    }
    xSemaphoreGive(result_mutex_);
  }
  
  return result;
}

// ============================================================================
// AsyncDallasSensor (Individual Sensor)
// ============================================================================

void AsyncDallasSensor::update() {
  float temp = parent_->get_temperature_c(address_);
  
  if (temp < -50 || temp > 125) {
    this->publish_state(NAN);
  } else {
    this->publish_state(temp);
  }
}

void AsyncDallasSensor::dump_config() {
  LOG_SENSOR("", "Async Dallas Sensor", this);
  if (has_address_) {
    ESP_LOGCONFIG(TAG, "  Address: 0x%016llX", address_);
  } else {
    ESP_LOGCONFIG(TAG, "  Index: %d", index_);
  }
  ESP_LOGCONFIG(TAG, "  Resolution: %d bits", resolution_);
}

} // namespace async_dallas
} // namespace esphome
