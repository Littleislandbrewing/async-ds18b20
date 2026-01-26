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
  // Give USB Serial time to connect before we do anything
  delay(2000);
  
  ESP_LOGE(TAG, "🔥🔥🔥 SETUP CALLED!!! 🔥🔥🔥");
  
  // Allow ESPHome to do its internal pin bookkeeping
  pin_->setup();
  
  uint8_t pin_num = pin_->get_pin();
  ESP_LOGCONFIG(TAG, "Setting up Async Dallas Hub on Pin %u...", pin_num);
  
  // ESP32-S3 GPIO initialization
  pinMode(pin_num, INPUT_PULLUP);
  delay(50);
  
  // Allocate OneWire
  one_wire_ = new (std::nothrow) OneWire(pin_num);
  if (!one_wire_) {
    ESP_LOGE(TAG, "FATAL: Heap Exhaustion (OneWire).");
    this->mark_failed();
    return;
  }

  delay(100);

  // Allocate DallasTemperature
  sensors_ = new (std::nothrow) DallasTemperature(one_wire_);
  if (!sensors_) {
    ESP_LOGE(TAG, "FATAL: Heap Exhaustion (DallasTemp).");
    this->mark_failed();
    return;
  }

  // Initialize hardware
  sensors_->begin();
  sensors_->setWaitForConversion(false);

  // Discover devices and set resolution
  uint8_t device_count = sensors_->getDeviceCount();
  ESP_LOGI(TAG, "Found %d device(s) on Pin %u", device_count, pin_num);
  
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
      temp_cache_[addr] = NAN;  // Initialize cache
    }
  }

  // Create mutex
  result_mutex_ = xSemaphoreCreateMutex();
  if (result_mutex_ == NULL) {
    ESP_LOGE(TAG, "FATAL: Mutex creation failed.");
    this->mark_failed();
    return;
  }

  // Create FreeRTOS task
  BaseType_t res;
  #if portNUM_PROCESSORS > 1
    res = xTaskCreatePinnedToCore(this->task_worker, "dallas_hub", 8192, this, 1, &task_handle_, 0);
  #else
    res = xTaskCreate(this->task_worker, "dallas_w", 8192, this, 1, &task_handle_);
  #endif

  if (res != pdPASS) {
    ESP_LOGE(TAG, "FATAL: Worker Task creation failed.");
    this->mark_failed();
  } else {
    ESP_LOGI(TAG, "Async Dallas Hub initialized successfully");
  }
}

void AsyncDallasComponent::dump_config() {
  uint8_t pin_num = pin_->get_pin();
  ESP_LOGCONFIG(TAG, "Async Dallas Hub:");
  ESP_LOGCONFIG(TAG, "  Pin: %u", pin_num);
  ESP_LOGCONFIG(TAG, "  Update Interval: %.1fs", this->get_update_interval() / 1000.0f);
  ESP_LOGCONFIG(TAG, "  Sensors: %d", sensors_list_.size());
}

void AsyncDallasComponent::register_sensor(AsyncDallasSensor *sensor) {
  sensors_list_.push_back(sensor);
}

void AsyncDallasComponent::update() {
  if (this->is_failed() || task_handle_ == nullptr) {
    return;
  }

  if (!request_pending_) {
    request_pending_ = true;
    xTaskNotifyGive(task_handle_);
  }
}

void AsyncDallasComponent::task_worker(void *pvParameters) {
  AsyncDallasComponent *hub = (AsyncDallasComponent *)pvParameters;
  const uint8_t MAX_ERRORS = 10;
  bool just_reset = false;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Bus recovery if needed
    if (hub->error_count_ >= MAX_ERRORS) {
      uint8_t pin_num = hub->pin_->get_pin();
      ESP_LOGW(TAG, "Pin %u: Bus Critical (10x Fail). Resetting Driver...", pin_num);
      
      hub->sensors_->begin();
      hub->sensors_->setWaitForConversion(false);
      
      hub->error_count_ = 0;
      just_reset = true;
    }

    // Request temperatures from all sensors
    hub->sensors_->requestTemperatures();
    vTaskDelay(750 / portTICK_PERIOD_MS);

    // Read all sensors and update cache
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
            if (hub->sensors_->getDS18Count() == 0) {
              ESP_LOGD(TAG, "Device %d: PHYSICAL DISCONNECT", i);
            } else {
              ESP_LOGD(TAG, "Device %d: CRC/NOISE (temp=%.2f)", i, temp);
            }
          } else {
            if (hub->error_count_ > 0) {
              ESP_LOGI(TAG, "Device %d: Signal recovered after %d errors", i, hub->error_count_);
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

  // Trigger sensor updates
  for (auto *sensor : sensors_list_) {
    sensor->update();
  }
  
  request_pending_ = false;
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
    ESP_LOGW(TAG, "'%s': Invalid reading %.2f°C", this->get_name().c_str(), temp);
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
