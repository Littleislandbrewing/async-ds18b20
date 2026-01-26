#pragma once

#include "esphome.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace esphome {
namespace async_dallas {

class AsyncDallasSensor : public PollingComponent, public sensor::Sensor {
 public:
  AsyncDallasSensor(uint8_t pin, uint32_t interval);

  // NOTE: Destructors are rarely called in ESPHome lifecycle.
  // We rely on "Fail-Closed" logic instead of cleanup.

  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;

  static void task_worker(void *pvParameters);

 protected:
  uint8_t pin_;
  OneWire *one_wire_ = nullptr;
  DallasTemperature *sensors_ = nullptr;
  TaskHandle_t task_handle_ = nullptr;
  SemaphoreHandle_t result_mutex_ = nullptr;
  
  volatile bool request_pending_ = false;
  float latest_temp_ = NAN;
};

} // namespace async_dallas
} // namespace esphome
