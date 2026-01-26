void AsyncDallasSensor::task_worker(void *pvParameters) {
  AsyncDallasSensor *this_sensor = (AsyncDallasSensor *)pvParameters;
  
  // Track consecutive failures
  uint8_t error_count = 0;
  
  // RIGID: 10 failures is the "Debounce" threshold.
  // At 1s intervals, this tolerates a 10s "Noise Storm" (e.g., VFD ramp-up)
  // without triggering a reset. We only reset if the sensor stays dead AFTER the storm.
  const uint8_t MAX_ERRORS = 10;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // --- WORKER LOGIC ---

    // 1. DOCTOR: Check if the sensor is "Latched"
    if (error_count >= MAX_ERRORS) {
        ESP_LOGW(TAG, "Pin %u: Bus Latch-Up detected (10 failures). Performing Hard Reset...", this_sensor->pin_);
        
        // A. Teardown: Clear Software State
        delete this_sensor->sensors_;
        delete this_sensor->one_wire_;
        
        // B. Physical Flush: Drain the line capacitance
        pinMode(this_sensor->pin_, OUTPUT);
        digitalWrite(this_sensor->pin_, LOW);
        vTaskDelay(10 / portTICK_PERIOD_MS); // 10ms "Slap"
        digitalWrite(this_sensor->pin_, HIGH);
        pinMode(this_sensor->pin_, INPUT);
        
        // C. Rebuild: Re-init Driver
        this_sensor->one_wire_ = new OneWire(this_sensor->pin_);
        this_sensor->sensors_ = new DallasTemperature(this_sensor->one_wire_);
        this_sensor->sensors_->begin();
        this_sensor->sensors_->setWaitForConversion(false);
        
        error_count = 0; // Reset counter
    }

    // 2. Request Temp
    this_sensor->sensors_->requestTemperatures();
    
    // Yield (750ms)
    vTaskDelay(750 / portTICK_PERIOD_MS); 

    // 3. Read Temp
    float temp = this_sensor->sensors_->getTempCByIndex(0);

    // 4. Validate Signal Health
    // -127 = Disconnected, 85 = Power-On Reset
    if (temp <= -100 || temp == 85.0) {
        error_count++;
    } else {
        if (error_count > 0) {
             // If we recovered naturally, it was just transient noise.
             ESP_LOGI(TAG, "Pin %u: Signal recovered (Transient Noise).", this_sensor->pin_);
        }
        error_count = 0;
    }

    // 5. Deliver Result
    if (xSemaphoreTake(this_sensor->result_mutex_, portMAX_DELAY) == pdTRUE) {
      this_sensor->latest_temp_ = temp;
      xSemaphoreGive(this_sensor->result_mutex_);
    }
  }
}
