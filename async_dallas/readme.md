# Async DS18B20 for ESPHome

**A High-Performance, Non-Blocking, Multi-Threaded Custom Component for ESP32.**

[![ESPHome](https://img.shields.io/badge/ESPHome-2024.12.0+-blue.svg)](https://esphome.io)
[![Platform](https://img.shields.io/badge/Platform-ESP32-green.svg)](https://esphome.io)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

## Overview

The standard `dallas` component in ESPHome is **blocking**. The DS18B20 sensor requires approximately 750ms to convert a temperature reading at 12-bit resolution. During this time, the standard component pauses the main loop to wait for the result or blocks interrupts to handle the 1-Wire timing.

**On a high-speed PLC or industrial controller, a 750ms lag is unacceptable.** It causes:
* Jitter in PID control loops.
* Delayed response to physical buttons and safety inputs.
* Network packet drops (WiFi/Ethernet latency).
* Watchdog warnings.

This component solves this by offloading the sensor communication to a separate FreeRTOS task, leveraging the ESP32's multi-core architecture.

## Architecture (Nuclear-Hard)

This component is engineered for **Industrial Reliability** ("RIGID" Protocol). It avoids common embedded pitfalls like heap fragmentation and race conditions.

1.  **Core 0 Offloading:** Spawns a FreeRTOS Worker Task pinned strictly to **Core 0** (the "Pro" core) on dual-core devices. Core 1 (Main Loop) is left 100% free for logic.
2.  **Static Allocation:** All memory objects are allocated **once** at boot. The component strictly forbids dynamic `delete`/`new` cycling during runtime to prevent Heap Fragmentation.
3.  **Thread-Safe Delivery:** Data is passed back to Core 1 via a Mutex-protected semaphore.
4.  **Physics Enforcement:** The component explicitly forces the sensor to **12-bit resolution** to ensure the 750ms async delay matches the hardware reality.

**Performance Impact:** Loop time drops from **~140ms+** (spiking) to **<15ms** (flat), even while reading sensors at 1Hz.

## "The Bus Doctor" (Self-Healing Logic)

In industrial environments (like breweries), heavy loads (VFDs, Solenoids) can introduce EMI spikes that cause the DS18B20 to **"Latch Up"** (freeze) or the software driver to desync.

This component includes a **Bus Doctor** routine:
1.  **Debounce Filter:** It tolerates up to **10 consecutive failures** (10 seconds) to filter out transient noise storms (e.g., a VFD ramping up).
2.  **Surgical Reset:** If errors persist beyond 10 seconds, it triggers a **Soft Re-Sync**. It forces the 1-Wire library to re-scan the bus and reset its internal state machine *without* destroying memory objects.
3.  **Brewery Safe (85°C Rule):** Unlike standard drivers, this component **DOES NOT** treat `85.0°C` (Power-On Reset value) as an error. In brewing, 85°C is a valid temperature (Sparge/Mash-out). We trust the reading to prevent automation failure at critical process steps.

## Installation

Add the following to your ESPHome YAML configuration to pull the component directly from GitHub:

```yaml
external_components:
  - source: github://Littleislandbrewing/async-ds18b20
    components: [ async_dallas ]
    refresh: 0s
```
**Performance Impact:** Loop time drops from **~140ms+** (spiking) to **<15ms** (flat), even while reading sensors at 1Hz.

### Universal ESP32 Support
This component automatically detects chip architecture at runtime BUT is not really supported for single core ESP32 chips:
* **Dual Core (ESP32-S3, WROOM):** Pins the worker to **Core 0** for maximum performance.
* **Single Core (ESP32-C3, S2, Solo):** Spawns a standard unpinned task. While it cannot offload to a second core, it still uses FreeRTOS delays to prevent blocking the main loop during the conversion phase.

  

Configuration
This component replaces the standard dallas platform. Note: It assumes a Bus Topology where you have one sensor per GPIO pin (common in industrial carrier boards) so therefore DOES NOT need an address specification. 1 sensor per GPIO. It reads Index 0 on the wire.

Basic Example:
```
YAML
sensor:
  - platform: async_dallas
    pin: 48
    name: "HLT Temp"
    update_interval: 1s  # Safe to run at high frequency
```

Configuration Variables

pin (Required): The GPIO pin connected to the DS18B20 data line.
name (Required): The name of the sensor in Home Assistant.
update_interval (Optional): How often to poll the sensor. Default: 1s
id (Optional): Manually set the ESPHome ID.

Technical Safety Features:
This component is engineered for industrial reliability with a caveat to do with timing: ("RIGID" protocol):

Deadlock Prevention: The "Pending" flag is cleared even on sensor read errors, ensuring the loop never freezes waiting for a response that will never come.

NaN Safety: If the sensor is disconnected (reads -127), the component publishes NAN (Unavailable) instead of holding the last known value. This ensures Safety Logic (Fail-Safe) triggers immediately.

Watchdog Compliance: Uses vTaskDelay to yield to the OS during conversion, preventing Task Watchdog (TWDT) resets.

Thread Safety: Uses Mutex locks (xSemaphoreTake) to prevent race conditions during data handover between cores.

One more note that is SUPER important. - THIS CODE IS NOT PROVIDED AS ANYTHING MORE THAN AN ALPHA RELEASE. TEST EVERYTHING. IT IS SPECIFIC TO MY SETUP AND MAY NOT BE APPLICABLE TO YOURS. I PROVIDE NO WARRANTY OR GUARANTEE OF HOW IT WORKS FOR YOU.... IT IS TOTALLY YOUR RESPONSIBILITY TO DO YOUR OWN DUE DILLIGENCE AND TESTING.

License
MIT License. Free to use for personal or commercial projects.
