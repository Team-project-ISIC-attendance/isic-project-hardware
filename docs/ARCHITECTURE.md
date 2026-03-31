# Architecture And Layers

## System Overview

This firmware is an event-driven embedded application for an ISIC attendance reader built on ESP8266/ESP32 hardware. The codebase is organized around service ownership, scheduler-driven loops, typed events, platform adapters, and a dual-domain power model where the ESP and PN532 can intentionally target different power states.

The design goals are:

- keep attendance capture reliable for the first tap after idle
- keep subsystem coupling low through the event bus
- support offline buffering when Wi-Fi or MQTT is unavailable
- keep platform-specific code isolated
- make runtime behavior observable through logs, metrics, and explicit state types

## Design Goals

| Goal | How The Architecture Supports It |
|---|---|
| Reliable card capture | `Pn532Service`, IRQ-first flow, buffered attendance |
| Loose coupling | `EventBus`, `Signal`, typed events in `Types.hpp` |
| Operational resilience | offline buffering, config persistence, reconnect logic |
| Cross-board support | platform wrappers in `include/platform/` |
| Maintainability | service boundaries, shared base classes, layered docs |
| Battery efficiency | `PowerService` coordinating ESP state and PN532 target mode |

## Repository And Layer Map

| Layer | Main Files | Role |
|---|---|---|
| Application | `include/App.hpp`, `src/App.cpp`, `src/main.cpp` | startup, service ownership, task scheduling |
| Common Types | `include/common/Config.hpp`, `include/common/Types.hpp`, `include/common/Logger.hpp` | shared config, enums, payloads, metrics, logging |
| Core | `include/core/EventBus.hpp`, `include/core/Signal.hpp`, `include/core/IService.hpp` | lifecycle contracts and async event dispatch |
| Services | `include/services/*`, `src/services/*` | firmware feature logic |
| Platform | `include/platform/*` | ESP8266/ESP32 wrappers and portability helpers |
| Utilities | `include/utils/FilesystemCommandHandler.hpp` | optional debugging/inspection utility |
| Docs | `docs/*` | user, developer, architecture, battery, memory references |

## System Diagram

```text
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
│  │ConfigService│ │  WiFi   │ │  Mqtt  │ │   Ota   │ │  PN532  │ │Feedback│  │
│  │• LittleFS   │ │ Service │ │Service │ │ Service │ │ Service │ │Service │  │
│  │• JSON parse │ │• AP mode│ │• Queue │ │• Update │ │• SPI    │ │• LED   │  │
│  └─────────────┘ └─────────┘ └────────┘ └─────────┘ └─────────┘ └────────┘  │
│          │                       │           │           │                  │
│          ▼                       ▼           ▼           ▼                  │
│  ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐  │
│  │  AttendanceService  │  │    HealthService    │  │    PowerService     │  │
│  │  • Debounce/batch   │  │  • Component checks │  │  • Sleep modes      │  │
│  │  • Offline buffer   │  │  • MQTT reporting   │  │  • Traffic-aware    │  │
│  └─────────────────────┘  └─────────────────────┘  └─────────────────────┘  │
│                                                                             │
│                           ┌─────────────────────┐                           │
│                           │    TaskScheduler    │                           │
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

## Design Patterns

| Pattern | Implementation | Purpose |
|---|---|---|
| **Signal/Slot** | `Signal<T>` + `EventBus` | Decoupled event-driven communication |
| **Service Base** | `IService` + `ServiceBase` | Common service lifecycle |
| **Cooperative Tasks** | `TaskScheduler` | Non-blocking multitasking |
| **RAII** | `ScopedConnection` | Auto-unsubscribe on destruction |

## EventBus (Signal/Slot Pattern)

The `EventBus` is the central nervous system using a type-safe Signal/Slot pattern with one signal per `EventType`.

### How It Works

```text
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

## Direct Ownership vs Event-Driven Coordination

### Direct Ownership

These relationships are direct object ownership or direct dependency references:

- `App` owns all services, the scheduler, the event bus, and the web server
- `WiFiService` directly uses `ConfigService` and `AsyncWebServer`
- `Pn532Service` directly uses `ConfigService`
- `MqttService` directly uses `MqttConfig` and `DeviceConfig`
- `PowerService` directly uses `PowerConfig`

### Event-Driven Coordination

These relationships are intentionally decoupled through `EventBus`:

