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

The firmware includes a comprehensive power management system for battery-powered deployments, targeting ~0.2mA in deep sleep.

### Sleep Modes

| Mode | Current | WiFi | CPU | Use Case |
|------|---------|------|-----|----------|
| **Active** | ~80mA | On | Running | Normal operation |
| **Light Sleep** | ~2mA | Associated | Paused | Brief idle periods |
| **Modem Sleep** | ~15mA | RF Off | Running | Processing without network |
| **Deep Sleep** | ~0.2mA | Off | Off | Long idle periods |

### Signal-Based Architecture

PowerService uses **EventBus signals** for decoupled power management:

```
PowerService ──(PowerStateChange)──► EventBus
                                        │
                    ┌───────────────────┼───────────────────┐
                    ▼                   ▼                   ▼
             WiFiService         Pn532Service        (other services)
                    │                   │
                    ▼                   ▼
        enterPowerSleep()         enterSleep()
        wakeFromPowerSleep()      wakeup()
```

Each service subscribes to `PowerStateChange` and manages its own power state.

### Hardware Requirements

| Connection | Purpose |
|------------|--------|
| **GPIO16 (D0) → RST** | Required for timer-based deep sleep wakeup |
| **PN532 IRQ → RST** | Optional, for NFC card wakeup from deep sleep |

### Usage Example

```cpp
// Request deep sleep for 5 minutes
app.getPowerService().requestSleep(PowerState::DeepSleep, 300000);

// Enter modem sleep (WiFi off, CPU running)
app.getPowerService().enterModemSleep();

// Wake from modem sleep
app.getPowerService().wakeFromModemSleep();

// Check wakeup reason after deep sleep
WakeupReason reason = app.getPowerService().getLastWakeupReason();
if (reason == WakeupReason::External) {
    // Woken by PN532 card detection or button
}
```

### Smart Sleep System

PowerService implements an intelligent power management system that automatically selects the optimal sleep mode based on activity patterns, network state, and estimated idle duration.

#### How Smart Sleep Works

```
┌──────────────────────────────────────────────────────────────────────┐
│                      SMART SLEEP DECISION FLOW                       │
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  1. ACTIVITY TRACKING (Configurable via activityTypeMask)            │
│     ┌────────────────────────────────────────────────────┐           │
│     │ Event Type      │ Bitmask │ Resets Idle Timer?     │           │
│     ├─────────────────┼─────────┼────────────────────────┤           │
│     │ CardScanned     │ 0b00001 │ ✓ (default enabled)    │           │
│     │ MqttMessage     │ 0b00010 │ ✓ (default enabled)    │           │
│     │ WifiConnected   │ 0b00100 │ ✓ (default enabled)    │           │
│     │ MqttConnected   │ 0b01000 │ ✗ (default disabled)   │           │
│     │ NfcReady        │ 0b10000 │ ✗ (default disabled)   │           │
│     └────────────────────────────────────────────────────┘           │
│     Default mask: 0b00111 (Card, MQTT msg, WiFi)                     │
│                                                                      │
│  2. IDLE TIMEOUT CHECK (when autoSleepEnabled = true)                │
│     ┌────────────────────────────────────────────────────┐           │
│     │  if (millis() - lastActivity > idleTimeoutMs)      │           │
│     │      → Trigger smart sleep selection               │           │
│     └────────────────────────────────────────────────────┘           │
│                                                                      │
│  3. SLEEP DEPTH SELECTION (if smartSleepEnabled = true)              │
│     ┌────────────────────────────────────────────────────┐           │
│     │  Estimated Idle Duration < 30s?                    │           │
│     │      → LIGHT SLEEP                                 │           │
│     │                                                    │           │
│     │  Estimated Idle Duration 30s - 5m?                 │           │
│     │      MQTT Connected?                               │           │
│     │          YES → LIGHT SLEEP                         │           │
│     │          NO  → MODEM SLEEP (save power)            │           │
│     │                                                    │           │
│     │  Estimated Idle Duration > 5m?                     │           │
│     │      Safe to deep sleep? (no pending ops)          │           │
│     │          YES → DEEP SLEEP                          │           │
│     │          NO  → MODEM SLEEP                         │           │
│     └────────────────────────────────────────────────────┘           │
│                                                                      │
│  4. NETWORK-AWARE SLEEP (if modemSleepOnMqttDisconnect = true)       │
│     ┌────────────────────────────────────────────────────┐           │
│     │  MQTT Disconnected detected                        │           │
│     │      → Auto-enter MODEM SLEEP                      │           │
│     │      → Wake on MQTT reconnect                      │           │
│     │      → Duration: modemSleepDurationMs (30s)        │           │
│     └────────────────────────────────────────────────────┘           │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

#### State Machine Architecture

PowerService follows the same event-driven architecture as MqttService:

```
┌─────────────────────────────────────────────────────────────────────┐
│                     POWERSERVICE STATE MACHINE                      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Uninitialized                                                      │
│       │                                                             │
│       │ begin()                                                     │
│       ▼                                                             │
│  Initializing ─────────────► (detect wakeup, load RTC, check       │
│       │                       chained sleep)                        │
│       │                                                             │
│       ▼                                                             │
│  Ready ◄────────────┐        (waiting for dependencies)            │
│   │                 │                                               │
│   │                 │ WiFi Disconnected                             │
│   │                 │                                               │
│   │ WiFi Connected  │                                               │
│   │ autoSleepEnabled│                                               │
│   ▼                 │                                               │
│  Running ───────────┘        (active power management)             │
│   │                                                                 │
│   ├─► Idle Timeout Check                                           │
│   │   └─► Smart Sleep Selection                                    │
│   │                                                                 │
│   └─► Network-Aware Check                                          │
│       └─► Auto Modem Sleep if MQTT down                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### Non-Blocking Sleep Operations

