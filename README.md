# ISIC Attendance Reader Firmware

<div align="center">

![Firmware](https://img.shields.io/badge/firmware-1.0.3-0f172a?style=flat-square)
![PlatformIO](https://img.shields.io/badge/build-PlatformIO%206-orange?style=flat-square&logo=platformio)
![C++](https://img.shields.io/badge/language-C%2B%2B17-00599C?style=flat-square&logo=c%2B%2B)
![ESP8266](https://img.shields.io/badge/target-ESP8266-16a34a?style=flat-square&logo=espressif)
![ESP32](https://img.shields.io/badge/target-ESP32-2563eb?style=flat-square&logo=espressif)
![Architecture](https://img.shields.io/badge/architecture-event--driven-7c3aed?style=flat-square)

</div>

Firmware for an ISIC attendance reader built around ESP8266/ESP32, PN532, MQTT, offline buffering, OTA, and smart dual-power behavior for classroom traffic.

## What This Project Provides

- PN532-based ISIC card reading over SPI
- MQTT publishing with offline attendance buffering
- Web-based configuration and OTA update support
- Event-driven service architecture with clear layering
- Smart ESP + PN532 power coordination for real classroom bursts

## Documentation

- [Documentation Hub](docs/README.md)
- [User Manual](docs/USER_MANUAL.md)
- [Developer Manual](docs/DEVELOPER_MANUAL.md)
- [Architecture And Layers](docs/ARCHITECTURE.md) - authoritative deep technical reference
- [MQTT API](docs/MQTT_API.md) - backend-facing topic and payload reference
- [Battery Consumption](docs/BATTERY_CONSUMPTION.md)
- [Memory Optimization](docs/MEMORY_OPTIMIZATION.md)

## System Snapshot

```text
ISIC Card
  -> PN532 Reader
  -> App Coordinator
  -> EventBus
     -> AttendanceService -> MqttService
     -> PowerService -> PN532 Reader
     -> PowerService -> WiFiService
     -> WiFiService -> MqttService
     -> FeedbackService
```

## Runtime Model

- `App` owns service lifecycle and scheduled loops.
- `EventBus` is the core integration boundary between services.
- `PowerService` coordinates ESP power state and PN532 target mode separately.
- `AttendanceService` keeps working even when Wi-Fi is down by buffering records locally.

## Supported Targets

| Environment | Purpose |
|---|---|
| `esp8266` | Primary production target |
| `esp8266_debug` | Debug-oriented ESP8266 build |
| `esp32dev` | Development and validation target |
| `esp32dev_debug` | Debug-oriented ESP32 build |
| `esp32_minimal` | Constraint comparison build |

## Quick Start

```bash
python3 -m platformio run -e esp8266
python3 -m platformio run -e esp32dev
```

Main configuration, types, and services live under [include/](include/) and [src/](src/).

## Repository Layout

```text
include/
  common/      shared config, enums, metrics
  core/        EventBus, Signal, service base contracts
  platform/    ESP-specific adapters
  services/    business/runtime services
src/
  services/    service implementations
docs/          operator, developer, architecture, battery docs
tools/         helper scripts for upload/deploy flows
```

## Project Positioning

This repository is structured like a production firmware project rather than a demo sketch:

- explicit service boundaries
- centralized event bus
- persistent configuration
- offline-safe attendance flow
- documented architecture and operations

## License

[MIT](LICENSE)
