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

  void setup() override;
  void update() override; // Core 1 Trigger
  void loop() override;   // Core 1 Collector
  void dump_config() override;

  // The Worker Task (Runs on Core 0)
  static void task_worker(void *pvParameters);

 protected:
  uint8_t pin_;
  OneWire *one_wire_;
  DallasTemperature *sensors_;
  TaskHandle_t task_handle_;
  SemaphoreHandle_t result_mutex_;
  
  bool request_pending_ = false;
  float latest_temp_ = NAN;
};

} // namespace async_dallas
} // namespace esphome