- card scans from `Pn532Service` to `AttendanceService` and `PowerService`
- MQTT connection state to attendance, health, and power behavior
- power state changes from `PowerService` to `WiFiService` and `Pn532Service`
- feedback requests and attendance notifications
- OTA lifecycle and guard behavior

This split is one of the main architectural choices in the repository: ownership is explicit, coordination is event-driven.

## Runtime Lifecycle

### Boot And Startup Sequence

```text
┌──────────┐     construct/begin()     ┌──────────┐
│   Main   │ ────────────────────────► │   App    │
└──────────┘                           └────┬─────┘
                                            │
                                            ▼
      ┌──────────────┬──────────────┬──────────────┬──────────────┐
      ▼              ▼              ▼              ▼              ▼
┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│ConfigService │ │ WiFiService  │ │ MqttService  │ │ Pn532Service │ │AttendanceSvc │
│  begin()     │ │  begin()     │ │  begin()     │ │  begin()     │ │  begin()     │
└──────────────┘ └──────────────┘ └──────────────┘ └──────────────┘ └──────────────┘
      ┌──────────────┬──────────────┬──────────────┐
      ▼              ▼              ▼
┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│ PowerService │ │FeedbackSvc   │ │ HealthSvc    │
│  begin()     │ │  begin()     │ │  begin()     │
└──────────────┘ └──────────────┘ └──────┬───────┘
                                         │
                                         ├── register components
                                         ├── start web server
                                         └── register scheduler tasks
```

### Scheduler Model

`App` drives services through periodic scheduler tasks instead of a blocking main loop. The important task cadence in `App` is:

- event bus dispatch: high-frequency
- feedback and NFC: fast enough for responsive interaction
- Wi-Fi, MQTT, OTA, power, health: lower-rate maintenance loops

This means services are expected to be non-blocking and stateful.

## Main Runtime Flows

### Card Scan Flow

```text
┌──────────────┐     tap card      ┌──────────────┐     CardScanned     ┌──────────────┐
│ Passive ISIC │ ────────────────► │ Pn532Service │ ──────────────────► │   EventBus   │
└──────────────┘                   └──────────────┘                     └──────┬───────┘
                                                                                │
                                             ┌──────────────────────────────────┴─────────────────────────────────┐
                                             ▼                                                                    ▼
                                    ┌──────────────────┐                                                 ┌──────────────┐
                                    │AttendanceService │                                                 │PowerService  │
                                    │ debounce/batch   │                                                 │ reset idle   │
                                    │ offline buffer   │                                                 │ track burst  │
                                    └────────┬─────────┘                                                 └──────────────┘
                                             │
                                             ├── AttendanceRecorded
                                             └── MqttPublishRequest
                                             │
                                             ▼
                                    ┌──────────────────┐
                                    │   EventBus       │
                                    └────────┬─────────┘
                                             ▼
                                    ┌──────────────────┐
                                    │   MqttService    │
                                    │ publish if online│
                                    └──────────────────┘
```

### Configuration Flow

```text
┌──────────────┐     begin()      ┌──────────────┐     load /config.json     ┌──────────────┐
│    Boot      │ ───────────────► │ConfigService │ ────────────────────────► │   LittleFS   │
└──────────────┘                  └──────┬───────┘                           └──────────────┘
                                         │
                                         ▼
                                ┌──────────────────┐
                                │ parse + validate │
                                │ apply defaults   │
                                └────────┬─────────┘
                                         │ runtime update
                                         ▼
                                ┌──────────────────┐
                                │    EventBus      │
                                │  ConfigChanged   │
                                └────────┬─────────┘
                                         ▼
                                ┌──────────────────┐
                                │  Other Services  │
                                │ reload settings  │
                                └──────────────────┘

Config persistence path:

┌──────────────┐    save updated config    ┌──────────────┐
│ConfigService │ ────────────────────────► │   LittleFS   │
└──────────────┘                           └──────────────┘
```

### MQTT Flow

