#pragma once

#include "esphome.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <map>
#include <cmath>

namespace esphome {
namespace async_dallas {

class AsyncDallasSensor;  // Forward declaration

class AsyncDallasComponent : public Component {
 public:
  void set_pin(InternalGPIOPin *pin) { pin_ = pin; }
  void set_update_interval(uint32_t ms) { update_interval_ms_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  void register_sensor(AsyncDallasSensor *sensor);
  float get_temperature_c(uint64_t address);

  static void task_worker(void *pvParameters);

 protected:
  InternalGPIOPin *pin_;
  OneWire *one_wire_ = nullptr;
  DallasTemperature *sensors_ = nullptr;
  TaskHandle_t task_handle_ = nullptr;
  SemaphoreHandle_t result_mutex_ = nullptr;

  std::vector<AsyncDallasSensor *> sensors_list_;
  std::map<uint64_t, float> temp_cache_;
  std::map<uint8_t, uint64_t> index_to_address_;  // index → address lookup

  uint32_t update_interval_ms_ = 10000;  // default 10s, overridden by YAML
  uint32_t last_update_ = 0;
  uint8_t error_count_ = 0;
};

class AsyncDallasSensor : public sensor::Sensor {
 public:
  void set_parent(AsyncDallasComponent *parent) { parent_ = parent; }
  void set_address(uint64_t address) { address_ = address; has_address_ = true; }
  void set_index(uint8_t index) { index_ = index; has_address_ = false; }
  void set_resolution(uint8_t resolution) { resolution_ = resolution; }

  void update(const std::map<uint8_t, uint64_t> &index_map);
  void dump_config();

  uint64_t get_address() const { return address_; }
  uint8_t get_index() const { return index_; }
  uint8_t get_resolution() const { return resolution_; }
  bool has_address() const { return has_address_; }

  // Resolve address from index map if needed
  uint64_t get_resolved_address(const std::map<uint8_t, uint64_t> &index_map) const;

 protected:
  AsyncDallasComponent *parent_;
  uint64_t address_{0};
  uint8_t index_{0};
  uint8_t resolution_{12};
  bool has_address_{false};
};

} // namespace async_dallas
} // namespace esphome
