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
  ESP_LOGI(TAG, "Async Dallas Setup");

  pin_->setup();
  uint8_t pin_num = pin_->get_pin();
  ESP_LOGI(TAG, "  Pin: %u", pin_num);

  delay(100);

  one_wire_ = new (std::nothrow) OneWire(pin_num);
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

  sensors_->begin();
  sensors_->setWaitForConversion(false);

  // Discover devices and cache their addresses by index
  uint8_t device_count = sensors_->getDeviceCount();
  ESP_LOGI(TAG, "  Found %d device(s) on pin %u", device_count, pin_num);

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

      // Store address by index for index-based lookup
      index_to_address_[i] = addr;
      temp_cache_[addr] = NAN;
    }
  }

  // Apply resolution to each registered sensor
  for (auto *sensor : sensors_list_) {
    uint64_t addr = sensor->get_resolved_address(index_to_address_);
    if (addr != 0) {
      DeviceAddress device_addr;
      for (uint8_t j = 0; j < 8; j++) {
        device_addr[j] = (addr >> (j * 8)) & 0xFF;
      }
      sensors_->setResolution(device_addr, sensor->get_resolution());
      ESP_LOGI(TAG, "  Set resolution %d-bit for sensor", sensor->get_resolution());
    }
  }

  result_mutex_ = xSemaphoreCreateMutex();
  if (result_mutex_ == NULL) {
    ESP_LOGE(TAG, "FATAL: Mutex creation failed.");
    this->mark_failed();
    return;
  }

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
    ESP_LOGI(TAG, "  Worker task started on Core 0");
  }
}

void AsyncDallasComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Async Dallas Hub:");
  ESP_LOGCONFIG(TAG, "  Pin: %u", pin_->get_pin());
  ESP_LOGCONFIG(TAG, "  Sensors: %d", sensors_list_.size());
}

void AsyncDallasComponent::register_sensor(AsyncDallasSensor *sensor) {
  sensors_list_.push_back(sensor);
}

void AsyncDallasComponent::task_worker(void *pvParameters) {
  AsyncDallasComponent *hub = (AsyncDallasComponent *)pvParameters;
  const uint8_t MAX_ERRORS = 10;
  bool just_reset = false;

  ESP_LOGI(TAG, "Worker task running on Core %d", xPortGetCoreID());

  for (;;) {
    // Wait for notification from loop()
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (hub->error_count_ >= MAX_ERRORS) {
      ESP_LOGW(TAG, "Bus Critical. Resetting driver...");
      hub->sensors_->begin();
      hub->sensors_->setWaitForConversion(false);
      hub->error_count_ = 0;
      just_reset = true;
    }

    // Request conversion on all devices
    hub->sensors_->requestTemperatures();

    // Wait for conversion — use longest resolution time (750ms for 12-bit)
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

          if (temp < -50.0f || temp > 125.0f) {
            hub->error_count_++;
            ESP_LOGW(TAG, "Device %d: Out of range (%.1f°C), error count: %d", i, temp, hub->error_count_);
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

  uint32_t now = millis();
  if (now - last_update_ < update_interval_ms_) return;
  last_update_ = now;

  // Trigger worker task
  if (task_handle_ != nullptr) {
    xTaskNotifyGive(task_handle_);
  }

  // Deliver readings to sensors
  for (auto *sensor : sensors_list_) {
    sensor->update(index_to_address_);
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
// AsyncDallasSensor
// ============================================================================

uint64_t AsyncDallasSensor::get_resolved_address(const std::map<uint8_t, uint64_t> &index_map) const {
  if (has_address_) return address_;
  auto it = index_map.find(index_);
  if (it != index_map.end()) return it->second;
  return 0;
}

void AsyncDallasSensor::update(const std::map<uint8_t, uint64_t> &index_map) {
  uint64_t addr = get_resolved_address(index_map);
  if (addr == 0) {
    ESP_LOGW(TAG, "Sensor index %d not found on bus", index_);
    this->publish_state(NAN);
    return;
  }

  float temp = parent_->get_temperature_c(addr);

  if (std::isnan(temp) || temp < -50.0f || temp > 125.0f) {
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
  ESP_LOGCONFIG(TAG, "  Resolution: %d-bit", resolution_);
}

} // namespace async_dallas
} // namespace esphome