All sleep modes (except deep sleep) are **non-blocking** and async:

```cpp
// Light Sleep - Non-blocking with timer
void enterLightSleepAsync(uint32_t durationMs) {
    lightSleepActive_ = true;
    lightSleepStartMs_ = millis();
    lightSleepDurationMs_ = durationMs;
    // loop() checks elapsed time and wakes up
}

// Modem Sleep - Non-blocking with timer
void enterModemSleepAsync(uint32_t durationMs) {
    modemSleepActive_ = true;
    modemSleepStartMs_ = millis();
    modemSleepDurationMs_ = durationMs;
    WiFi.setSleep(WIFI_MODEM_SLEEP);
    // loop() wakes after duration or MQTT reconnect
}

// Deep Sleep - Blocking (device resets on wakeup)
void enterDeepSleepAsync(uint32_t durationMs) {
    prepareForSleep(PowerState::DeepSleep);
    saveToRtcMemory();  // Persist metrics
    ESP.deepSleep(durationMs * 1000);  // Device resets
}
```

#### Event-Driven Dependencies

PowerService tracks WiFi and MQTT readiness through events:

```cpp
// Subscribe to WiFi events
bus_.subscribeScoped(EventType::WifiConnected, [this](const Event&) {
    wifiReady_ = true;
    if (autoSleepEnabled && m_state == ServiceState::Ready)
        setState(ServiceState::Running);
});

// Subscribe to MQTT events
bus_.subscribeScoped(EventType::MqttConnected, [this](const Event&) {
    mqttReady_ = true;
    // Wake from modem sleep if active
    if (modemSleepActive_)
        wakeFromModemSleep();
});

bus_.subscribeScoped(EventType::MqttDisconnected, [this](const Event&) {
    mqttReady_ = false;
    // Network-aware: enter modem sleep to save power
});
```

#### PN532 IRQ-Based Wakeup

When `enableNfcWakeup` is enabled, the PN532 can wake the ESP from deep sleep:

