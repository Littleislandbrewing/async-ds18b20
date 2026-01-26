Here is the comprehensive, production-ready `README.md`. It includes the "Nuclear-Hard" architecture details, the specific "Bus Doctor" logic, and a dedicated section on how to interpret the forensic logs during deployment.

---

# Async DS18B20 for ESPHome

**A High-Performance, Non-Blocking, Multi-Threaded Custom Component for ESP32.**

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

1. **Core 0 Offloading:** Spawns a FreeRTOS Worker Task pinned strictly to **Core 0** (the "Pro" core) on dual-core devices. Core 1 (Main Loop) is left 100% free for logic.
2. **Static Allocation:** All memory objects are allocated **once** at boot. The component strictly forbids dynamic `delete`/`new` cycling during runtime to prevent Heap Fragmentation.
3. **Thread-Safe Delivery:** Data is passed back to Core 1 via a Mutex-protected semaphore.
4. **Physics Enforcement:** The component explicitly forces the sensor to **12-bit resolution** to ensure the 750ms async delay matches the hardware reality.

**Performance Impact:** Loop time drops from **~140ms+** (spiking) to **<15ms** (flat), even while reading sensors at 1Hz.

## "The Bus Doctor" (Self-Healing Logic)

In industrial environments (like breweries), heavy loads (VFDs, Solenoids) can introduce EMI spikes that cause the DS18B20 to **"Latch Up"** (freeze) or the software driver to desync.

This component includes a **Bus Doctor** routine:

1. **Debounce Filter:** It tolerates up to **10 consecutive failures** (10 seconds) to filter out transient noise storms (e.g., a VFD ramping up).
2. **Surgical Reset:** If errors persist beyond 10 seconds, it triggers a **Soft Re-Sync**. It forces the 1-Wire library to re-scan the bus and reset its internal state machine *without* destroying memory objects.
3. **Brewery Safe (85°C Rule):** Unlike standard drivers, this component **DOES NOT** treat `85.0°C` (Power-On Reset value) as an error. In brewing, 85°C is a valid temperature (Sparge/Mash-out). We trust the reading to prevent automation failure at critical process steps.

## Installation

Add the following to your ESPHome YAML configuration to pull the component directly from GitHub:

```yaml
external_components:
  - source: github://Littleislandbrewing/async-ds18b20
    components: [ async_dallas ]
    refresh: 0s

```

## Configuration

This component replaces the standard `dallas` platform.
**Constraint:** It enforces a **1-Pin-1-Sensor** topology. You cannot daisy-chain multiple sensors on a single pin.

### Basic Example

```yaml
sensor:
  - platform: async_dallas
    pin: 48
    name: "HLT Temp"
    # Default is 1s (Safe for 12-bit in liquid).
    # Increase to 10s/60s for air sensors to prevent self-heating.
    update_interval: 1s  

```

### Configuration Variables

| Variable | Description | Required | Default |
| --- | --- | --- | --- |
| `pin` | The GPIO pin connected to the DS18B20 data line. | **Yes** | - |
| `name` | The name of the sensor in Home Assistant. | **Yes** | - |
| `update_interval` | How often to poll the sensor. | No | `1s` |

## Troubleshooting & Logs

This component includes "Forensic Logging" to help diagnose physical layer issues (especially on 3.3V setups like Kincony controllers).

To see detailed diagnostics, enable debug logging for this component only:

```yaml
logger:
  level: INFO
  logs:
    async_dallas: DEBUG

```

### Log Signature Guide

| Log Message | Meaning | Action Required |
| --- | --- | --- |
| `PHYSICAL DISCONNECT` | The CPU asked for data, but no sensor replied. | **Check Wiring.** Wire is broken, screw terminal is loose, or sensor is unplugged. |
| `CRC/NOISE` | The sensor replied, but data was garbage. | **Check Power.** Typical of 3.3V "Brownouts". Install a 100nF capacitor at the sensor. |
| `OUT OF RANGE` | Sensor returned impossible data (e.g. -196.00). | **Check Logic Logic.** Likely a voltage drop causing logic errors. |
| `Bus Critical ... Resetting Driver` | The sensor has been dead for 10 seconds. | **System Intervention.** The "Bus Doctor" is attempting to revive the sensor. |
| `Signal recovered after X errors` | The sensor came back online. | **Success.** The self-healing logic worked. |
| `stuck at 9-bit! ... CHECK POWER` | Setup check failed. | **Critical Power Failure.** The sensor voltage is too low to accept config commands. |

## Technical Safety Features

* **NaN Safety:** If the sensor is disconnected (reads -127 or enters invalid range), the component publishes `NAN` (Unavailable). This ensures Safety Logic (Fail-Safe) triggers immediately.
* **Watchdog Compliance:** Uses `vTaskDelay` to yield to the OS during conversion, preventing Task Watchdog (TWDT) resets.
* **Stack Safety:** The Worker Task is allocated a generous **8KB (8192 bytes)** stack to prevent overflows during logging or exception handling.
* **Guard Rails:** The `update()` loop checks for task validity before execution to prevent null-pointer dereferencing during startup or failure states.

## License

MIT License. Free to use for personal or commercial projects.
