# Developer Manual

## Overview

This firmware is organized as a service-based embedded application with a central event bus. The codebase is intended to remain maintainable as features evolve across connectivity, attendance processing, OTA, health, and power behavior.

## Tooling

- PlatformIO
- Arduino framework
- C++17
- ESP8266 and ESP32 targets

## Build Targets

| Environment | Purpose |
|---|---|
| `esp8266` | main target |
| `esp8266_debug` | debug build |
| `esp32dev` | secondary validation target |
| `esp32dev_debug` | debug build |
| `esp32_minimal` | constrained comparison build |

## Common Commands

```bash
python3 -m platformio run -e esp8266
python3 -m platformio run -e esp32dev
python3 -m platformio run -e esp8266_debug
python3 -m platformio run -e esp32dev_debug
```

## Repository Structure

```text
include/
  common/      shared configuration, enums, metrics
  core/        EventBus, Signal, IService
  platform/    platform-specific wrappers
  services/    public service interfaces
  utils/       utility helpers
src/
  App.cpp      main application coordinator
  main.cpp     firmware entry point
  services/    service implementations
docs/          manuals and references
tools/         upload and helper scripts
```

## Core Design Rules

- Prefer event-driven coordination over tight service coupling.
- Keep service responsibilities narrow and explicit.
- Preserve existing logs and comments unless there is a strong reason to change them.
- Avoid broad rewrites when a local fix or local extension is enough.
- Keep the ESP and PN532 power policies coordinated but independently reasoned about.

## Main Runtime Components

| Component | Responsibility |
|---|---|
| `App` | owns service lifecycle and scheduler tasks |
| `EventBus` | decouples runtime communication |
| `ConfigService` | persistent JSON-backed configuration |
| `WiFiService` | station/AP mode and web server responsibilities |
| `MqttService` | broker connection and MQTT traffic |
| `Pn532Service` | NFC reader control and wake/read flow |
| `AttendanceService` | debounce, batch, offline buffering |
| `PowerService` | smart dual-power coordination |
| `HealthService` | health aggregation and reporting |
| `FeedbackService` | LED/buzzer patterns |
| `OtaService` | OTA workflow |

## Configuration Model

The central configuration type lives in [include/common/Config.hpp](../include/common/Config.hpp).

Key points:

- `ConfigService` serializes/deserializes the full config tree
- runtime behavior depends heavily on `power`, `pn532`, `wifi`, `mqtt`, and `attendance`
- power policy now includes smart-idle knobs for burst detection and PN532-first sleep behavior

## Power Model

The firmware uses a dual-domain power strategy:

- ESP state is represented by `PowerState`
- PN532 target behavior is represented separately
- `PowerService` decides both targets
- `WiFiService` reacts to ESP state changes
- `Pn532Service` reacts to the PN532 target included in power events

This is important: the reader may keep the ESP fully awake while PN532 sleeps, or keep both fully awake during a scan burst.

## Safe Change Areas

Good isolated change points:

- `PowerService` for policy decisions
- `Pn532Service` for reader sleep/wake behavior
- `AttendanceService` for queueing and flush policy
- `ConfigService` for new config fields

Be more careful in:

- `Types.hpp` because it changes shared enums/payloads/metrics
- `WiFiService` because it affects AP mode and reconnect behavior
- platform wrappers because they affect both boards

## Verification Expectations

Before closing a firmware change, at minimum:

1. Build `esp8266`
2. Build `esp32dev`
3. Check that changed config fields serialize and deserialize
4. Review any event payload changes for all subscribers
5. Verify documentation if public behavior changed

## Extension Guidelines

When adding a feature:

- prefer adding one clear event over hidden cross-service calls
- keep changes local to the smallest possible subsystem
- extend metrics when the runtime behavior becomes harder to observe
- document operator-facing behavior in the user manual
- document system-facing behavior in the architecture guide

## Related Docs

- [Architecture And Layers](ARCHITECTURE.md)
- [Battery Consumption](BATTERY_CONSUMPTION.md)
- [Memory Optimization](MEMORY_OPTIMIZATION.md)
