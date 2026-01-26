Async DS18B20 for ESPHome
A High-Performance, Non-Blocking, Multi-Threaded Custom Component for ESP32.

Overview
The standard dallas component in ESPHome is blocking. The DS18B20 sensor requires approximately 750ms to convert a temperature reading at 12-bit resolution. During this time, the standard component pauses the main loop to wait for the result or blocks interrupts to handle the 1-Wire timing.

On a high-speed PLC or industrial controller, a 750ms lag is unacceptable. It causes:

Jitter in PID control loops.

Delayed response to physical buttons and safety inputs.

Network packet drops (WiFi/Ethernet latency).

Watchdog warnings.

This component solves this by offloading the sensor communication to a separate FreeRTOS task, leveraging the ESP32's multi-core architecture.

Architecture
Core 0 Offloading: Spawns a FreeRTOS Worker Task pinned strictly to Core 0 (the "Pro" core) on dual-core devices.

Zero-Impact Loop: The Main Loop (Core 1) sends a "Start" signal and immediately continues processing logic. It does not wait.

Background Processing: The worker task handles the heavy 1-Wire bit-banging and the 750ms conversion delay.

Thread-Safe Delivery: Data is passed back to Core 1 via a Mutex-protected semaphore.

Performance Impact: Loop time drops from ~140ms+ (spiking) to <15ms (flat), even while reading sensors at 1Hz.

Universal ESP32 Support
This component automatically detects chip architecture at runtime:

Dual Core (ESP32-S3, WROOM): Pins the worker to Core 0 for maximum performance.

Single Core (ESP32-C3, S2, Solo): Spawns a standard unpinned task. While it cannot offload to a second core, it still uses FreeRTOS delays to prevent blocking the main loop during the conversion phase.

Installation
Add the following to your ESPHome YAML configuration to pull the component directly from GitHub:

YAML Code:

YAML
external_components:
  - source: github://Littleislandbrewing/async-ds18b20
    components: [ async_dallas ]
    refresh: 0s
Configuration
This component replaces the standard dallas platform. Note: It assumes a Bus Topology where you have one sensor per GPIO pin (common in industrial carrier boards). It reads Index 0 on the wire.

Basic Example
YAML Code:

YAML
sensor:
  - platform: async_dallas
    pin: 48
    name: "HLT Temp"
    update_interval: 1s  # Safe to run at high frequency
Configuration Variables
pin (Required): The GPIO pin connected to the DS18B20 data line.

name (Required): The name of the sensor in Home Assistant.

update_interval (Optional): How often to poll the sensor. Default: 10s.

id (Optional): Manually set the ESPHome ID.

Technical Safety Features
This component is engineered for industrial reliability ("RIGID" protocol):

Deadlock Prevention: The "Pending" flag is cleared even on sensor read errors, ensuring the loop never freezes waiting for a response that will never come.

NaN Safety: If the sensor is disconnected (reads -127), the component publishes NAN (Unavailable) instead of holding the last known value. This ensures Safety Logic (Fail-Safe) triggers immediately.

Watchdog Compliance: Uses vTaskDelay to yield to the OS during conversion, preventing Task Watchdog (TWDT) resets.

Thread Safety: Uses Mutex locks (xSemaphoreTake) to prevent race conditions during data handover between cores.

License
MIT License. Free to use for personal or commercial projects.