```text
┌──────────────┐   WifiConnected   ┌──────────────┐   notify/connect   ┌──────────────┐
│ WiFiService  │ ────────────────► │   EventBus   │ ─────────────────► │ MqttService  │
└──────────────┘                   └──────────────┘                    └──────┬───────┘
                                                                               │
                                                                               ▼
                                                                      ┌────────────────┐
                                                                      │ MQTT Broker    │
                                                                      │ connect/reconn │
                                                                      └──────┬─────────┘
                                                                             │
                                                                             ▼
                                                                    ┌──────────────────┐
                                                                    │    EventBus       │
                                                                    │  MqttConnected    │
                                                                    └──────┬───────────┘
                    ┌──────────────────────────────┬─────────────────────────┴──────────────────────────────┐
                    ▼                              ▼                                                        ▼
           ┌──────────────────┐           ┌──────────────────┐                                   ┌──────────────────┐
           │AttendanceService │           │   OtaService     │                                   │ HealthService    │
           │ flush offline     │           │ subscribe/check  │                                   │ enable reporting │
           └────────┬─────────┘           └──────────────────┘                                   └──────────────────┘
                    │
                    ▼
           ┌──────────────────┐     MqttPublishRequest      ┌──────────────────┐
           │    EventBus      │ ──────────────────────────► │   MqttService    │
           └──────────────────┘                             └────────┬─────────┘
                                                                      ▼
                                                            ┌──────────────────┐
                                                            │ MqttMessage event │
                                                            │ to subscribers    │
                                                            └──────────────────┘
```

### OTA Flow

```text
┌──────────────┐     ota/start     ┌──────────────┐    fetch manifest    ┌──────────────┐
│ MQTT Trigger │ ────────────────► │ OtaService   │ ───────────────────► │ HTTP Server  │
└──────────────┘                   └──────┬───────┘                      └──────────────┘
                                          │
                                          ▼
                                 ┌──────────────────┐
                                 │ version check    │
                                 │ state transition │
                                 └────────┬─────────┘
                                          ▼
                                 ┌──────────────────┐
                                 │    EventBus      │
                                 │   OtaStarted     │
                                 └────────┬─────────┘
                                          ▼
                                 ┌──────────────────┐
                                 │  PowerService    │
                                 │ block low-power  │
                                 └──────────────────┘

Download path:

┌──────────────┐   download firmware   ┌──────────────┐
│ OtaService   │ ────────────────────► │ HTTP Server  │
└──────┬───────┘                       └──────────────┘
       │
       ├── stream firmware data ─────► ┌──────────────┐
       │                               │Update Library│
       │                               └──────────────┘
       │
       └── publish progress/status ──► ┌──────────────┐
                                       │   EventBus   │
                                       │ OtaProgress  │
                                       │ OtaDone/Error│
                                       └──────────────┘
```

### Health And Metrics Flow

```text
┌──────────────┐   inspect state/metrics   ┌──────────────────────┐
│HealthService │ ────────────────────────► │ Registered Components │
└──────┬───────┘                           └──────────────────────┘
       │
       ▼
┌──────────────────┐
│ aggregate health │
│ build snapshot   │
└────────┬─────────┘
         ▼
┌──────────────────┐      health payload      ┌──────────────────┐
│    EventBus      │ ───────────────────────► │   MqttService    │
└──────────────────┘                          └──────────────────┘
```

## Power Architecture

The current firmware intentionally separates:

- ESP-level runtime power state
- PN532 reader target power mode

This is a major architectural point. The reader may keep Wi-Fi and the main MCU ready while the PN532 sleeps, or keep both fully active during a burst of scans.

### Power Coordination Diagram

```text
    ┌────────────────────┐
    │ Recent CardScanned │
    └─────────┬──────────┘
              │
    ┌─────────▼──────────┐
    │  MQTT disconnect   │
    └─────────┬──────────┘
              │
    ┌─────────▼──────────┐
    │   AP mode guard    │
    └─────────┬──────────┘
              │
    ┌─────────▼──────────┐
    │    OTA guard       │
    └─────────┬──────────┘
              │
              ▼
┌──────────────────────────────┐
│         PowerService         │
│ traffic-aware coordinator    │
│ burst detection + idle logic │
└──────────────┬───────────────┘
               │
       ┌───────┴────────┐
       ▼                ▼
┌──────────────┐  ┌──────────────┐
│  WiFiService │  │ Pn532Service │
│ PowerState   │  │Pn532PowerMode│
└──────┬───────┘  └──────────────┘
       ▼
┌──────────────────────┐
│ AttendanceService    │
│ offline/online flush │
└──────────────────────┘
```

### Why Two Power Domains

- PN532 wake latency directly affects first-tap user experience
- Wi-Fi reconnect cost affects battery and upload latency
- attendance buffering allows the network side to sleep without losing events
- classroom traffic is bursty, so sleeping both subsystems together is often suboptimal

