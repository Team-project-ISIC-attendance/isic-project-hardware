# ISIC Attendance System - ESP32 Hardware Firmware

Production-grade ESP32 firmware for ISIC card-based attendance tracking with NFC, MQTT, power management, and health monitoring.

## Features

- **NFC Card Reading**: PN532-based ISIC card scanning with configurable debounce
- **MQTT Integration**: Async publishing with offline buffering and backpressure handling
- **Power Management**: Light/deep sleep with wake-lock mechanism and PN532 wake support
- **Health Monitoring**: Comprehensive component health tracking and reporting
- **OTA Updates**: Over-the-air firmware updates via MQTT commands
- **Event-Driven Architecture**: Central EventBus for decoupled communication
- **High Throughput**: Non-blocking design for handling many cards per minute

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        EventBus                             │
│  (Central message hub for all events)                       │
└─────────────────────────────────────────────────────────────┘
        │                    │                    │
        ▼                    ▼                    ▼
┌─────────────┐    ┌─────────────────┐    ┌─────────────┐
│ ConfigService│    │  PowerService   │    │HealthMonitor│
│ (NVS storage)│    │ (Sleep/wake)    │    │ (Status)    │
└─────────────┘    └─────────────────┘    └─────────────┘
        │                    │
        ▼                    ▼
┌─────────────┐    ┌─────────────────┐    ┌─────────────┐
│ MqttService │    │  Pn532Driver    │    │  OtaService │
│ (Async queue)│    │ (NFC + health)  │    │ (Updates)   │
└─────────────┘    └─────────────────┘    └─────────────┘
        │                    │
        └────────┬───────────┘
                 ▼
        ┌─────────────────┐
        │AttendanceModule │
        │(Debounce, queue)│
        └─────────────────┘
```

## Build Targets

```
# ESP32 Development Kit (default)
pio run -e esp32dev

# ESP-12F (ESP8266) Production
pio run -e esp12f
```

### Conditional Compilation

```cpp
#ifdef HW_TARGET_ESP32
// ESP32-specific code (dual-core, FreeRTOS, etc.)
#elif defined(HW_TARGET_ESP8266)
// ESP8266-specific code (single core, lighter footprint)
#endif

