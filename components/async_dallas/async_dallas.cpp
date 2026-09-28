#include "async_dallas.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <rom/ets_sys.h>

namespace esphome {
namespace async_dallas {

static const char *TAG = "async_dallas";

// DS18B20 commands
static const uint8_t CMD_SEARCH_ROM      = 0xF0;
static const uint8_t CMD_READ_ROM        = 0x33;
static const uint8_t CMD_MATCH_ROM       = 0x55;
static const uint8_t CMD_SKIP_ROM        = 0xCC;
static const uint8_t CMD_CONVERT_T       = 0x44;
static const uint8_t CMD_READ_SCRATCHPAD = 0xBE;
static const uint8_t CMD_WRITE_SCRATCHPAD= 0x4E;

// ============================================================================
// OneWireBus — raw 1-Wire using ESP-IDF GPIO + ets_delay_us
// ============================================================================

void OneWireBus::begin() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = (1ULL << pin_);
  cfg.mode         = GPIO_MODE_INPUT;
  cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
  reset_search();
}

bool OneWireBus::reset() {
  portDISABLE_INTERRUPTS();
  set_low();
  ets_delay_us(480);
  set_high();
  ets_delay_us(70);
  uint8_t presence = !read_pin();
  portENABLE_INTERRUPTS();
  ets_delay_us(410);
  return presence;
}

void OneWireBus::write_bit(uint8_t bit) {
  portDISABLE_INTERRUPTS();
  if (bit) {
    set_low();
    ets_delay_us(6);
    set_high();
    ets_delay_us(64);
  } else {
    set_low();
    ets_delay_us(60);
    set_high();
    ets_delay_us(10);
  }
  portENABLE_INTERRUPTS();
}

uint8_t OneWireBus::read_bit() {
  portDISABLE_INTERRUPTS();
  set_low();
  ets_delay_us(6);
  set_high();
  ets_delay_us(9);
  uint8_t bit = read_pin();
  portENABLE_INTERRUPTS();
  ets_delay_us(55);
  return bit;
}

void OneWireBus::write_byte(uint8_t byte) {
  for (uint8_t i = 0; i < 8; i++) {
    write_bit(byte & 0x01);
    byte >>= 1;
  }
}

uint8_t OneWireBus::read_byte() {
  uint8_t byte = 0;
  for (uint8_t i = 0; i < 8; i++) {
    byte |= (read_bit() << i);
  }
  return byte;
}

uint8_t OneWireBus::crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < len; i++) {
    uint8_t byte = data[i];
    for (uint8_t j = 0; j < 8; j++) {
      uint8_t mix = (crc ^ byte) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      byte >>= 1;
    }
  }
  return crc;
}

void OneWireBus::reset_search() {
  last_discrepancy_        = 0;
  last_device_flag_        = 0;
  last_family_discrepancy_ = 0;
  memset(rom_, 0, 8);
}

bool OneWireBus::search(uint8_t *address) {
  if (last_device_flag_) {
    reset_search();
    return false;
  }

  if (!reset()) {
    reset_search();
    return false;
  }

  write_byte(CMD_SEARCH_ROM);

  uint8_t id_bit_number   = 1;
  uint8_t last_zero       = 0;
  uint8_t rom_byte_number = 0;
  uint8_t rom_byte_mask   = 1;
  bool    search_result   = false;

  do {
    uint8_t id_bit      = read_bit();
    uint8_t cmp_id_bit  = read_bit();

    if (id_bit == 1 && cmp_id_bit == 1) break;  // no devices

    uint8_t search_direction;
    if (id_bit != cmp_id_bit) {
      search_direction = id_bit;
    } else {
      if (id_bit_number < last_discrepancy_)
        search_direction = ((rom_[rom_byte_number] & rom_byte_mask) > 0);
      else
        search_direction = (id_bit_number == last_discrepancy_);
      if (!search_direction) last_zero = id_bit_number;
    }

    if (search_direction)
      rom_[rom_byte_number] |= rom_byte_mask;
    else
      rom_[rom_byte_number] &= ~rom_byte_mask;

    write_bit(search_direction);

    id_bit_number++;
    rom_byte_mask <<= 1;
    if (!rom_byte_mask) {
      rom_byte_number++;
      rom_byte_mask = 1;
    }
  } while (rom_byte_number < 8);

  if (id_bit_number >= 65) {
    last_discrepancy_ = last_zero;
    if (!last_discrepancy_) last_device_flag_ = 1;
    search_result = true;
  }

  if (!search_result || !rom_[0]) {
    reset_search();
    return false;
  }

  memcpy(address, rom_, 8);
  return true;
}