### Current Power Roles

| Component | Role |
|---|---|
| `PowerService` | traffic-aware power coordinator and policy engine |
| `WiFiService` | applies ESP-side Wi-Fi sleep/power-down behavior |
| `Pn532Service` | applies reader-side active, sleep, and recovery transitions |
| `AttendanceService` | makes modem sleep safe by buffering records offline |

## Event Model

The event model is defined in `include/common/Types.hpp` and dispatched through `EventBus`.

### Event Categories

| Category | Examples |
|---|---|
| System | `SystemReady`, `SystemError` |
| Config | `ConfigChanged`, `ConfigError` |
| Wi-Fi | `WifiConnected`, `WifiDisconnected`, `WifiApStarted`, `WifiApStopped` |
| MQTT | `MqttConnected`, `MqttDisconnected`, `MqttMessage`, `MqttPublishRequest`, `MqttSubscribeRequest` |
| NFC | `NfcReady`, `CardScanned`, `CardRemoved`, `NfcError` |
| Attendance | `AttendanceRecorded`, `AttendanceError` |
| OTA | `OtaStarted`, `OtaProgress`, `OtaCompleted`, `OtaError` |
| Feedback | `FeedbackRequest` |
| Health | `HealthChanged` |
| Power | `PowerStateChange`, `SleepRequested`, `WakeupOccurred` |

### Event Payload Types

| Payload Struct | Main Use |
|---|---|
| `CardEvent` | card UID and timestamp |
| `MqttEvent` | topic, payload, retain flag |
| `FeedbackEvent` | feedback signal request |
| `PowerEvent` | ESP power state transitions and PN532 target mode |
| `Event` | variant container for dispatch |

## Class Catalog

This section covers every class defined in `include/` and the important non-class architectural modules.

### Application

#### `App`

- Purpose: top-level application coordinator
- Ownership/layer: application layer
- Major dependencies: `EventBus`, all services, `TaskScheduler`, `AsyncWebServer`
- Important state/data: scheduler tasks, service instances, app lifecycle state
- Events consumed/published: owns the bus but mainly wires and schedules rather than acting as a business-event subscriber
- Key responsibilities:
  - construct and own all runtime services
  - start services in the required order
  - register periodic scheduler tasks
  - expose service accessors
  - start the web server once services are ready
- Related files: `include/App.hpp`, `src/App.cpp`, `src/main.cpp`

### Core

#### `EventBus`

- Purpose: typed asynchronous publish/subscribe bus
- Ownership/layer: core layer
- Major dependencies: `Signal`, `Event`, `EventType`
- Important state/data: fixed signal array indexed by event type
- Events consumed/published: routes all runtime events
- Key responsibilities:
  - register subscriptions
  - queue events from producers
  - dispatch queued events during scheduler-driven processing
  - provide scoped subscription handles
- Related files: `include/core/EventBus.hpp`, `include/common/Types.hpp`

#### `Signal<Args...>`

- Purpose: generic async signal/slot primitive underneath `EventBus`
- Ownership/layer: core layer
- Major dependencies: `PlatformMutex`, callback storage
- Important state/data: slot list, pending ring buffer, connection IDs
- Events consumed/published: generic transport, not domain-specific
- Key responsibilities:
  - connect/disconnect callbacks
  - queue emissions safely
  - dispatch deferred callbacks
  - support scoped connection lifetime
- Related files: `include/core/Signal.hpp`, `include/platform/PlatformMutex.hpp`

#### `Signal<Args...>::ScopedConnection`

- Purpose: RAII subscription holder
- Ownership/layer: core layer
- Major dependencies: owning `Signal`
- Important state/data: signal pointer, connection ID
- Events consumed/published: none directly
- Key responsibilities:
  - auto-disconnect on destruction
  - prevent dangling subscriptions
- Related files: `include/core/Signal.hpp`

#### `IService`

- Purpose: common service lifecycle contract
- Ownership/layer: core layer
- Major dependencies: shared `Status`, `ServiceState`
- Important state/data: interface only
- Events consumed/published: not directly
- Key responsibilities:
  - define `begin`, `loop`, `end`
  - expose service name/state/metrics contract
- Related files: `include/core/IService.hpp`

#### `ServiceBase`

