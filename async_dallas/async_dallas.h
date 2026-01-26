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
  
  // FIX #5: Destructor to clean up resources on shutdown/OTA
  ~AsyncDallasSensor() {
    if (task_handle_ != NULL) {
      vTaskDelete(task_handle_);
      task_handle_ = NULL;
    }
    if (result_mutex_ != NULL) {
      vSemaphoreDelete(result_mutex_);
      result_mutex_ = NULL;
    }
    // Clean up pointers
    if (sensors_) delete sensors_;
    if (one_wire_) delete one_wire_;
  }

  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;

  static void task_worker(void *pvParameters);

 protected:
  uint8_t pin_;
  OneWire *one_wire_ = nullptr;
  DallasTemperature *sensors_ = nullptr;
  TaskHandle_t task_handle_ = NULL;
  SemaphoreHandle_t result_mutex_ = NULL;
  
  volatile bool request_pending_ = false;
  float latest_temp_ = NAN;
};

} // namespace async_dallas
} // namespace esphome
