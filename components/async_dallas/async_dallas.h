#pragma once

#include "esphome.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/components/sensor/sensor.h"
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <map>
#include <vector>
#include <cmath>

namespace esphome {
namespace async_dallas {

class AsyncDallasSensor;

class OneWireBus {
 public:
  explicit OneWireBus(uint8_t pin) : pin_((gpio_num_t)pin) {}
  void begin();
  bool reset();
  void write_byte(uint8_t byte);
  uint8_t read_byte();
  void write_bit(uint8_t bit);
  uint8_t read_bit();
  bool search(uint8_t *address);
  void reset_search();
  static uint8_t crc8(const uint8_t *data, uint8_t len);

 protected:
  gpio_num_t pin_;
  uint8_t last_discrepancy_{0};
  uint8_t last_device_flag_{0};
  uint8_t last_family_discrepancy_{0};
  uint8_t rom_[8]{};

  inline void set_low()     { gpio_set_direction(pin_, GPIO_MODE_OUTPUT); gpio_set_level(pin_, 0); }
  inline void set_high()    { gpio_set_direction(pin_, GPIO_MODE_INPUT); }
  inline uint8_t read_pin() { return (uint8_t)gpio_get_level(pin_); }
};

class AsyncDallasComponent : public Component {
 public:
  void set_pin(InternalGPIOPin *pin) { pin_ = pin; }
  void set_update_interval(uint32_t ms) { update_interval_ms_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  void register_sensor(AsyncDallasSensor *sensor);
  float get_temperature_c(uint64_t address);
  void set_resolution(uint64_t addr64, uint8_t resolution);

  static void task_worker(void *pvParameters);

 protected:
  InternalGPIOPin *pin_{nullptr};
  OneWireBus *bus_{nullptr};

  TaskHandle_t task_handle_{nullptr};
  SemaphoreHandle_t result_mutex_{nullptr};

  std::vector<AsyncDallasSensor *> sensors_list_;
  std::map<uint64_t, float> temp_cache_;
  std::map<uint8_t, uint64_t> index_to_address_;

  uint32_t update_interval_ms_{10000};
  uint32_t last_update_{0};
  uint8_t error_count_{0};
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
  uint64_t get_resolved_address(const std::map<uint8_t, uint64_t> &index_map) const;

 protected:
  AsyncDallasComponent *parent_{nullptr};
  uint64_t address_{0};
  uint8_t index_{0};
  uint8_t resolution_{9};
  bool has_address_{false};
};

} // namespace async_dallas
} // namespace esphome