- Purpose: base implementation for services
- Ownership/layer: core layer
- Major dependencies: `IService`
- Important state/data: service name and current service state
- Events consumed/published: none directly
- Key responsibilities:
  - hold basic name/state bookkeeping
  - provide default metrics serialization
- Related files: `include/core/IService.hpp`

### Services

#### `ConfigService`

- Purpose: persistent configuration access and save/load coordination
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `Config`, LittleFS-backed implementation
- Important state/data: full `Config`, dirty flag, event connections
- Events consumed/published:
  - publishes `ConfigChanged`
  - handles MQTT-based config get/set paths in implementation
- Key responsibilities:
  - load config from `/config.json`
  - save config updates
  - expose config subtree accessors
  - reset config to defaults
- Related files: `include/services/ConfigService.hpp`, `src/services/ConfigService.cpp`

#### `WiFiService`

- Purpose: station/AP mode management and HTTP setup surface
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `ConfigService`, `AsyncWebServer`, `DNSServer`, `PlatformWiFi`
- Important state/data: Wi-Fi state, reconnect timing, AP state, power-sleep flags
- Events consumed/published:
  - consumes `PowerStateChange`
  - publishes Wi-Fi connection and AP lifecycle events
- Key responsibilities:
  - connect to configured station network
  - start/stop AP mode
  - host configuration/status endpoints
  - react to ESP power state transitions
- Related files: `include/services/WiFiService.hpp`, `src/services/WiFiService.cpp`

#### `MqttService`

- Purpose: MQTT connectivity and message transport
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `MqttConfig`, `DeviceConfig`, `PubSubClient`
- Important state/data: MQTT state, topic prefix, reconnect counters, metrics
- Events consumed/published:
  - consumes Wi-Fi and publish/subscribe request events
  - publishes `MqttConnected`, `MqttDisconnected`, `MqttMessage`
- Key responsibilities:
  - maintain broker connection
  - build device-scoped topic prefix
  - publish and subscribe through the event bus
  - apply reconnect backoff
- Related files: `include/services/MqttService.hpp`, `src/services/MqttService.cpp`

#### `OtaService`

- Purpose: OTA update orchestration
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `OtaConfig`, `PlatformOta`, HTTP/Update client types
- Important state/data: OTA state, progress, stream/download state, expected MD5 and size
- Events consumed/published:
  - consumes MQTT connectivity and OTA trigger messages
  - publishes OTA lifecycle events and MQTT publish requests
- Key responsibilities:
  - check for updates
  - fetch manifest
  - stream firmware image
  - complete or fail update cleanly
- Related files: `include/services/OtaService.hpp`, `src/services/OtaService.cpp`

#### `Pn532Service`

- Purpose: NFC reader control and card detection
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `ConfigService`, `Adafruit_PN532`
- Important state/data: PN532 state, power mode, IRQ state, last card, wake/recovery timing
- Events consumed/published:
  - consumes `PowerStateChange`
  - publishes `CardScanned`
- Key responsibilities:
  - initialize PN532 in SPI mode
  - handle IRQ-first reading path
  - support polling fallback
  - perform PN532 sleep, wake, and recovery logic
- Related files: `include/services/Pn532Service.hpp`, `src/services/Pn532Service.cpp`

#### `AttendanceService`

- Purpose: convert card scans into durable attendance records
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `AttendanceConfig`, `PlatformESP` time helper
- Important state/data: batch buffer, offline buffer, debounce cache, sequence number
- Events consumed/published:
  - consumes `CardScanned`, MQTT connectivity, config change events
  - publishes `AttendanceRecorded` and `MqttPublishRequest`
- Key responsibilities:
  - debounce repeated taps
  - batch records for upload
  - keep offline buffer when backend is unavailable
  - flush offline records after reconnect
- Related files: `include/services/AttendanceService.hpp`, `src/services/AttendanceService.cpp`

#### `FeedbackService`

- Purpose: LED and buzzer feedback control
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `FeedbackConfig`
- Important state/data: circular pattern queue, current pattern execution state, device output state
- Events consumed/published:
  - reacts to attendance and other service-driven feedback triggers in implementation
- Key responsibilities:
  - queue and execute feedback patterns
  - provide convenience success/error/connectivity signals
  - keep actuator behavior non-blocking
- Related files: `include/services/FeedbackService.hpp`, `src/services/FeedbackService.cpp`

#### `HealthService`