#ifdef PROD_BUILD
// Production: reduced logging, no debug APIs
#else
// Development: verbose logging, test endpoints
#endif
```

## Configuration

### Via NVS (Persistent Storage)

Configuration is stored in ESP32 NVS and loaded at boot. Modify via MQTT or code.

### Via MQTT

Publish to `device/<device_id>/config/set`:

```json
{
  "wifi": {
    "ssid": "MyNetwork",
    "password": "secret123"
  },
  "mqtt": {
    "broker": "mqtt.example.com",
    "port": 1883,
    "username": "device",
    "password": "secret"
  },
  "device": {
    "deviceId": "isic-esp32-lab1",
    "locationId": "building-a-room-101"
  },
  "attendance": {
    "debounceMs": 2000,
    "offlineBufferSize": 256
  },
  "pn532": {
    "pollIntervalMs": 100,
    "healthCheckIntervalMs": 5000,
    "maxConsecutiveErrors": 5
  },
  "power": {
    "sleepEnabled": true,
    "sleepType": "light",
    "idleTimeoutMs": 30000,
    "wakeSourcePn532Enabled": true
  },
  "health": {
    "checkIntervalMs": 10000,
    "reportIntervalMs": 60000,
    "publishToMqtt": true
  }
}
```

## MQTT Topics

### Subscriptions (Device Listens)

| Topic | Description |
|-------|-------------|
| `device/<id>/config/set` | Configuration updates (JSON) |
| `device/<id>/ota/set` | OTA update commands (JSON) |
| `device/<id>/modules/+/set` | Module-specific commands |

### Publications (Device Publishes)

| Topic | Description | Retained |
|-------|-------------|----------|
| `device/<id>/status` | Device online/offline status | Yes |
| `device/<id>/attendance` | Card scan events | No |
| `device/<id>/attendance/batch` | Batched card events | No |
| `device/<id>/status/health` | Health monitoring reports | No |
| `device/<id>/status/pn532` | PN532 status and metrics | No |
| `device/<id>/ota/status` | OTA state changes | Yes |
| `device/<id>/ota/progress` | OTA download progress | No |
| `device/<id>/ota/error` | OTA error messages | No |
| `device/<id>/metrics` | System metrics | No |

### Example Messages

#### Attendance Event
```json
{
  "card_id": "04A5B7C8D9E0F112",
  "timestamp_ms": 1699876543210,
  "device_id": "isic-esp32-lab1",
  "location_id": "building-a-room-101",
  "sequence": 42
}
```

#### Health Report
```json
{
  "overall": "healthy",
  "uptime_s": 3600,
  "free_heap_kb": 180,
  "min_heap_kb": 150,
  "device_id": "isic-esp32-lab1",
  "firmware": "1.0.0",
  "counts": {
    "healthy": 3,
    "degraded": 0,
    "unhealthy": 0,
    "unknown": 0
  },
  "components": [
    {"name": "MQTT", "state": "healthy"},
    {"name": "PN532", "state": "healthy"},
    {"name": "Attendance", "state": "healthy"}
  ]
}
```

## OTA (Over-The-Air) Updates

The firmware includes a production-grade OTA update system with the following features:

- **Dual-partition OTA**: Two app partitions (ota_0, ota_1) for safe updates
- **Automatic rollback**: Failed boots trigger automatic rollback to previous firmware
- **HTTPS support**: Secure downloads with optional certificate pinning
- **SHA256 verification**: Firmware integrity checking
- **Progress reporting**: Real-time download progress via MQTT
- **Non-blocking**: Updates run in background, card reading continues

### Partition Layout

```
┌────────────┬──────────────┬───────────┬────────────────────────────────────┐
│ Partition  │ Offset       │ Size      │ Description                        │
├────────────┼──────────────┼───────────┼────────────────────────────────────┤
│ nvs        │ 0x9000       │ 20KB      │ Non-volatile storage (config)      │
│ otadata    │ 0xE000       │ 8KB       │ OTA boot selection metadata        │
│ app0       │ 0x10000      │ 1.25MB    │ OTA slot 0 (ota_0)                 │
│ app1       │ 0x150000     │ 1.25MB    │ OTA slot 1 (ota_1)                 │
│ spiffs     │ 0x290000     │ 1.4MB     │ Filesystem for logs/data           │
└────────────┴──────────────┴───────────┴────────────────────────────────────┘
```

### MQTT OTA Topics

| Topic | Direction | Description |
|-------|-----------|-------------|
| `device/<id>/ota/set` | Subscribe | Receive OTA commands |
| `device/<id>/ota/status` | Publish | Current OTA state (retained) |
| `device/<id>/ota/progress` | Publish | Download progress updates |
| `device/<id>/ota/error` | Publish | Error messages |

### OTA Commands

#### 1. Trigger Firmware Update

```json
{
  "action": "update",
  "url": "https://releases.example.com/firmware/v1.2.3.bin",
  "version": "1.2.3",
  "sha256": "a1b2c3d4e5f6...",
  "force": false
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `action` | Yes | Must be `"update"` |
| `url` | Yes | HTTPS URL of firmware binary |
| `version` | No | Version string for comparison |
| `sha256` | No | Expected SHA256 hash (hex string) |
| `force` | No | Skip version check if `true` |

#### 2. Check for Updates

```json
{ "action": "check" }
```

Queries the configured update server for available versions.

#### 3. Cancel In-Progress Update

```json
{ "action": "cancel" }
```

Aborts the current download and returns to idle state.

#### 4. Rollback to Previous Firmware

```json
{ "action": "rollback" }
```

Switches boot partition to the previous firmware and restarts.

#### 5. Mark Current Partition Valid

```json
{ "action": "mark_valid" }
```

Confirms the current firmware is working correctly, preventing automatic rollback.

#### 6. Get Current Status

```json
{ "action": "get_status" }
```

Publishes current OTA state to the status topic.

### OTA Status Messages

Status is published to `device/<id>/ota/status` (retained):

```json
{
  "old_state": "idle",
  "new_state": "downloading",
  "message": "Downloading firmware...",
  "timestamp_ms": 1699876543210,
  "device_id": "isic-esp32-001",
  "firmware_version": "1.0.0"
}
```

**State Values:**
- `idle` - Ready for update commands
- `checking` - Querying update server
- `downloading` - Streaming firmware to flash
- `verifying` - Validating firmware checksum
- `applying` - Setting boot partition
- `completed` - Update successful, restart pending
- `failed` - Update failed, see error message

### OTA Progress Messages

Progress is published to `device/<id>/ota/progress`:

```json
{
  "percent": 45,
  "bytes_downloaded": 512000,
  "total_bytes": 1138688,
  "remaining_bytes": 626688,
  "success": false,
  "message": "Downloading: 45%",
  "device_id": "isic-esp32-001"
}
```

### OTA Error Messages

Errors are published to `device/<id>/ota/error`:

```json
{
  "error": "Download timeout after 300 seconds",
  "timestamp_ms": 1699876543210,
  "device_id": "isic-esp32-001"
}
```

**Common Error Codes:**
- `wifi_disconnected` - No network connection
- `http_not_found` - Firmware URL returned 404
- `checksum_mismatch` - SHA256 verification failed
- `image_too_large` - Firmware exceeds partition size
- `write_error` - Flash write failed
- `verification_failed` - Firmware header invalid

### Boot Validation & Rollback

After an OTA update, the new firmware boots in "pending validation" state:

1. **Successful Boot**: If the device boots successfully and runs for 30 seconds without critical errors, the partition is automatically marked as valid.

2. **Failed Boot**: If the device fails to boot or crashes repeatedly, the ESP32 bootloader automatically rolls back to the previous firmware.

3. **Manual Validation**: Send `{"action": "mark_valid"}` to manually confirm the firmware is working.

### Configuration Options

```cpp
struct OtaConfig {
    bool enabled{true};              // Enable OTA functionality
    bool autoCheck{false};           // Periodically check for updates
    bool autoUpdate{false};          // Auto-install available updates
    std::uint32_t checkIntervalMs{3600000};  // Check interval (1 hour)
    std::string updateServerUrl{};   // Version check endpoint
    bool requireHttps{true};         // Require HTTPS for downloads
    bool allowDowngrade{false};      // Allow installing older versions
    std::uint32_t downloadTimeoutMs{300000}; // Download timeout (5 min)
    bool verifyChecksum{true};       // Verify SHA256 if provided
    std::uint32_t failureBackoffMs{300000};  // Backoff after failure
    std::uint8_t maxRetryAttempts{5};// Max retry attempts
    bool autoValidatePartition{true};// Auto-validate on successful boot
};
```

### Example: Triggering OTA via MQTT

Using mosquitto_pub:

```bash
# Trigger update
mosquitto_pub -h mqtt.example.com -t "device/isic-esp32-001/ota/set" \
  -m '{"action":"update","url":"https://releases.example.com/v1.2.3.bin","version":"1.2.3"}'

# Check status
mosquitto_sub -h mqtt.example.com -t "device/isic-esp32-001/ota/status" -v

# Watch progress
mosquitto_sub -h mqtt.example.com -t "device/isic-esp32-001/ota/progress" -v

# Rollback if needed
mosquitto_pub -h mqtt.example.com -t "device/isic-esp32-001/ota/set" \
  -m '{"action":"rollback"}'
```

### Expected Logs During OTA

```
[I][OtaService] OTA update triggered:
[I][OtaService]   URL: https://releases.example.com/v1.2.3.bin
[I][OtaService]   Version: 1.2.3
[I][OtaService] State: idle -> downloading - Connecting to server...
[I][OtaService] Connecting to: https://releases.example.com/v1.2.3.bin
[I][OtaService] Firmware size: 1138688 bytes
[I][OtaService] Writing to partition: app1 @ 0x150000
[D][OtaService] Progress: 10% (113868/1138688 bytes, 45 KB/s)
[D][OtaService] Progress: 20% (227737/1138688 bytes, 48 KB/s)
...
[I][OtaService] Download complete: 1138688 bytes in 25432 ms
[I][OtaService] State: downloading -> verifying - Verifying firmware...
[I][OtaService] Firmware verification passed
[I][OtaService] State: verifying -> applying - Setting boot partition...
[I][OtaService] Boot partition set to: app1
[I][OtaService] State: applying -> completed - Update complete, restarting...
[I][OtaService] Restarting to apply update...
```

### Hosting Firmware

For production deployments, host firmware binaries on:

1. **Static file server** (S3, CloudFront, nginx)
2. **Firmware management service** with version tracking
3. **GitHub Releases** for open-source projects

Example server endpoint for version checking:

```
GET /api/firmware/version?device=isic-esp32-001&current=1.0.0

Response:
{
  "version": "1.2.3",
  "url": "https://releases.example.com/firmware/v1.2.3.bin",
  "sha256": "a1b2c3d4e5f6...",
  "mandatory": false,
  "changelog": "- Bug fixes\n- Performance improvements"
}
```

## Power Management

### Sleep Modes

- **Light Sleep**: CPU paused, RAM retained, ~1ms wake latency
- **Deep Sleep**: Full power down, ~200ms wake latency, requires reboot

### Wake Sources

1. **PN532 Interrupt**: Card presented to NFC reader
2. **Timer**: Periodic wake for health checks
3. **GPIO**: External trigger

### Wake Locks

Services acquire wake locks to prevent sleep during critical operations:

```cpp
// Acquire lock
auto lock = powerService.requestWakeLock("mqtt_publish");

// ... do work ...

// Release lock (or use ScopedWakeLock for RAII)
powerService.releaseWakeLock(lock);
```

## Health Monitoring

### Component States

| State | Description |
|-------|-------------|
| `healthy` | Operating normally |
| `degraded` | Functional but with issues |
| `unhealthy` | Not functional |
| `unknown` | State not determined |

### PN532 Status States

| State | Description |
|-------|-------------|
| `uninitialized` | Not yet started |
| `initializing` | Hardware init in progress |
| `ready` | Ready for card reads |
| `error` | Communication error |
| `offline` | Hardware not responding |
| `recovering` | Attempting recovery |

## Development

### Prerequisites

- PlatformIO Core or VS Code with PlatformIO extension
- ESP32 development board
- PN532 NFC module (SPI mode)

### Pin Configuration

Default ESP32 pins:
- **SPI SCK**: GPIO 18
- **SPI MISO**: GPIO 19
- **SPI MOSI**: GPIO 23
- **SPI SS (PN532)**: GPIO 5
- **PN532 IRQ**: GPIO 4
- **PN532 RST**: GPIO 5

### Building

```bash
# Build for ESP32
pio run -e esp32dev

# Upload to device
pio run -e esp32dev -t upload

# Monitor serial output
pio device monitor
```

### Code Style & C++20 Standards

This project uses **C++20** (`-std=gnu++20`) targeting embedded microcontrollers (ESP32/ESP8266).

#### Language Standard
- **C++20** is the target standard (no C++23-only features)
- Custom `Result<T>` type instead of C++23's `std::expected`
- Embedded-safe subset of the standard library

#### Coding Conventions
- 4-space indentation
- Opening braces on same line
- `snake_case` for variables, `PascalCase` for types
- `m_` prefix for member variables
- Brace initialization `{}` for all variables

#### C++20 Features Used
- Fixed-width integers (`std::uint8_t`, `std::uint32_t`, etc.)
- `enum class` for all enumerations
- `std::span` for buffer views
- `std::optional` instead of magic values
- `std::variant` for event payloads
- `std::string_view` for non-owning strings
- `constexpr` for compile-time constants
- `[[nodiscard]]` for return values that must be checked
- `noexcept` for functions that don't throw

#### Embedded-Safe Guidelines
- Exceptions disabled (`-fno-exceptions`)
- Minimal heap allocation in hot paths
- `std::array` preferred over `std::vector`
- Static/stack allocation for services and drivers
- No `<thread>`, `<filesystem>`, `<future>` usage

## File Structure

```
├── include/
│   ├── AppConfig.hpp           # Configuration structures
│   ├── core/
│   │   ├── EventBus.hpp        # Central event system
│   │   ├── Events.hpp          # Event types and payloads
│   │   ├── IHealthCheck.hpp    # Health monitoring interface
│   │   ├── IModule.hpp         # Module interface
│   │   ├── Logger.hpp          # Logging macros
│   │   ├── ModuleManager.hpp   # Module lifecycle
│   │   └── Result.hpp          # Error handling types
│   ├── drivers/
│   │   └── Pn532Driver.hpp     # NFC driver with health
│   ├── modules/
│   │   ├── AttendanceModule.hpp
│   │   └── OtaModule.hpp
│   └── services/
│       ├── ConfigService.hpp   # NVS config management
│       ├── HealthMonitorService.hpp
│       ├── MqttService.hpp     # Async MQTT client
│       ├── OtaService.hpp
│       └── PowerService.hpp    # Sleep/wake management
├── src/
│   ├── main.cpp                # Entry point
│   ├── core/
│   │   ├── EventBus.cpp
│   │   └── ModuleManager.cpp
│   ├── drivers/
│   │   └── Pn532Driver.cpp
│   ├── modules/
│   │   ├── AttendanceModule.cpp
│   │   └── OtaModule.cpp
│   └── services/
│       ├── ConfigService.cpp
│       ├── HealthMonitorService.cpp
│       ├── MqttService.cpp
│       ├── OtaService.cpp
│       └── PowerService.cpp
├── platformio.ini              # Build configuration
├── partitions_ota_esp32.csv    # Flash partition layout
└── README.md
```

## License

Proprietary - ISIC Attendance System Project