// ============================================================================
// AsyncDallasComponent
// ============================================================================

void AsyncDallasComponent::setup() {
  ESP_LOGI(TAG, "Async Dallas Setup on GPIO%d", pin_->get_pin());

  pin_->setup();
  uint8_t pin_num = pin_->get_pin();

  bus_ = new OneWireBus(pin_num);
  bus_->begin();

  // Discover devices
  uint8_t addr[8];
  uint8_t index = 0;
  bus_->reset_search();

  while (bus_->search(addr)) {
    if (OneWireBus::crc8(addr, 7) != addr[7]) {
      ESP_LOGW(TAG, "  Device %d: CRC invalid, skipping", index);
      continue;
    }
    uint64_t addr64 = 0;
    for (uint8_t j = 0; j < 8; j++) addr64 |= ((uint64_t)addr[j]) << (j * 8);

    char addr_str[24];
    snprintf(addr_str, sizeof(addr_str), "0x%02X%02X%02X%02X%02X%02X%02X%02X",
      addr[7], addr[6], addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    ESP_LOGI(TAG, "  Device %d: %s", index, addr_str);

    index_to_address_[index] = addr64;
    temp_cache_[addr64] = NAN;
    index++;
  }

  if (index == 0) {
    ESP_LOGW(TAG, "  No devices found — bus will retry");
  }

  // Set resolution for each registered sensor
  for (auto *sensor : sensors_list_) {
    uint64_t addr64 = sensor->get_resolved_address(index_to_address_);
    if (addr64 == 0) continue;
    uint8_t dev_addr[8];
    for (uint8_t j = 0; j < 8; j++) dev_addr[j] = (addr64 >> (j * 8)) & 0xFF;

    uint8_t res_byte;
    switch (sensor->get_resolution()) {
      case 9:  res_byte = 0x1F; break;
      case 10: res_byte = 0x3F; break;
      case 11: res_byte = 0x5F; break;
      default: res_byte = 0x7F; break;  // 12-bit
    }

    if (bus_->reset()) {
      bus_->write_byte(CMD_MATCH_ROM);
      for (uint8_t j = 0; j < 8; j++) bus_->write_byte(dev_addr[j]);
      bus_->write_byte(CMD_WRITE_SCRATCHPAD);
      bus_->write_byte(0x00);  // TH register
      bus_->write_byte(0x00);  // TL register
      bus_->write_byte(res_byte);
      ESP_LOGI(TAG, "  Set resolution %d-bit", sensor->get_resolution());
    }
  }

  result_mutex_ = xSemaphoreCreateMutex();
  if (!result_mutex_) {
    ESP_LOGE(TAG, "FATAL: Mutex creation failed");
    this->mark_failed();
    return;
  }

  BaseType_t res;
#if portNUM_PROCESSORS > 1
  res = xTaskCreatePinnedToCore(task_worker, "dallas_hub", 8192, this, 1, &task_handle_, 0);
#else
  res = xTaskCreate(task_worker, "dallas_hub", 8192, this, 1, &task_handle_);
#endif

  if (res != pdPASS) {
    ESP_LOGE(TAG, "FATAL: Worker task creation failed");
    this->mark_failed();
  } else {
    ESP_LOGI(TAG, "  Worker task started on Core 0");
  }
}

void AsyncDallasComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Async Dallas Hub:");
  ESP_LOGCONFIG(TAG, "  Pin: GPIO%d", pin_->get_pin());
  ESP_LOGCONFIG(TAG, "  Sensors: %d", sensors_list_.size());
  ESP_LOGCONFIG(TAG, "  Update Interval: %dms", update_interval_ms_);
}

void AsyncDallasComponent::register_sensor(AsyncDallasSensor *sensor) {
  sensors_list_.push_back(sensor);
}

// Conversion time per resolution
static uint32_t conversion_time_ms(uint8_t resolution) {
  switch (resolution) {
    case 9:  return 94;
    case 10: return 188;
    case 11: return 375;
    default: return 750;
  }
}