- Purpose: aggregate runtime health and metrics
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `HealthConfig`, registered `IService` components
- Important state/data: `SystemHealth`, registered component list, publish timing
- Events consumed/published:
  - reacts to MQTT state in implementation
  - publishes health and metrics updates through MQTT request events
- Key responsibilities:
  - inspect component states
  - compute overall health
  - publish health and metrics snapshots
- Related files: `include/services/HealthService.hpp`, `src/services/HealthService.cpp`

#### `PowerService`

- Purpose: traffic-aware coordinator for ESP and PN532 power behavior
- Ownership/layer: service layer
- Major dependencies: `EventBus`, `PowerConfig`, `PlatformPower`
- Important state/data: current ESP power state, PN532 target mode, burst tracking window, sleep metrics
- Events consumed/published:
  - consumes Wi-Fi, MQTT, NFC, card, AP, and OTA lifecycle events
  - publishes `PowerStateChange`, `SleepRequested`, `WakeupOccurred`
- Key responsibilities:
  - compute desired ESP state
  - compute desired PN532 target mode
  - block sleep when AP or OTA state requires it
  - keep the system awake during classroom scan bursts
- Related files: `include/services/PowerService.hpp`, `src/services/PowerService.cpp`

### Platform Classes

#### `Mutex`

- Purpose: cross-platform locking primitive
- Ownership/layer: platform/core boundary
- Major dependencies: ESP8266 interrupt model or ESP32 standard mutexes
- Important state/data: platform-specific lock implementation
- Events consumed/published: none
- Key responsibilities:
  - provide portable locking for signal/event infrastructure
- Related files: `include/platform/PlatformMutex.hpp`

#### `LockGuard<T>`

- Purpose: RAII lock wrapper
- Ownership/layer: platform/core boundary
- Major dependencies: `Mutex`-like type
- Important state/data: reference to held mutex
- Events consumed/published: none
- Key responsibilities:
  - acquire on construction
  - release on destruction
- Related files: `include/platform/PlatformMutex.hpp`

#### `UniqueLock<T>`

- Purpose: movable/manual lock wrapper
- Ownership/layer: platform/core boundary
- Major dependencies: `Mutex`-like type
- Important state/data: mutex pointer and ownership state
- Events consumed/published: none
- Key responsibilities:
  - support flexible locking semantics for cross-platform code
- Related files: `include/platform/PlatformMutex.hpp`

### Utility Classes

#### `FilesystemCommandHandler`

- Purpose: optional serial filesystem inspection helper
- Ownership/layer: utility/debug layer
- Major dependencies: LittleFS, serial I/O
- Important state/data: command and response protocol constants
- Events consumed/published: none
- Key responsibilities:
  - process filesystem inspection commands
  - list files
  - read file contents
  - expose filesystem info
- Related files: `include/utils/FilesystemCommandHandler.hpp`

## Platform Helper Modules

These are not all standalone classes, but they are important architectural modules.

### `PlatformWiFi`

- abstracts Wi-Fi power-save and mode differences between ESP8266 and ESP32
- exposes helpers such as secure-network detection, light sleep, normal power, power down, power up
- used primarily by `WiFiService` and indirectly by MQTT/network behavior

### `PlatformPower`

- translates board reset and wake causes into shared `WakeupReason`
- used by `PowerService`

### `PlatformESP`

- groups chip ID, heap, flash, time, RTC memory, and deep-sleep helpers
- used by attendance timing, health reporting, and other low-level support logic

### `PlatformOta`

- centralizes OTA include differences and shared constants like board name/update size
- used by `OtaService`

## Class Relationship Diagram

```text
┌──────────────┐
│   IService   │
└──────┬───────┘
       ▼
┌──────────────┐
│ ServiceBase  │
└──────┬───────┘
       │
       ├── ConfigService
       ├── WiFiService
       ├── MqttService
       ├── OtaService
       ├── Pn532Service
       ├── AttendanceService
       ├── FeedbackService
       ├── HealthService
       └── PowerService

┌──────────────┐          owns          ┌──────────────┐
│     App      │ ─────────────────────► │   EventBus   │
└──────┬───────┘                        └──────────────┘
       │
       ├── owns ConfigService
       ├── owns WiFiService
       ├── owns MqttService
       ├── owns OtaService
       ├── owns Pn532Service
       ├── owns AttendanceService
       ├── owns FeedbackService
       ├── owns HealthService
       └── owns PowerService

┌──────────────┐      uses       ┌──────────────┐      uses       ┌──────────────┐
│   EventBus   │ ──────────────► │    Signal    │ ──────────────► │    Mutex     │
└──────────────┘                 └──────────────┘                 └──────────────┘

┌──────────────┐   direct ref    ┌──────────────┐
│ WiFiService  │ ──────────────► │ConfigService │
└──────────────┘                 └──────────────┘

┌──────────────┐   direct ref    ┌──────────────┐
│ Pn532Service │ ──────────────► │ConfigService │
└──────────────┘                 └──────────────┘

All runtime services publish and subscribe through the EventBus.
```

