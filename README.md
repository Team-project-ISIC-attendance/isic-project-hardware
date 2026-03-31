# ISIC Attendance System — ESP8266 Firmware

<div align="center">

![C++17](https://img.shields.io/badge/C++-17-00599C?style=flat&logo=c%2B%2B)
![PlatformIO](https://img.shields.io/badge/PlatformIO-6.x-orange?style=flat&logo=platformio)
![ESP8266](https://img.shields.io/badge/ESP8266-ESP12F-green?style=flat&logo=espressif)
![ESP32](https://img.shields.io/badge/ESP32-ESP32DevKit-blue?style=flat&logo=espressif)
![License](https://img.shields.io/badge/License-Proprietary-red)

</div>

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
  - [System Diagram](#system-diagram)
  - [Design Patterns](#design-patterns)
  - [EventBus (Signal/Slot Pattern)](#eventbus-signalslot-pattern)
  - [Service System](#service-system)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Hardware Setup](#hardware-setup)
  - [Building & Flashing](#building--flashing)
- [Configuration](#configuration)
  - [Configuration Structure](#configuration-structure)
  - [LittleFS Persistent Storage](#littlefs-persistent-storage)
  - [Runtime Configuration via MQTT](#runtime-configuration-via-mqtt)
- [MQTT Protocol](#mqtt-protocol)
  - [Topic Structure](#topic-structure)
  - [Message Formats](#message-formats)
- [OTA Updates](#ota-updates)
- [Project Structure](#project-structure)
- [License](#license)

---

## Overview

This firmware implements a complete attendance tracking system for **ESP8266 (ESP-12F)** microcontrollers. When an ISIC card is presented to the PN532 NFC reader, the system records the attendance event, batches multiple events for efficiency, and publishes them to an MQTT broker.

### Key Capabilities

| Capability | Description |
|------------|-------------|
| **NFC Card Reading** | PN532-based ISIC card scanning via SPI |
| **MQTT Integration** | Async publishing with offline buffering |
| **OTA Updates** | Web-based OTA via ElegantOTA |
| **Health Monitoring** | Real-time component health tracking |
| **Event-Driven** | Central EventBus with Signal/Slot pattern |
| **Cooperative Multitasking** | TaskScheduler for non-blocking operation |

---

## Architecture

### System Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              APPLICATION LAYER                              │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                                App                                    │  │
│  │         Main coordinator • Service lifecycle • Task setup             │  │
│  └───────────────────────────────────┬───────────────────────────────────┘  │
├──────────────────────────────────────┼──────────────────────────────────────┤
│                              SERVICE LAYER                                  │
├──────────────────────────────────────┼──────────────────────────────────────┤
│                                      ▼                                      │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                           EventBus                                    │  │
│  │           Signal/Slot pub/sub • Type-safe events • RAII connections   │  │
│  └───────────────────────────────────┬───────────────────────────────────┘  │
│          ┌───────────┬───────────┬───┴───┬───────────┬───────────┐          │
│          ▼           ▼           ▼       ▼           ▼           ▼          │
│  ┌─────────────┐ ┌─────────┐ ┌────────┐ ┌─────────┐ ┌─────────┐ ┌────────┐  │
│  │ConfigService│ │  WiFi   │ │  MQTT  │ │   OTA   │ │PN532    │ │Feedback│  │
│  │• LittleFS   │ │ Service │ │Service │ │ Service │ │Service  │ │Service │  │
│  │• JSON parse │ │• AP mode│ │• Queue │ │• Elegant│ │• SPI    │ │• LED   │  │
│  └─────────────┘ └─────────┘ └────────┘ └─────────┘ └─────────┘ └────────┘  │
│          │                       │           │           │                  │
│          ▼                       ▼           ▼           ▼                  │
│  ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐  │
│  │  AttendanceService  │  │    HealthService    │  │    PowerService     │  │
│  │  • Debounce/batch   │  │  • Component checks │  │  • Sleep modes      │  │
│  │  • Offline buffer   │  │  • MQTT reporting   │  │  • Signal-based     │  │
│  └─────────────────────┘  └─────────────────────┘  └─────────────────────┘  │
│                                                                             │
│                           ┌─────────────────────┐                           │
│                           │   TaskScheduler     │                           │
│                           │  • Cooperative      │                           │
│                           │  • Non-blocking     │                           │
│                           └─────────────────────┘                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                              HARDWARE LAYER                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │   SPI    │  │   WiFi   │  │ LittleFS │  │   GPIO   │  │  Flash   │       │
│  │  (PN532) │  │ (MQTT)   │  │ (Config) │  │(LED/Buzz)│  │  (OTA)   │       │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘  └──────────┘       │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Design Patterns

| Pattern | Implementation | Purpose |
|---------|---------------|---------|
| **Signal/Slot** | `Signal<T>` + `EventBus` | Decoupled event-driven communication |
| **Service Base** | `IService` + `ServiceBase` | Common service lifecycle |
| **Cooperative Tasks** | `TaskScheduler` | Non-blocking multitasking |
| **RAII** | `ScopedConnection` | Auto-unsubscribe on destruction |

---

### EventBus (Signal/Slot Pattern)

The `EventBus` is the central nervous system using a **type-safe Signal/Slot pattern** with per-event-type signals.

#### How It Works

```
┌──────────────┐     publish()      ┌──────────────┐     emit()          ┌──────────────┐
│   Producer   │ ─────────────────► │   EventBus   │ ─────────────────► │  Subscriber  │
│ (Pn532Svc)   │                    │              │                    │ (AttendSvc)  │
└──────────────┘                    │  ┌────────┐  │                    └──────────────┘
                                    │  │Signal  │  │                    ┌──────────────┐
                                    │  │[Card   │  │ ─────────────────► │  Subscriber  │
                                    │  │Scanned]│  │                    │  (Feedback)  │
                                    │  └────────┘  │                    └──────────────┘
                                    │      ...     │
                                    │  Signal per  │
                                    │  EventType   │
                                    └──────────────┘
```

#### Key Features

- **Per-Type Signals**: One `Signal<const Event&>` per `EventType` for O(1) dispatch
- **RAII Connections**: `ScopedConnection` auto-disconnects on destruction
- **Type-Safe**: Strong typing via `std::variant` payloads
- **Memory Efficient**: Fixed-size array of signals, no dynamic allocation

#### Usage Example

```cpp
// 1. Subscribe to events
auto conn = eventBus.subscribe(EventType::CardScanned, 
    [this](const Event& event) {
        auto& cardEvent = std::get<CardEvent>(event.data);
        handleCard(cardEvent);
    });

// 2. Or use scoped connection (auto-unsubscribe)
auto scopedConn = eventBus.subscribeScoped(EventType::MqttConnected,
    [this](const Event& event) {
        onMqttConnected();
    });

// 3. Publish events
eventBus.publish(Event{
    .type = EventType::CardScanned,
    .data = CardEvent{.uid = cardUid, .uidLength = len, .timestampMs = millis()}
});

// 4. Helper for simple events
eventBus.publish(EventType::MqttConnected);
```

#### Event Types

| Domain | Events |
|--------|--------|
| **System** | `SystemReady`, `SystemError`, `ConfigUpdated`, `Heartbeat` |
| **WiFi** | `WiFiConnected`, `WiFiDisconnected`, `WiFiApStarted` |
| **MQTT** | `MqttConnected`, `MqttDisconnected`, `MqttMessage` |
| **NFC** | `CardScanned`, `CardError`, `NfcReady`, `NfcError` |
| **Attendance** | `AttendanceRecorded`, `AttendanceBatchReady` |
| **OTA** | `OtaStarted`, `OtaProgress`, `OtaCompleted`, `OtaError` |
| **Health** | `HealthChanged` |
| **Feedback** | `FeedbackRequest` |
| **Power** | `PowerStateChange`, `SleepRequested`, `WakeupOccurred` |

---

### Service System

All services implement `IService` and optionally `IHealthReporter`. The `ServiceBase` class provides common functionality.

#### Service Lifecycle

```
                    ┌─────────────────┐
                    │  Uninitialized  │
                    └────────┬────────┘
                             │ begin()
                             ▼
                    ┌─────────────────┐
                    │   Initializing  │
                    └────────┬────────┘
                             │ success
                             ▼
                    ┌─────────────────┐
                    │     Running     │ ◄── loop() called by Task
                    └────────┬────────┘
                             │ end() / error
                             ▼
                ┌────────────┴────────────┐
                ▼                         ▼
        ┌──────────┐              ┌──────────┐
        │  Stopped │              │   Error  │
        └──────────┘              └──────────┘
```

#### Services Overview

| Service | Responsibility |
|---------|----------------|
| **ConfigService** | Load/save JSON config from LittleFS |
| **WiFiService** | Station + AP mode, captive portal |
| **MqttService** | MQTT client with queue and reconnect |
| **OtaService** | ElegantOTA web-based updates |
| **Pn532Service** | NFC card reading via SPI |
| **AttendanceService** | Card debounce, batching, offline buffer |
| **FeedbackService** | LED blink and buzzer patterns |
| **HealthService** | Aggregate and report component health |
| **PowerService** | Sleep modes, signal-based power management |

---

## Getting Started

### Prerequisites

| Requirement       | Version | Notes |
|-------------------|---------|-------|
| **PlatformIO**    | 6.x | CLI or VS Code extension |
| **Python**        | 3.8+ | For PlatformIO |
| **ESP-12F Board** | — | NodeMCU or similar |
| **PN532 Module**  | — | SPI mode required |
| **MQTT Broker**   | Any | Mosquitto, HiveMQ, etc. |

### Hardware Setup

#### Pin Configuration (ESP8266)

| Signal | GPIO | PN532 Pin | Notes |
|--------|------|-----------|-------|
| SPI SCK | 14 (D5) | SCK | Clock |
| SPI MISO | 12 (D6) | MISO | Data from PN532 |
| SPI MOSI | 13 (D7) | MOSI | Data to PN532 |
| SPI SS | 15 (D8) | SS | Chip select |
| IRQ | 5 (D1) | IRQ | Card detect |
| RST | 4 (D2) | RSTPDN | Hardware reset |
| LED | 2 | — | Built-in LED (active LOW) |
| Buzzer | 14 (D5) | — | PWM output |

### Building & Flashing

```bash
# Clone the repository
git clone https://github.com/your-org/isic-project-hardware.git
cd isic-project-hardware

# Build for ESP8266 (default)
pio run

# Upload to connected device
pio run -t upload

# Monitor serial output
pio device monitor

# Build + Upload + Monitor
pio run -t upload -t monitor

# Debug build
pio run -e esp8266_debug
```

### Testing with MQTT Broker

For local development and testing, a Docker-based MQTT broker is available:

```bash
# Start local MQTT broker
cd tools/mqtt-broker
docker-compose up -d

# View logs
docker-compose logs -f mosquitto
```

See [tools/mqtt-broker/README.md](tools/mqtt-broker/README.md) for detailed MQTT testing instructions.

---

## Configuration

### Configuration Structure

All configuration is centralized in `AppConfig.hpp`.

### LittleFS Persistent Storage

Configuration is stored in LittleFS as `/config.json` and loaded at boot.

### Runtime Configuration via MQTT

Publish to `<base_topic>/<device_id>/config/set`:

```json
{
  "wifi": {
    "ssid": "NetworkSSID",
    "password": "password123"
  },
  "mqtt": {
    "broker": "mqtt.example.com",
    "port": 1883,
    "username": "user",
    "password": "pass"
  },
  "device": {
    "deviceId": "isic-esp8266-001",
    "locationId": "building-a"
  },
  "attendance": {
    "debounceMs": 2000,
    "batchMaxSize": 10,
    "batchFlushIntervalMs": 30000
  }
}
```

---

## MQTT Protocol

### Topic Structure

All topics follow: `<base_topic>/<device_id>/<resource>[/<action>]`

```
<base_topic>/
└── isic-esp8266-001/
    ├── status              # Online/offline (LWT)
    ├── attendance          # Card events (always array format)
    ├── config/set/#        # Configuration commands (subscribe)
    ├── health              # Health reports
    └── ota/status          # OTA state
```

### Message Formats

#### Attendance Event (Single Card)

```json
[
  {
    "uid": "04A5B7C8D9E0F1",
    "ts": 1699876543210,
    "ts_source": "unix_ms",
    "seq": 1
  }
]
```

> [!NOTE]
> The `attendance` topic always uses the same array format. Single scans produce an array with one record, batched scans produce an array with multiple records.
> `ts` is Unix ms when NTP time is available; otherwise it falls back to uptime milliseconds and `ts_source` is set to `"uptime_ms"`.

#### Health Report

```json
{
  "overall": "healthy",
  "uptimeS": 86400,
  "freeHeapKb": 30,
  "deviceId": "isic-esp8266-001",
  "firmware": "1.0.0",
  "components": [
    {"name": "WiFi", "state": "healthy"},
    {"name": "MQTT", "state": "healthy"},
    {"name": "PN532", "state": "healthy"}
  ]
}
```

---

## OTA Updates

The firmware uses **ElegantOTA** for web-based over-the-air updates.

### Access OTA Interface

1. Connect to device WiFi or ensure device is on same network
2. Navigate to `http://<device_ip>/update`
3. Upload `.bin` firmware file
4. Device reboots automatically after successful update

### OTA via AsyncWebServer

The OTA service runs on port 80 alongside the AsyncWebServer for configuration.

---

## Power Management

The reader now uses a smart dual-power model for passive ISIC scanning. Deep sleep is no longer part of the runtime flow; the ESP and PN532 are coordinated separately so the reader stays responsive during class bursts and only powers each subsystem down when the room is actually quiet.

### Runtime States

| ESP State | WiFi | PN532 Target | Use Case |
|------|------|-------|----------|
| **Active** | Connected when available | `ActiveScan` or `PowerDown` | Normal classroom operation or short quiet gap |
| **Light Sleep** | Associated / power-save | `PowerDown` with IRQ wake | No relevant activity for `30s` |
| **Modem Sleep** | Powered down | `PowerDown` with IRQ wake | No relevant activity for `5 min` or MQTT disconnect |

### Reader Flow

```text
Card scanned
  -> both ESP and PN532 stay fully active for the short ready-hold
  -> the scan is added to the rolling burst window

Several cards in a short window
  -> burst mode turns on
  -> keep WiFi up
  -> keep PN532 in ActiveScan

Quiet gap after the burst
  -> PN532 enters PowerDown first (default: after 10s)
  -> ESP stays Active / connected

Longer quiet gap
  -> ESP enters LightSleep (default: after 30s)

Long quiet period
  -> ESP enters ModemSleep (default: after 5 min)
  -> attendance still buffers locally

First card after idle
  -> PN532 IRQ goes LOW
  -> Pn532Service wakes chip and reads the same card
  -> CardScanned event wakes PowerService to Active and ActiveScan
  -> WiFi reconnects asynchronously if it was in ModemSleep
```

### Service Coordination

`PowerService` still drives power changes through `PowerStateChange` events:

```text
PowerService -> EventBus -> WiFiService / Pn532Service
```

- `WiFiService` keeps station state in `LightSleep` and powers Wi‑Fi down in `ModemSleep`.
- `Pn532Service` can enter `PowerDown` before the ESP does and wakes on card IRQ.
- `PowerService` blocks low-power entry while AP mode is active or OTA is downloading and suppresses sleep while card bursts are active.

### Hardware Notes

| Connection | Purpose |
|------------|---------|
| **PN532 IRQ -> ESP GPIO** | Required for first-card wake in the dual-power idle model |

The PN532 IRQ pin must match both `pn532.irqPin` and `power.nfcWakeupPin`.

### Usage Example

```cpp
// Force the reader into light sleep
app.getPowerService().requestSleep(PowerState::LightSleep);

// Force the reader into modem sleep
app.getPowerService().requestSleep(PowerState::ModemSleep);

// Wake back to active immediately
app.getPowerService().wakeToActive();
```

### Power Configuration

```cpp
struct PowerConfig {
    uint32_t readerIdleTimeoutMs{30000};     // 30 seconds to ESP LightSleep
    uint32_t modemSleepAfterMs{300000};      // 5 minutes to ESP ModemSleep
    uint32_t pn532SleepAfterMs{10000};       // 10 seconds to PN532 PowerDown
    uint32_t readerReadyHoldMs{5000};        // 5 seconds both stay fully active after a read
    uint32_t burstWindowMs{15000};           // 15 second rolling burst window
    uint32_t burstHoldMs{45000};             // 45 seconds to keep burst mode active after last scan
    uint8_t nfcWakeupPin{Pn532Config::kDefaultIrqPin};
    uint8_t burstScanCount{3};               // burst mode after 3 scans in the window
    uint8_t activityTypeMask{0b00001};       // CardScanned only by default
    bool enableNfcWakeup{true};
    bool autoSleepEnabled{false};
    bool disableWiFiDuringSleep{true};
    bool pn532SleepBetweenScans{true};
    bool modemSleepOnMqttDisconnect{true};
};
```

### Example Profiles

**Balanced Classroom**
```json
{
  "power": {
    "autoSleepEnabled": true,
    "pn532SleepAfterMs": 10000,
    "readerReadyHoldMs": 5000,
    "burstWindowMs": 15000,
    "burstScanCount": 3,
    "burstHoldMs": 45000,
    "readerIdleTimeoutMs": 30000,
    "modemSleepAfterMs": 300000,
    "modemSleepOnMqttDisconnect": true,
    "pn532SleepBetweenScans": true,
    "activityTypeMask": 1
  }
}
```
- First-stage PN532 sleep after 10 seconds of quiet
- Short post-scan ready hold keeps both ESP and PN532 hot
- Burst mode keeps WiFi and PN532 fully awake during classroom tap waves
- Full Wi‑Fi power-down after 5 minutes
- Card activity is the only default wake-reset source

**Always Ready**
```json
{
  "power": {
    "autoSleepEnabled": false,
    "activityTypeMask": 31
  }
}
```
- No automatic low-power entry
- Useful for mains-powered or test deployments

### Metrics

`PowerService` tracks:
- `light_sleep_entries`
- `modem_sleep_entries`
- `light_sleep_wakeups`
- `modem_sleep_wakeups`
- `burst_entries`
- `burst_exits`
- `sleep_blocked`
- `sleep_blocked_ap`
- `sleep_blocked_ota`
- `sleep_suppressed_by_burst`

`Pn532Service` tracks:
- `sleep_entries`
- `early_sleep_entries`
- `irq_wakeups`
- `wake_failures`
- `sleep_wake_reads`
- `wake_read_failures`

### Battery Estimates

See [docs/BATTERY_CONSUMPTION.md](/Users/andrian/stu/ing1/1/tp/isic-project-hardware/docs/BATTERY_CONSUMPTION.md) for current estimates, runtime formulas, classroom duty-cycle examples, and a measurement procedure.

---

## Project Structure

```
isic-project-hardware/
├── include/
│   ├── App.hpp                 # Main application coordinator
│   ├── AppConfig.hpp           # Configuration structures
│   ├── core/
│   │   ├── EventBus.hpp        # Central event system
│   │   ├── IService.hpp        # Service interfaces
│   │   ├── Logger.hpp          # Logging utilities
│   │   ├── PlatformMutex.hpp   # Platform-agnostic mutex
│   │   ├── Signal.hpp          # Signal/Slot implementation
│   │   ├── Tagged.hpp          # CRTP tag mixin
│   │   └── Types.hpp           # Type definitions & events
│   └── services/
│       ├── AttendanceService.hpp
│       ├── ConfigService.hpp
│       ├── FeedbackService.hpp
│       ├── HealthService.hpp
│       ├── MqttService.hpp
│       ├── OtaService.hpp
│       ├── Pn532Service.hpp
│       ├── PowerService.hpp
│       └── WiFiService.hpp
├── src/
│   ├── main.cpp                # Entry point
│   ├── App.cpp                 # Application implementation
│   └── services/               # Service implementations
├── platformio.ini              # PlatformIO configuration
└── README.md
```

---

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| [TaskScheduler](https://github.com/arkhipenko/TaskScheduler) | ^3.7.0 | Cooperative multitasking |
| [Adafruit PN532](https://github.com/adafruit/Adafruit-PN532) | ^1.3.4 | NFC reader driver |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | ^7.2.0 | JSON parsing |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | ^2.8.0 | MQTT client |
| [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) | ^3.1.6 | OTA updates |
| [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) | ^1.2.4 | Async HTTP server |

---

## Build Environments

### esp8266 (Production)

```ini
[env:esp8266]
platform = espressif8266@4.2.1
board = esp12e
board_build.f_cpu = 160000000L
build_flags = 
    -std=gnu++2a
    -DISIC_PLATFORM_ESP8266
```

### esp8266_debug (Development)

```ini
[env:esp8266_debug]
extends = env:esp8266
build_flags = 
    ${env:esp8266.build_flags}
    -DISIC_DEBUG=1
    -DDEBUG_ESP_PORT=Serial
build_type = debug
```

---

## License

Proprietary. All rights reserved.
