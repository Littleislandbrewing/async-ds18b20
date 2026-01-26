#pragma once

#include "esphome.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <map>

namespace esphome {
namespace async_dallas {

class AsyncDallasSensor;  // Forward declaration

class AsyncDallasComponent : public PollingComponent {
 public:
  void set_pin(InternalGPIOPin *pin) { pin_ = pin; }
  
  void setup() override;
  void update() override;
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
  std::map<uint64_t, float> temp_cache_;  // address -> temperature
  
  volatile bool request_pending_ = false;
  uint8_t error_count_ = 0;
};

class AsyncDallasSensor : public sensor::Sensor {
 public:
  void set_parent(AsyncDallasComponent *parent) { parent_ = parent; }
  void set_address(uint64_t address) { address_ = address; has_address_ = true; }
  void set_index(uint8_t index) { index_ = index; has_address_ = false; }
  void set_resolution(uint8_t resolution) { resolution_ = resolution; }
  
  void update();
  void dump_config();
  
  uint64_t get_address() const { return address_; }
  uint8_t get_index() const { return index_; }
  bool has_address() const { return has_address_; }

 protected:
  AsyncDallasComponent *parent_;
  uint64_t address_{0};
  uint8_t index_{0};
  uint8_t resolution_{12};
  bool has_address_{false};
};

} // namespace async_dallas
} // namespace esphome