```
┌─────────────────────────────────────────────────────────────────┐
│                    IRQ WAKEUP MECHANISM                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. PowerService enters deep sleep                             │
│     ├─► Save metrics to RTC memory                             │
│     ├─► Publish PowerStateChange(DeepSleep)                    │
│     └─► ESP.deepSleep(duration)                                │
│                                                                 │
│  2. Pn532Service receives PowerStateChange                     │
│     ├─► Put PN532 into sleep mode (10µA)                       │
│     ├─► Enable RF field detection wakeup                       │
│     └─► Configure IRQ pin to trigger on card                   │
│                                                                 │
│  3. Card enters NFC field                                      │
│     ├─► PN532 detects RF field                                 │
│     ├─► PN532 triggers IRQ pin                                 │
│     └─► ESP wakes from deep sleep (device reset)               │
│                                                                 │
│  4. PowerService.begin() after wakeup                          │
│     ├─► Detect wakeup reason (External = IRQ)                  │
│     ├─► Restore metrics from RTC memory                        │
│     ├─► Publish WakeupOccurred(External)                       │
│     └─► Resume normal operation                                │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

Hardware Connection: PN532 IRQ → ESP GPIO (configured in nfcWakeupPin)
Power Draw in Sleep: ~0.2mA (ESP) + ~10µA (PN532) ≈ 0.21mA total
```

#### Power Metrics

PowerService tracks comprehensive metrics across sleep cycles:

```cpp
struct PowerServiceMetrics {
    // Sleep cycle tracking
    uint32_t lightSleepCycles{0};
    uint32_t modemSleepCycles{0};
    uint32_t deepSleepCycles{0};

    // Duration tracking
    uint32_t totalLightSleepMs{0};
    uint32_t totalModemSleepMs{0};
    uint32_t totalDeepSleepMs{0};     // Survives deep sleep via RTC

    // Smart decision tracking
    uint32_t idleTimeoutsTriggered{0};
    uint32_t smartSleepDecisions{0};
    uint32_t networkAwareSleeps{0};   // Auto modem sleep on MQTT down
    uint32_t wakeupCount{0};          // Survives deep sleep via RTC
};
```

### Configuration

```cpp
struct PowerConfig {
    // Basic power management
    bool enabled{true};
    uint32_t deepSleepDurationMs{300000};    // 5 minutes
    uint32_t lightSleepDurationMs{10000};    // 10 seconds
    uint32_t idleTimeoutMs{60000};           // Auto-sleep after idle
    bool autoSleepEnabled{false};
    bool enableNfcWakeup{true};
    uint8_t nfcWakeupPin{5};                 // GPIO5 (D1)

    // Smart sleep management
    bool smartSleepEnabled{true};
    bool modemSleepOnMqttDisconnect{true};
    uint32_t modemSleepDurationMs{30000};            // 30 seconds
    uint32_t smartSleepShortThresholdMs{30000};      // <30s = light
    uint32_t smartSleepMediumThresholdMs{300000};    // <5m = modem/light

    // Activity tracking (bitmask: Card|MQTT|WiFi|MqttConn|NfcReady)
    uint8_t activityTypeMask{0b00111};               // Default: Card + MQTT msg + WiFi
};
```

#### Configuration Examples

**Minimal Power (Battery Optimized)**
```json
{
  "power": {
    "autoSleepEnabled": true,
    "smartSleepEnabled": true,
    "idleTimeoutMs": 30000,
    "modemSleepOnMqttDisconnect": true,
    "activityTypeMask": 1,
    "enableNfcWakeup": true
  }
}
```
- Only card scans reset idle timer (mask = 0b00001)
- Enter modem sleep when MQTT down
- Auto deep sleep after 30s idle
- Wake on card detection via IRQ

**Maximum Responsiveness (Mains Powered)**
```json
{
  "power": {
    "autoSleepEnabled": false,
    "smartSleepEnabled": false,
    "activityTypeMask": 31
  }
}
```
- No automatic sleep
- All events tracked (mask = 0b11111)
- Always active for instant response

**Balanced (Default)**
```json
{
  "power": {
    "autoSleepEnabled": true,
    "smartSleepEnabled": true,
    "idleTimeoutMs": 60000,
    "modemSleepOnMqttDisconnect": true,
    "activityTypeMask": 7
  }
}
```
- Card scans, MQTT messages, WiFi events reset timer (mask = 0b00111)
- Smart sleep selection based on duration and network state
- Network-aware modem sleep when MQTT disconnected

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