## Shared Types And Config Appendix

### Shared Config Types

The config model lives in `include/common/Config.hpp`.

| Type | Role |
|---|---|
| `Config` | root persisted configuration object |
| `WiFiConfig` | station/AP credentials and timing |
| `MqttConfig` | broker connection and topic settings |
| `DeviceConfig` | device identity and location metadata |
| `Pn532Config` | SPI/IRQ pins and NFC timing |
| `AttendanceConfig` | debounce, batching, offline queue policy |
| `FeedbackConfig` | LED/buzzer behavior |
| `HealthConfig` | health and metrics timing |
| `OtaConfig` | update source and timeout settings |
| `PowerConfig` | ESP/PN532 dual-power policy settings |

### Shared Runtime Types

The shared runtime payloads and enums live in `include/common/Types.hpp`.

| Type | Role |
|---|---|
| `Status` | operation result object |
| `SystemHealth` | aggregated health snapshot |
| `AttendanceRecord` | durable attendance entry |
| `FeedbackPattern` | LED/buzzer pattern definition |
| `Event` | bus event container |
| `CardEvent` | card UID + timestamp payload |
| `MqttEvent` | topic/payload transport payload |
| `FeedbackEvent` | feedback request payload |
| `PowerEvent` | power-state and PN532-target payload |

### Metrics Structs

| Type | Role |
|---|---|
| `MqttMetrics` | MQTT publish/receive/reconnect counters |
| `WiFiMetrics` | Wi-Fi disconnect and RSSI metrics |
| `AttendanceMetrics` | processed, debounced, batch, error counters |
| `Pn532Metrics` | read/sleep/wake/recovery counters |
| `PowerMetrics` | ESP sleep and burst-mode counters |

### Important Enums

| Enum | Meaning |
|---|---|
| `ServiceState` | service lifecycle state |
| `HealthState` | system health classification |
| `WiFiState` | network connection state |
| `MqttState` | broker connection state |
| `Pn532State` | PN532 service-level status |
| `Pn532PowerMode` | PN532 target power behavior |
| `OtaState` | OTA lifecycle state |
| `PowerState` | ESP/system power state |
| `WakeupReason` | reset/wakeup source |
| `EventType` | event routing key |

## Cross-Platform Notes

### ESP8266 vs ESP32

The codebase intentionally hides many differences behind platform headers:

- mutex implementation
- Wi-Fi power management APIs
- wakeup reason detection
- OTA include differences
- chip and heap helpers

The service layer should reason mostly in terms of shared enums and helper calls, not raw ESP-IDF or ESP8266 SDK APIs.

### Platform Divergence To Watch

- Wi-Fi sleep/power-down APIs differ
- OTA stack includes differ
- heap fragmentation reporting differs
- RTC/deep-sleep helper capabilities differ
- some low-level timing behaviors are not symmetric across boards

## Extension And Debugging Notes

### When Extending The Architecture

- add new cross-service coordination through events first, not direct references
- keep new per-service state inside the owning service
- extend shared types carefully because they affect many subscribers
- document any new runtime state machine in this manual, not only in code comments

### When Debugging

- inspect the event path first for missing coordination
- inspect service state and metrics before assuming hardware failure
- inspect PN532 IRQ behavior and power event flow for idle/wake problems
- inspect config serialization when behavior changes after reboot

### Logging

Logging is provided through macros in `include/common/Logger.hpp`. It is a support module rather than a class, but it is part of the architectural observability model because nearly every service depends on it for runtime diagnosis.

## Related Documents

- [Documentation Hub](README.md)
- [User Manual](USER_MANUAL.md)
- [Developer Manual](DEVELOPER_MANUAL.md)
- [Battery Consumption](BATTERY_CONSUMPTION.md)
- [Memory Optimization](MEMORY_OPTIMIZATION.md)
