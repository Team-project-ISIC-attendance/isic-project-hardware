# ISIC Attendance System — ESP32 Hardware Firmware

<div align="center">

![C++20](https://img.shields.io/badge/C++-20-00599C?style=flat&logo=c%2B%2B)
![PlatformIO](https://img.shields.io/badge/PlatformIO-6.x-orange?style=flat&logo=platformio)
![ESP32](https://img.shields.io/badge/ESP32-Supported-green?style=flat&logo=espressif)
![ESP8266](https://img.shields.io/badge/ESP8266-Supported-green?style=flat&logo=espressif)
![License](https://img.shields.io/badge/License-Proprietary-red)

**Production-grade embedded firmware for ISIC card-based attendance tracking with NFC, MQTT, OTA updates, and power management.**

</div>

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Architecture](#architecture)
  - [System Diagram](#system-diagram)
  - [Design Patterns](#design-patterns)
  - [EventBus (Observer Pattern)](#eventbus-observer-pattern)
  - [Module System (Strategy Pattern)](#module-system-strategy-pattern)
  - [State Machine Pattern](#state-machine-pattern)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Hardware Setup](#hardware-setup)
  - [Building & Flashing](#building--flashing)
- [Target Platforms](#target-platforms)
  - [ESP32 Development Kit](#esp32-development-kit)
  - [ESP-12F (ESP8266)](#esp-12f-esp8266)
  - [Conditional Compilation](#conditional-compilation)
- [Configuration](#configuration)
  - [Configuration Structure](#configuration-structure)
  - [NVS Persistent Storage](#nvs-persistent-storage)
  - [Runtime Configuration via MQTT](#runtime-configuration-via-mqtt)
- [MQTT Protocol](#mqtt-protocol)
  - [Topic Structure](#topic-structure)
  - [Subscriptions (Device Listens)](#subscriptions-device-listens)
  - [Publications (Device Publishes)](#publications-device-publishes)
  - [Message Formats](#message-formats)
- [OTA Updates](#ota-updates)
  - [Partition Layout](#partition-layout)
  - [OTA State Machine](#ota-state-machine)
  - [OTA Commands](#ota-commands)
  - [Boot Validation & Rollback](#boot-validation--rollback)
- [Power Management](#power-management)
- [Health Monitoring](#health-monitoring)
- [Code Style & Standards](#code-style--standards)
- [Project Structure](#project-structure)
- [License](#license)

---

## Overview

This firmware implements a complete attendance tracking system for ESP32/ESP8266 microcontrollers. When an ISIC card is presented to the NFC reader, the system records the attendance event, batches multiple events for efficiency, and publishes them to an MQTT broker. The firmware is designed for production deployments with features like OTA updates, automatic rollback, power management, and comprehensive health monitoring.

### Key Capabilities

| Capability | Description |
|------------|-------------|
| **NFC Card Reading** | PN532-based ISIC card scanning with interrupt-driven wake |
| **MQTT Integration** | Async publishing with offline buffering and backpressure |
| **Power Management** | Light/deep sleep with wake-lock mechanism |
| **OTA Updates** | Dual-partition updates with automatic rollback |
| **Health Monitoring** | Real-time component health tracking and reporting |
| **Event-Driven** | Central EventBus for decoupled communication |

---

## Features

- **High Throughput**: Non-blocking design handles 120+ cards/minute
- **Offline Resilience**: 256-event buffer for network outages
- **Smart Batching**: Configurable batching reduces MQTT overhead by 10x
- **Debouncing**: Prevents duplicate scans (configurable 2s default)
- **Wake-on-Card**: PN532 interrupt wakes ESP32 from sleep
- **Exponential Backoff**: Graceful recovery from network failures
- **Metrics & Telemetry**: Real-time performance monitoring

---

## Architecture

### System Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              APPLICATION LAYER                              │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐          │
│  │AttendanceModule │    │   OtaModule     │    │  (Future Mods)  │          │
│  │  • Debounce     │    │  • MQTT handler │    │                 │          │
│  │  • Validation   │    │  • State mgmt   │    │                 │          │
│  └────────┬────────┘    └────────┬────────┘    └────────┬────────┘          │
│           │                      │                      │                   │
│           └──────────────────────┼──────────────────────┘                   │
│                                  ▼                                          │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                           ModuleManager                               │  │
│  │            Lifecycle management • Event routing • Config updates      │  │
│  └───────────────────────────────────┬───────────────────────────────────┘  │
├──────────────────────────────────────┼──────────────────────────────────────┤
│                              SERVICE LAYER                                  │
├──────────────────────────────────────┼──────────────────────────────────────┤
│                                      ▼                                      │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                             EventBus                                  │  │
│  │     Thread-safe pub/sub • Priority queues • O(1) event filtering      │  │
│  └───────────────────────────────────┬───────────────────────────────────┘  │
│           ┌──────────────┬───────────┼───────────┬──────────────┐           │
│           ▼              ▼           ▼           ▼              ▼           │
│  ┌─────────────┐ ┌─────────────┐ ┌────────┐ ┌─────────┐ ┌─────────────────┐ │
│  │ConfigService│ │ MqttService │ │  OTA   │ │ Power   │ │HealthMonitor    │ │
│  │ • NVS load  │ │ • Async pub │ │Service │ │ Service │ │ • Component     │ │
│  │ • Validation│ │ • Reconnect │ │        │ │ • Sleep │ │   tracking      │ │
│  │ • Events    │ │ • Queue     │ │        │ │ • Wake  │ │ • Reporting     │ │
│  └─────────────┘ └─────────────┘ └────────┘ └─────────┘ └─────────────────┘ │
├─────────────────────────────────────────────────────────────────────────────┤
│                              DRIVER LAYER                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                          Pn532Driver                                │    │
│  │   SPI communication • IRQ handling • Health checks • Error recovery │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────────────────────┤
│                              HARDWARE LAYER                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │   SPI    │  │   WiFi   │  │   NVS    │  │   GPIO   │  │  Flash   │       │
│  │  (PN532) │  │ (MQTT)   │  │ (Config) │  │(LED/Buzz)│  │  (OTA)   │       │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘  └──────────┘       │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Design Patterns

The firmware employs several well-established design patterns optimized for embedded systems:

| Pattern | Implementation | Purpose |
|---------|---------------|---------|
| **Observer** | `EventBus` + `IEventListener` | Decoupled event-driven communication |
| **Strategy** | `IModule` + `ModuleManager` | Pluggable functionality modules |
| **State Machine** | `OtaService`, `PowerService` | Complex state transitions |
| **Singleton** | Global service instances | Shared resources (EventBus, Config) |
| **RAII** | `ScopedWakeLock` | Resource management without exceptions |
| **Template Method** | `ModuleBase` | Common module lifecycle |

---

### EventBus (Observer Pattern)

The `EventBus` is the central nervous system of the firmware, implementing a **thread-safe publish-subscribe** pattern with priority support.

#### How It Works

```
┌──────────────┐     publish()      ┌──────────────┐     dispatch()     ┌──────────────┐
│   Producer   │ ─────────────────► │   EventBus   │ ─────────────────► │  Subscriber  │
│  (PN532Drv)  │                    │              │                    │ (Attendance) │
└──────────────┘                    │  ┌────────┐  │                    └──────────────┘
                                    │  │ Queue  │  │                    ┌──────────────┐
                                    │  │(Normal)│  │ ─────────────────► │  Subscriber  │
                                    │  └────────┘  │                    │    (MQTT)    │
                                    │  ┌────────┐  │                    └──────────────┘
                                    │  │ Queue  │  │                    ┌──────────────┐
                                    │  │ (High) │  │ ─────────────────► │  Subscriber  │
                                    │  └────────┘  │                    │   (Health)   │
                                    └──────────────┘                    └──────────────┘
```

#### Key Features

- **Dual Priority Queues**: High-priority events (errors, OTA) processed first
- **Bitmask Filtering**: O(1) event type filtering with `EventFilter`
- **Thread-Safe**: FreeRTOS mutex protection for subscriber list
- **Non-Blocking**: Configurable queue timeout (default: immediate return)

#### Usage Example

```cpp
// 1. Implement IEventListener
class MyComponent : public IEventListener {
public:
    void onEvent(const Event& event) override {
        if (event.type == EventType::CardScanned) {
            auto& payload = std::get<CardScannedEvent>(event.payload);
            // Handle card scan...
        }
    }
};

// 2. Subscribe with optional filter
EventFilter filter = EventFilter::none()
    .include(EventType::CardScanned)
    .include(EventType::MqttConnected);

auto id = eventBus.subscribe(&myComponent, filter);

// 3. Publish events
Event event{
    .type = EventType::CardScanned,
    .payload = CardScannedEvent{.cardId = cardId, .timestampMs = millis()},
    .timestampMs = millis(),
    .priority = EventPriority::E_NORMAL
};
eventBus.publish(event);

// 4. High-priority events skip the normal queue
eventBus.publishHighPriority(criticalEvent);
```

#### Event Types

The system defines 30+ event types organized by domain:

| Domain | Events |
|--------|--------|
| **Card/Attendance** | `CardScanned`, `CardReadError`, `AttendanceRecorded`, `AttendanceBatchReady` |
| **MQTT** | `MqttConnected`, `MqttDisconnected`, `MqttMessageReceived`, `MqttQueueOverflow` |
| **OTA** | `OtaRequested`, `OtaStateChanged`, `OtaProgress`, `OtaVersionInfo` |
| **PN532** | `Pn532StatusChanged`, `Pn532Error`, `Pn532Recovered`, `Pn532CardPresent` |
| **Power** | `PowerStateChanged`, `SleepEntering`, `WakeupOccurred`, `WakeLockAcquired` |
| **Health** | `HealthStatusChanged`, `HighLoadDetected`, `QueueOverflow` |
| **System** | `Heartbeat`, `SystemError`, `SystemWarning`, `ModuleStateChanged` |

---

### Module System (Strategy Pattern)

Modules are the primary extension point for adding new functionality. The `IModule` interface defines a clear lifecycle with event handling.

#### Module Lifecycle

```
                    ┌─────────────────┐
                    │  Uninitialized  │
                    └────────┬────────┘
                             │ begin()
                             ▼
                    ┌─────────────────┐
                    │   Initialized   │
                    └────────┬────────┘
                             │ start()
                             ▼
┌──────────┐        ┌─────────────────┐        ┌──────────┐
│ Starting │◄───────│     Running     │───────►│ Stopping │
└──────────┘        └─────────────────┘        └──────────┘
                             │                        │
                             │ error                  │ stop()
                             ▼                        ▼
                    ┌─────────────────┐        ┌──────────┐
                    │      Error      │        │  Stopped │
                    └─────────────────┘        └──────────┘
```

#### Creating a Custom Module

```cpp
class MyModule : public ModuleBase, public IHealthCheck {
public:
    explicit MyModule(EventBus& bus) : ModuleBase(bus) {}

    // Required: Module metadata
    ModuleInfo getInfo() const override {
        return {
            .name = "MyModule",
            .version = "1.0.0",
            .description = "Does something useful",
            .enabledByDefault = true,
            .priority = 5  // Higher = started first
        };
    }

    // Required: Health check interface
    HealthStatus getHealth() const override {
        return {.state = HealthState::Healthy, .message = "OK"};
    }
    std::string_view getComponentName() const override { return "MyModule"; }
    bool performHealthCheck() override { return true; }

protected:
    // Lifecycle hooks
    void onStart() override {
        // Initialize resources, start tasks
    }

    void onStop() override {
        // Cleanup resources, stop tasks
    }

    // Event handling (called only when Running)
    void onEvent(const Event& event) override {
        switch (event.type) {
            case EventType::CardScanned:
                handleCardScan(std::get<CardScannedEvent>(event.payload));
                break;
            // ...
        }
    }

    // Configuration updates
    void onConfigUpdate(const AppConfig& config) override {
        // Apply new configuration
    }
};
```

#### Registering Modules

```cpp
// In main.cpp setup()
g_moduleManager = new ModuleManager(*g_eventBus);
g_moduleManager->addModule(*g_attendanceModule);
g_moduleManager->addModule(*g_otaModule);
g_moduleManager->addModule(*g_myModule);  // Add your module

// Start all modules in priority order
g_moduleManager->startAll();
```

---

### State Machine Pattern

Complex components use explicit state machines for predictable behavior:

#### OTA State Machine

```
┌────────┐  check   ┌──────────┐  url valid  ┌─────────────┐
│  Idle  │─────────►│ Checking │────────────►│ Downloading │
└────────┘          └──────────┘             └──────┬──────┘
    ▲                    │                          │
    │                    │ no update                │ complete
    │                    ▼                          ▼
    │               ┌──────────┐             ┌───────────┐
    │               │  Failed  │◄────────────│ Verifying │
    │               └──────────┘   error     └─────┬─────┘
    │                    │                         │ valid
    │                    │ retry                   ▼
    │                    │                   ┌──────────┐
    │                    │                   │ Applying │
    │                    │                   └────┬─────┘
    │                    │                        │
    └────────────────────┴────────────────────────┘
                                            ┌───────────┐
                                            │ Completed │──► restart
                                            └───────────┘
```

#### Power State Machine

```
┌────────┐  idle timeout  ┌──────┐  deeper sleep  ┌───────────┐
│ Active │───────────────►│ Idle │───────────────►│LightSleep │
└────────┘                └──────┘                └─────┬─────┘
    ▲                         ▲                         │
    │                         │                         │ extended idle
    │ wake event              │ activity               ▼
    │                         │                   ┌───────────┐
    └─────────────────────────┴───────────────────│ DeepSleep │
                                                  └───────────┘
```

---

## Getting Started

### Prerequisites

| Requirement | Version | Notes |
|-------------|---------|-------|
| **PlatformIO** | 6.x | CLI or VS Code extension |
| **Python** | 3.8+ | For PlatformIO |
| **ESP32 Board** | Any | ESP32-DevKit recommended |
| **PN532 Module** | — | SPI mode required |
| **MQTT Broker** | Any | Mosquitto, HiveMQ, etc. |

### Hardware Setup

#### Pin Configuration (ESP32)

| Signal | GPIO | PN532 Pin | Notes |
|--------|------|-----------|-------|
| SPI SCK | 18 | SCK | Clock |
| SPI MISO | 19 | MISO | Data from PN532 |
| SPI MOSI | 23 | MOSI | Data to PN532 |
| SPI SS | 5 | SS | Chip select |
| IRQ | 4 | IRQ | Wake interrupt |
| RST | 5 | RSTPDN | Hardware reset |

#### Wiring Diagram

```
ESP32                          PN532
┌──────────┐                  ┌──────────┐
│      3V3 │──────────────────│ VCC      │
│      GND │──────────────────│ GND      │
│   GPIO18 │──────────────────│ SCK      │
│   GPIO19 │──────────────────│ MISO     │
│   GPIO23 │──────────────────│ MOSI     │
│    GPIO5 │──────────────────│ SS       │
│    GPIO4 │──────────────────│ IRQ      │
│    GPIO5 │──────────────────│ RSTPDN   │
└──────────┘                  └──────────┘
```

### Building & Flashing

```bash
# Clone the repository
git clone https://github.com/your-org/isic-project-hardware.git
cd isic-project-hardware

# Build for ESP32 (default)
pio run -e esp32dev

# Build for ESP8266
pio run -e esp12f

# Upload to connected device
pio run -e esp32dev -t upload

# Monitor serial output
pio device monitor

# Build + Upload + Monitor (one command)
pio run -e esp32dev -t upload -t monitor

# Clean build artifacts
pio run -t clean
```

---

## Target Platforms

### ESP32 Development Kit

**Environment**: `esp32dev`

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
board_build.partitions = partitions_ota_esp32.csv

build_flags =
    -std=gnu++20
    -DHW_TARGET_ESP32
    -DCORE_DEBUG_LEVEL=ARDUHAL_LOG_LEVEL_DEBUG
```

**Features**:
- Dual-core FreeRTOS (240MHz)
- 4MB Flash with OTA partitions
- Full feature set enabled
- Debug logging enabled

### ESP-12F (ESP8266)

**Environment**: `esp12f`

```ini
[env:esp12f]
platform = espressif8266
board = esp12e
board_build.flash_size = 4MB
board_build.ldscript = eagle.flash.4m1m.ld

build_flags =
    -std=gnu++20
    -DHW_TARGET_ESP8266
    -DPROD_BUILD
    -DCORE_DEBUG_LEVEL=ARDUHAL_LOG_LEVEL_ERROR
```

**Features**:
- Single-core (80MHz)
- 4MB Flash, 1MB SPIFFS
- Production optimizations
- Reduced logging

### Conditional Compilation

Use preprocessor macros for platform-specific code:

```cpp
#ifdef HW_TARGET_ESP32
    // ESP32-specific
#elif defined(HW_TARGET_ESP8266)
    // ESP8266-specific
#endif

#ifdef PROD_BUILD
    // Production
#else
    // Development
#endif
```

---

## Configuration

### Configuration Structure

All configuration is centralized in `AppConfig.hpp`:

```cpp
struct AppConfig {
    WifiConfig wifi;           // SSID, password, timeouts
    MqttConfig mqtt;           // Broker, topics, queue settings
    DeviceConfig device;       // Device ID, location, firmware version
    AttendanceConfig attendance; // Debounce, batching, buffers
    OtaConfig ota;             // Update settings, validation
    Pn532Config pn532;         // Pins, polling, health checks
    PowerConfig power;         // Sleep modes, wake sources
    FeedbackConfig feedback;   // LED/buzzer settings
    HealthConfig health;       // Check intervals, thresholds
    EventBusConfig eventBus;   // Queue sizes, task settings
    LogConfig log;             // Log levels, formatting
    ModulesConfig modules;     // Enable/disable modules
};
```

### NVS Persistent Storage

Configuration is stored in ESP32's Non-Volatile Storage (NVS) and loaded at boot:

```cpp
// ConfigService handles NVS operations
ConfigService configService(eventBus);
configService.begin();  // Loads from NVS or creates defaults

// Access current config
const AppConfig& config = configService.get();

// Update and persist
AppConfig newConfig = config;
newConfig.attendance.debounceMs = 3000;
configService.update(newConfig);  // Saves to NVS + publishes ConfigUpdated event
```

### Runtime Configuration via MQTT

Publish to `device/<device_id>/config/set`:

```json
{
  "wifi": {
    "ssid": "ProductionNetwork",
    "password": "secure_password_123",
    "connectTimeoutMs": 15000,
    "maxRetries": 10
  },
  "mqtt": {
    "broker": "mqtt.production.example.com",
    "port": 8883,
    "username": "device_user",
    "password": "mqtt_secret",
    "tls": true,
    "keepAliveSeconds": 30,
    "outboundQueueSize": 128
  },
  "device": {
    "deviceId": "isic-esp32-building-a-001",
    "locationId": "building-a-entrance"
  },
  "attendance": {
    "debounceMs": 2500,
    "offlineBufferSize": 512,
    "batchingEnabled": true,
    "batchMaxSize": 20,
    "batchFlushIntervalMs": 5000
  },
  "pn532": {
    "pollIntervalMs": 50,
    "healthCheckIntervalMs": 10000,
    "maxConsecutiveErrors": 3
  },
  "power": {
    "sleepEnabled": true,
    "sleepType": "light",
    "idleTimeoutMs": 60000,
    "wakeSourcePn532Enabled": true
  },
  "health": {
    "checkIntervalMs": 30000,
    "reportIntervalMs": 120000,
    "publishToMqtt": true
  },
  "ota": {
    "enabled": true,
    "autoCheck": true,
    "checkIntervalMs": 3600000,
    "requireHttps": true
  }
}
```

---

## MQTT Protocol

### Topic Structure

All topics follow the pattern: `device/<device_id>/<resource>[/<action>]`

```
device/
└── isic-esp32-001/
    ├── status              # Online/offline (retained)
    ├── attendance          # Single card events
    ├── attendance/batch    # Batched events
    ├── config/
    │   └── set             # Configuration commands
    ├── ota/
    │   ├── set             # OTA commands
    │   ├── status          # OTA state (retained)
    │   ├── progress        # Download progress
    │   └── error           # Error messages
    ├── status/
    │   ├── health          # Health reports
    │   └── pn532           # NFC reader status
    ├── metrics             # Performance metrics
    └── modules/
        └── +/set           # Module commands
```

### Subscriptions (Device Listens)

| Topic | Description | QoS |
|-------|-------------|-----|
| `device/<id>/config/set` | Configuration updates | 1 |
| `device/<id>/ota/set` | OTA commands | 1 |
| `device/<id>/modules/+/set` | Module-specific commands | 0 |

### Publications (Device Publishes)

| Topic | Description | Retained | QoS |
|-------|-------------|----------|-----|
| `device/<id>/status` | Online/offline LWT | Yes | 1 |
| `device/<id>/attendance` | Single card scan | No | 0 |
| `device/<id>/attendance/batch` | Batched scans | No | 1 |
| `device/<id>/status/health` | Health report | No | 0 |
| `device/<id>/status/pn532` | NFC status | No | 0 |
| `device/<id>/ota/status` | OTA state | Yes | 1 |
| `device/<id>/ota/progress` | Download % | No | 0 |
| `device/<id>/ota/error` | Error details | No | 1 |
| `device/<id>/metrics` | System metrics | No | 0 |

### Message Formats

#### Attendance Event (Single)

```json
{
  "card_id": "04A5B7C8D9E0F1",
  "timestamp_ms": 1699876543210,
  "device_id": "isic-esp32-001",
  "location_id": "building-a-room-101",
  "sequence": 42
}
```

#### Attendance Batch

```json
{
  "batch_id": "b_1699876543",
  "device_id": "isic-esp32-001",
  "location_id": "building-a-room-101",
  "count": 5,
  "first_timestamp_ms": 1699876540000,
  "last_timestamp_ms": 1699876543210,
  "records": [
    {"card_id": "04A5B7C8D9E0F1", "timestamp_ms": 1699876540000, "sequence": 40},
    {"card_id": "04B6C8D9E0F213", "timestamp_ms": 1699876541000, "sequence": 41},
    {"card_id": "04C7D9E0F11425", "timestamp_ms": 1699876542000, "sequence": 42},
    {"card_id": "04D8E0F1152536", "timestamp_ms": 1699876542500, "sequence": 43},
    {"card_id": "04E9F1A2163647", "timestamp_ms": 1699876543210, "sequence": 44}
  ]
}
```

#### Health Report

```json
{
  "overall": "healthy",
  "uptime_s": 86400,
  "free_heap_kb": 180,
  "min_heap_kb": 145,
  "device_id": "isic-esp32-001",
  "firmware": "1.0.0",
  "wifi_rssi": -65,
  "counts": {
    "healthy": 4,
    "degraded": 0,
    "unhealthy": 0,
    "unknown": 0
  },
  "components": [
    {"name": "MQTT", "state": "healthy", "message": "Connected"},
    {"name": "PN532", "state": "healthy", "message": "Ready"},
    {"name": "Attendance", "state": "healthy", "message": "Processing"},
    {"name": "OTA", "state": "healthy", "message": "Idle"}
  ]
}
```

#### OTA Status

```json
{
  "old_state": "idle",
  "new_state": "downloading",
  "message": "Downloading firmware v1.2.3...",
  "timestamp_ms": 1699876543210,
  "device_id": "isic-esp32-001",
  "firmware_version": "1.0.0",
  "target_version": "1.2.3"
}
```

#### OTA Progress

```json
{
  "percent": 45,
  "bytes_downloaded": 512000,
  "total_bytes": 1138688,
  "remaining_bytes": 626688,
  "speed_bps": 48000,
  "eta_seconds": 13,
  "success": false,
  "message": "Downloading: 45%",
  "device_id": "isic-esp32-001"
}
```

#### OTA Error

```json
{
  "error": "checksum_mismatch",
  "message": "SHA256 verification failed: expected abc123..., got def456...",
  "timestamp_ms": 1699876543210,
  "device_id": "isic-esp32-001",
  "recoverable": true
}
```

---

## OTA Updates

### Partition Layout

The ESP32 uses a dual-partition OTA scheme for safe updates:

```
┌────────────────────────────────────────────────────────────────────┐
│                        4MB Flash Layout                            │
├──────────────┬──────────────┬───────────┬──────────────────────────┤
│   Offset     │  Partition   │   Size    │      Description         │
├──────────────┼──────────────┼───────────┼──────────────────────────┤
│   0x9000     │     nvs      │   20 KB   │ Non-volatile storage     │
│   0xE000     │   otadata    │    8 KB   │ OTA boot selection       │
│  0x10000     │    app0      │ 1.25 MB   │ OTA slot 0 (ota_0)       │
│ 0x150000     │    app1      │ 1.25 MB   │ OTA slot 1 (ota_1)       │
│ 0x290000     │   spiffs     │  1.4 MB   │ Filesystem               │
└──────────────┴──────────────┴───────────┴──────────────────────────┘
```

### OTA State Machine

```
┌─────────────────────────────────────────────────────────────────────┐
│                        OTA Update Flow                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌────────┐                                                        │
│   │  Idle  │◄────────────────────────────────────────────┐          │
│   └───┬────┘                                             │          │
│       │ update cmd                                       │          │
│       ▼                                                  │          │
│   ┌──────────┐  no update   ┌────────┐                   │          │
│   │ Checking │─────────────►│ Failed │───────────────────┤          │
│   └────┬─────┘              └────────┘                   │          │
│        │ update available                                │          │
│        ▼                                                 │          │
│   ┌─────────────┐                                        │          │
│   │ Downloading │────────────────────────────────────────┤          │
│   │             │  timeout/error                         │          │
│   └──────┬──────┘                                        │          │
│          │ complete                                      │          │
│          ▼                                               │          │
│   ┌───────────┐                                          │          │
│   │ Verifying │──────────────────────────────────────────┤          │
│   │           │  checksum fail                           │          │
│   └─────┬─────┘                                          │          │
│         │ valid                                          │          │
│         ▼                                                │          │
│   ┌──────────┐                                           │          │
│   │ Applying │───────────────────────────────────────────┘          │
│   │          │  partition error                                     │
│   └────┬─────┘                                                      │
│        │ success                                                    │
│        ▼                                                            │
│   ┌───────────┐                                                     │
│   │ Completed │──────► ESP.restart()                                │
│   └───────────┘                                                     │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### OTA Commands

Publish to `device/<device_id>/ota/set`:

#### Trigger Update

```json
{
  "action": "update",
  "url": "https://releases.example.com/firmware/v1.2.3.bin",
  "version": "1.2.3",
  "sha256": "a1b2c3d4e5f6789...",
  "force": false
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `action` | Yes | Must be `"update"` |
| `url` | Yes | HTTPS URL of firmware binary |
| `version` | No | Target version for comparison |
| `sha256` | No | Expected SHA256 hash (64 hex chars) |
| `force` | No | Skip version check if `true` |

#### Other Commands

```json
// Check for updates
{ "action": "check" }

// Cancel in-progress update
{ "action": "cancel" }

// Rollback to previous firmware
{ "action": "rollback" }

// Mark current partition as valid
{ "action": "mark_valid" }

// Get current status
{ "action": "get_status" }
```

### Boot Validation & Rollback

After OTA, the new firmware boots in "pending validation" state:

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Post-OTA Boot Sequence                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────┐                                                    │
│  │  New Boot   │                                                    │
│  │  (Pending)  │                                                    │
│  └──────┬──────┘                                                    │
│         │                                                           │
│         ├───────────────────┬─────────────────────┐                 │
│         │                   │                     │                 │
│         ▼                   ▼                     ▼                 │
│  ┌─────────────┐    ┌─────────────┐      ┌─────────────┐            │
│  │ WiFi OK?    │    │ MQTT OK?    │      │ 30s stable? │            │
│  └──────┬──────┘    └──────┬──────┘      └──────┬──────┘            │
│         │                  │                    │                   │
│         └──────────────────┴────────────────────┘                   │
│                             │                                       │
│              ┌──────────────┴──────────────┐                        │
│              │                             │                        │
│              ▼ All pass                    ▼ Any fail               │
│     ┌─────────────────┐           ┌─────────────────┐               │
│     │  Mark Valid     │           │ Auto Rollback   │               │
│     │ (Partition OK)  │           │ (Previous FW)   │               │
│     └─────────────────┘           └─────────────────┘               │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**Validation Criteria** (configurable):
1. WiFi connection established
2. MQTT broker connected
3. No critical errors for 30 seconds
4. Or manual `mark_valid` command received

### CLI Examples

```bash
# Trigger OTA update
mosquitto_pub -h mqtt.example.com \
  -t "device/isic-esp32-001/ota/set" \
  -m '{"action":"update","url":"https://releases.example.com/v1.2.3.bin","version":"1.2.3"}'

# Monitor OTA status
mosquitto_sub -h mqtt.example.com \
  -t "device/isic-esp32-001/ota/status" -v

# Watch download progress
mosquitto_sub -h mqtt.example.com \
  -t "device/isic-esp32-001/ota/progress" -v

# Emergency rollback
mosquitto_pub -h mqtt.example.com \
  -t "device/isic-esp32-001/ota/set" \
  -m '{"action":"rollback"}'
```

---

## Power Management

### Sleep Modes

| Mode | Power | Wake Latency | RAM | Use Case |
|------|-------|--------------|-----|----------|
| **Active** | ~100mA | — | Full | Processing |
| **Light Sleep** | ~0.8mA | ~1ms | Retained | Idle waiting |
| **Deep Sleep** | ~10µA | ~200ms | Lost | Extended idle |

### Wake Sources

1. **PN532 Interrupt**: Card presented to NFC reader
2. **Timer**: Periodic wake for health checks
3. **GPIO**: External button/sensor
4. **Touch**: Capacitive touch pins

### Wake Lock API

Services acquire wake locks to prevent sleep during critical operations:

```cpp
// RAII-style (recommended)
{
    ScopedWakeLock lock(powerService, "mqtt_publish");
    // Device won't sleep while lock exists
    mqttService.publishSync(topic, payload);
}  // Lock released automatically

// Manual style
auto lockId = powerService.requestWakeLock("ota_download");
// ... long operation ...
powerService.releaseWakeLock(lockId);
```

---

## Health Monitoring

### Component States

| State | Description | Action |
|-------|-------------|--------|
| `healthy` | Operating normally | None |
| `degraded` | Functional with issues | Monitor |
| `unhealthy` | Not functional | Alert/Recover |
| `unknown` | State not determined | Initialize |

### Health Check Interface

```cpp
class IHealthCheck {
public:
    virtual HealthStatus getHealth() const = 0;
    virtual std::string_view getComponentName() const = 0;
    virtual bool performHealthCheck() = 0;
};

struct HealthStatus {
    HealthState state{HealthState::Unknown};
    std::string message{};
    std::uint64_t lastCheckMs{0};
};
```

### Registered Components

- **MqttService**: Connection state, queue depth
- **Pn532Driver**: Communication status, error rate
- **AttendanceModule**: Processing state, buffer usage
- **OtaService**: Update state, partition health

---

## Code Style & Standards

### Language Standard

- **C++20** (`-std=gnu++20`) — No C++23 features
- Custom `Result<T>` instead of `std::expected`
- Embedded-safe subset of STL

### Coding Conventions

```cpp
// 4-space indentation
// Opening braces on same line
// snake_case for variables
// PascalCase for types
// m_ prefix for members
// Brace initialization {}

class MyService {
public:
    explicit MyService(EventBus& bus);

    [[nodiscard]] Status begin(const AppConfig& cfg);
    [[nodiscard]] bool isReady() const noexcept { return m_ready; }

private:
    EventBus& m_bus;
    bool m_ready{false};
    std::uint32_t m_counter{0};
};
```

### C++20 Features Used

| Feature | Usage |
|---------|-------|
| `std::uint8_t`, `std::uint32_t` | Fixed-width integers |
| `enum class` | Type-safe enumerations |
| `std::span` | Buffer views |
| `std::optional` | Nullable values |
| `std::variant` | Event payloads |
| `std::string_view` | Non-owning strings |
| `constexpr` | Compile-time constants |
| `[[nodiscard]]` | Enforced return checking |
| `noexcept` | Exception specification |

### Embedded Constraints

- Exceptions disabled (`-fno-exceptions`)
- Minimal heap in hot paths
- `std::array` over `std::vector`
- Static/stack allocation for services
- No `<thread>`, `<filesystem>`, `<future>`

---

## Project Structure

```
isic-project-hardware/
├── include/
│   ├── AppConfig.hpp              # All configuration structures
│   ├── core/
│   │   ├── EventBus.hpp           # Pub/sub event system
│   │   ├── Types.hpp              # Events, payloads, enums
│   │   ├── IHealthCheck.hpp       # Health monitoring interface
│   │   ├── IModule.hpp            # Module interface + base class
│   │   ├── ModuleManager.hpp      # Module lifecycle management
│   │   ├── Logger.hpp             # Logging macros
│   │   └── Result.hpp             # Error handling (Result<T>)
│   ├── drivers/
│   │   └── Pn532Driver.hpp        # NFC driver with health
│   ├── modules/
│   │   ├── AttendanceModule.hpp   # Card processing module
│   │   └── OtaModule.hpp          # OTA command handler
│   └── services/
│       ├── AttendanceBatcher.hpp  # Event batching
│       ├── ConfigService.hpp      # NVS configuration
│       ├── HealthMonitorService.hpp
│       ├── MqttService.hpp        # Async MQTT client
│       ├── OtaService.hpp         # OTA state machine
│       ├── PowerService.hpp       # Sleep/wake management
│       └── UserFeedbackService.hpp # LED/buzzer
├── src/
│   ├── main.cpp                   # Entry point, initialization
│   ├── core/
│   │   ├── EventBus.cpp
│   │   └── ModuleManager.cpp
│   ├── drivers/
│   │   └── Pn532Driver.cpp
│   ├── modules/
│   │   ├── AttendanceModule.cpp
│   │   └── OtaModule.cpp
│   └── services/
│       ├── AttendanceBatcher.cpp
│       ├── ConfigService.cpp
│       ├── HealthMonitorService.cpp
│       ├── MqttService.cpp
│       ├── OtaService.cpp
│       ├── PowerService.cpp
│       └── UserFeedbackService.cpp
├── platformio.ini                 # Build configuration
├── partitions_ota_esp32.csv       # Flash partition layout
└── README.md
```

---

## License

**Proprietary** — ISIC Attendance System Project

© 2025 All Rights Reserved