void AsyncDallasComponent::task_worker(void *pvParameters) {
  AsyncDallasComponent *hub = (AsyncDallasComponent *)pvParameters;
  const uint8_t MAX_ERRORS = 10;

  ESP_LOGI(TAG, "Worker task running on Core %d", xPortGetCoreID());

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (hub->is_failed()) {
      vTaskDelete(NULL);
      return;
    }

    // Bus recovery if too many errors
    if (hub->error_count_ >= MAX_ERRORS) {
      ESP_LOGW(TAG, "Bus critical — re-scanning");
      hub->bus_->reset_search();
      hub->index_to_address_.clear();
      uint8_t addr[8];
      uint8_t index = 0;
      while (hub->bus_->search(addr)) {
        uint64_t addr64 = 0;
        for (uint8_t j = 0; j < 8; j++) addr64 |= ((uint64_t)addr[j]) << (j * 8);
        hub->index_to_address_[index++] = addr64;
        if (hub->temp_cache_.find(addr64) == hub->temp_cache_.end())
          hub->temp_cache_[addr64] = NAN;
      }
      hub->error_count_ = 0;
    }

    if (hub->index_to_address_.empty()) {
      // No devices found yet — try a discovery
      hub->bus_->reset_search();
      uint8_t addr[8];
      uint8_t index = 0;
      while (hub->bus_->search(addr)) {
        uint64_t addr64 = 0;
        for (uint8_t j = 0; j < 8; j++) addr64 |= ((uint64_t)addr[j]) << (j * 8);
        hub->index_to_address_[index++] = addr64;
        hub->temp_cache_[addr64] = NAN;
        ESP_LOGI(TAG, "Discovered device %d on retry", index - 1);
      }
      if (hub->index_to_address_.empty()) {
        hub->error_count_++;
        continue;
      }
    }

    // Issue SKIP ROM + CONVERT T (broadcasts to all devices)
    if (!hub->bus_->reset()) {
      hub->error_count_++;
      ESP_LOGW(TAG, "Bus reset failed (error %d)", hub->error_count_);
      continue;
    }
    hub->bus_->write_byte(CMD_SKIP_ROM);
    hub->bus_->write_byte(CMD_CONVERT_T);

    // Wait for conversion — use max resolution time of registered sensors
    uint8_t max_res = 9;
    for (auto *s : hub->sensors_list_) {
      if (s->get_resolution() > max_res) max_res = s->get_resolution();
    }
    vTaskDelay(conversion_time_ms(max_res) / portTICK_PERIOD_MS);

    // Read each device
    if (xSemaphoreTake(hub->result_mutex_, portMAX_DELAY) == pdTRUE) {
      for (auto &kv : hub->index_to_address_) {
        uint64_t addr64 = kv.second;
        uint8_t dev_addr[8];
        for (uint8_t j = 0; j < 8; j++) dev_addr[j] = (addr64 >> (j * 8)) & 0xFF;

        if (!hub->bus_->reset()) {
          hub->error_count_++;
          continue;
        }
        hub->bus_->write_byte(CMD_MATCH_ROM);
        for (uint8_t j = 0; j < 8; j++) hub->bus_->write_byte(dev_addr[j]);
        hub->bus_->write_byte(CMD_READ_SCRATCHPAD);

        uint8_t scratchpad[9];
        for (uint8_t j = 0; j < 9; j++) scratchpad[j] = hub->bus_->read_byte();

        if (OneWireBus::crc8(scratchpad, 8) != scratchpad[8]) {
          ESP_LOGW(TAG, "Scratchpad CRC error");
          hub->error_count_++;
          continue;
        }

        int16_t raw = (scratchpad[1] << 8) | scratchpad[0];
        float temp = raw / 16.0f;

        if (temp < -55.0f || temp > 125.0f) {
          hub->error_count_++;
          hub->temp_cache_[addr64] = NAN;
        } else {
          if (hub->error_count_ > 0) {
            ESP_LOGI(TAG, "Signal recovered after %d errors", hub->error_count_);
          }
          hub->error_count_ = 0;
          hub->temp_cache_[addr64] = temp;
        }
      }
      xSemaphoreGive(hub->result_mutex_);
    }
  }
}

void AsyncDallasComponent::loop() {
  if (this->is_failed()) return;

  uint32_t now = millis();
  if (now - last_update_ < update_interval_ms_) return;
  last_update_ = now;

  if (task_handle_) {
    xTaskNotifyGive(task_handle_);
  }

  for (auto *sensor : sensors_list_) {
    sensor->update(index_to_address_);
  }
}

float AsyncDallasComponent::get_temperature_c(uint64_t address) {
  float result = NAN;
  if (result_mutex_ && xSemaphoreTake(result_mutex_, 0) == pdTRUE) {
    auto it = temp_cache_.find(address);
    if (it != temp_cache_.end()) result = it->second;
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
  return (it != index_map.end()) ? it->second : 0;
}

void AsyncDallasSensor::update(const std::map<uint8_t, uint64_t> &index_map) {
  uint64_t addr = get_resolved_address(index_map);
  if (addr == 0) {
    this->publish_state(NAN);
    return;
  }
  float temp = parent_->get_temperature_c(addr);
  this->publish_state((std::isnan(temp) || temp < -55.0f || temp > 125.0f) ? NAN : temp);
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
