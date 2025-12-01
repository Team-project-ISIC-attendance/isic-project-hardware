#ifndef HARDWARE_APPCONFIG_HPP
#define HARDWARE_APPCONFIG_HPP

/**
 * @file AppConfig.hpp
 * @brief Application configuration structures for the ISIC firmware.
 *
 * This file defines all configuration structures used throughout the
 * firmware. All structures use:
 * - Fixed-width integer types for predictable memory layout
 * - Brace initialization with sensible defaults
 * - `enum class` for type-safe enumerations
 *
 * @note C++20 Standard - No C++23 features.
 * @note All constexpr values are compile-time constants.
 */

#include <array>
#include <string>
#include <string_view>
#include <cstdint>

namespace isic {

    // ================== Compile-time Constants ==================
    /**
     * @brief Default values for task sizes, priorities, and queue sizes.
     *
     * All values are `inline constexpr` for C++20 compatibility and
     * to ensure single definition across translation units.
     */
    namespace defaults {
        // Task stack sizes (in words, multiply by 4 for bytes on ESP32)
        inline constexpr std::uint32_t PN532_TASK_STACK = 4096;
        inline constexpr std::uint32_t MQTT_TASK_STACK = 8192;
        inline constexpr std::uint32_t ATTENDANCE_TASK_STACK = 4096;
        inline constexpr std::uint32_t POWER_TASK_STACK = 4096;
        inline constexpr std::uint32_t HEALTH_TASK_STACK = 4096;
        inline constexpr std::uint32_t OTA_TASK_STACK = 8192;
        inline constexpr std::uint32_t EVENTBUS_TASK_STACK = 4096;
        inline constexpr std::uint32_t FEEDBACK_TASK_STACK = 2048;

        // Task priorities (higher = more urgent, ESP32 max is configMAX_PRIORITIES-1)
        inline constexpr std::uint8_t PN532_TASK_PRIORITY = 4;      // High - real-time card reading
        inline constexpr std::uint8_t ATTENDANCE_TASK_PRIORITY = 3;
        inline constexpr std::uint8_t MQTT_TASK_PRIORITY = 2;
        inline constexpr std::uint8_t POWER_TASK_PRIORITY = 2;
        inline constexpr std::uint8_t HEALTH_TASK_PRIORITY = 1;
        inline constexpr std::uint8_t OTA_TASK_PRIORITY = 1;
        inline constexpr std::uint8_t EVENTBUS_TASK_PRIORITY = 2;
        inline constexpr std::uint8_t FEEDBACK_TASK_PRIORITY = 3;   // High - responsive UX

        // Queue sizes
        inline constexpr std::size_t EVENTBUS_QUEUE_SIZE = 64;
        inline constexpr std::size_t CARD_EVENT_QUEUE_SIZE = 32;
        inline constexpr std::size_t MQTT_OUTBOUND_QUEUE_SIZE = 64;
        inline constexpr std::size_t FEEDBACK_QUEUE_SIZE = 16;
        inline constexpr std::size_t ATTENDANCE_BATCH_SIZE = 10;

        // Core assignments (ESP32 has cores 0 and 1)
        inline constexpr std::uint8_t PN532_TASK_CORE = 1;
        inline constexpr std::uint8_t MQTT_TASK_CORE = 1;
        inline constexpr std::uint8_t EVENTBUS_TASK_CORE = 0;

        // Firmware version
        inline constexpr std::string_view FIRMWARE_VERSION = "1.0.0";
    }

    // ================== WiFi Configuration ==================
    struct WifiConfig {
        std::string ssid{};
        std::string password{};
        std::uint32_t connectTimeoutMs{10000};
        std::uint8_t maxRetries{5};
        bool powerSaveEnabled{false};
    };

    // ================== MQTT Configuration ==================
    struct MqttConfig {
        std::string broker{};
        std::uint16_t port{1883};
        std::string username{};
        std::string password{};
        std::string baseTopic{"device"};
        bool tls{false};

        // Connection settings
        std::uint32_t keepAliveSeconds{60};
        std::uint32_t reconnectBackoffMinMs{1000};
        std::uint32_t reconnectBackoffMaxMs{30000};
        float reconnectBackoffMultiplier{2.0f};

        // Queue settings
        std::size_t outboundQueueSize{defaults::MQTT_OUTBOUND_QUEUE_SIZE};
        enum class QueueFullPolicy : std::uint8_t {
            DropOldest,
            DropNewest,
            Block
        };
        QueueFullPolicy queueFullPolicy{QueueFullPolicy::DropOldest};

        // Task tuning
        std::uint32_t taskStackSize{defaults::MQTT_TASK_STACK};
        std::uint8_t taskPriority{defaults::MQTT_TASK_PRIORITY};
        std::uint8_t taskCore{defaults::MQTT_TASK_CORE};
    };

    // ================== Device Configuration ==================
    struct DeviceConfig {
        std::string deviceId{"isic-esp32-001"};
        std::string locationId{"unknown"};
        std::string firmwareVersion{std::string(defaults::FIRMWARE_VERSION)};
    };

    // ================== Attendance Configuration ==================
    struct AttendanceConfig {
        std::uint32_t debounceMs{2000};
        std::size_t offlineBufferSize{256};
        std::size_t eventQueueSize{defaults::CARD_EVENT_QUEUE_SIZE};

        // High-load thresholds
        std::size_t queueHighWatermark{24};
        std::uint32_t maxEventsPerMinute{120};

        // Batching configuration
        bool batchingEnabled{true};
        std::size_t batchMaxSize{defaults::ATTENDANCE_BATCH_SIZE};
        std::uint32_t batchFlushIntervalMs{3000};  // Flush every 3 seconds
        std::uint32_t batchFlushOnIdleMs{1000};    // Flush 1s after last event

        // Task tuning
        std::uint32_t taskStackSize{defaults::ATTENDANCE_TASK_STACK};
        std::uint8_t taskPriority{defaults::ATTENDANCE_TASK_PRIORITY};
    };

    // ================== OTA Configuration ==================

    /**
     * @brief OTA update state machine states.
     *
     * State flow:
     * - Idle: Ready for update commands
     * - Checking: Querying update server for available versions
     * - Downloading: Streaming firmware to flash partition
     * - Verifying: Validating firmware checksum and headers
     * - Applying: Setting boot partition to new firmware
     * - Completed: Update successful, restart pending
     * - Failed: Update failed, see error message
     */
    enum class OtaState : std::uint8_t {
        Idle,
        Checking,
        Downloading,
        Verifying,
        Applying,
        Failed,
        Completed
    };

    [[nodiscard]] inline constexpr const char* toString(OtaState state) noexcept {
        switch (state) {
            case OtaState::Idle:        return "idle";
            case OtaState::Checking:    return "checking";
            case OtaState::Downloading: return "downloading";
            case OtaState::Verifying:   return "verifying";
            case OtaState::Applying:    return "applying";
            case OtaState::Failed:      return "failed";
            case OtaState::Completed:   return "completed";
            default:                    return "unknown";
        }
    }

    /**
     * @brief Configuration for Over-The-Air (OTA) firmware updates.
     *
     * This configuration controls all aspects of the OTA update process:
     *
     * Basic Control:
     * - enabled: Master switch for OTA functionality
     * - autoCheck: Periodically check for updates
     * - autoUpdate: Automatically apply available updates
     *
     * Server Configuration:
     * - updateServerUrl: Base URL for version check endpoint
     * - requireHttps: Enforce HTTPS for secure downloads
     *
     * Partition Layout (ESP32 with 4MB flash):
     * - Uses dual OTA partitions (ota_0, ota_1) of ~1.25MB each
     * - Bootloader selects partition based on otadata partition
     * - Failed boots trigger automatic rollback
     *
     * Security:
     * - HTTPS with optional CA certificate pinning
     * - SHA256 checksum verification
     * - Version comparison prevents accidental downgrades
     *
     * MQTT Topics:
     * - device/<id>/ota/set: Receive update commands
     * - device/<id>/ota/status: Publish current state (retained)
     * - device/<id>/ota/progress: Publish download progress
     * - device/<id>/ota/error: Publish error messages
     *
     * Example MQTT Update Command:
     * {
     *   "action": "update",
     *   "url": "https://releases.example.com/firmware/v1.2.3.bin",
     *   "version": "1.2.3",
     *   "sha256": "abc123..."  // optional
     * }
     */
    struct OtaConfig {
        // ==================== Basic Control ====================

        /** @brief Enable/disable OTA functionality */
        bool enabled{true};

        /** @brief Automatically check for updates at checkIntervalMs */
        bool autoCheck{false};

        /** @brief Automatically apply updates when found (requires autoCheck) */
        bool autoUpdate{false};

        /** @brief Interval between automatic update checks (ms) */
        std::uint32_t checkIntervalMs{3600 * 1000};  // 1 hour

        // ==================== Server Configuration ====================

        /**
         * @brief URL of update server for version checking.
         *
         * The server should respond to GET requests with JSON:
         * {
         *   "version": "1.2.3",
         *   "url": "https://releases.example.com/firmware.bin",
         *   "sha256": "abc123...",
         *   "mandatory": false,
         *   "changelog": "Bug fixes..."
         * }
         */
        std::string updateServerUrl{};

        /** @brief Require HTTPS for firmware downloads */
        bool requireHttps{true};

        /** @brief Allow version downgrades (not recommended) */
        bool allowDowngrade{false};

        // ==================== Version Information ====================

        /** @brief Current firmware version for comparison */
        std::string currentVersion{std::string(defaults::FIRMWARE_VERSION)};

        // ==================== Download Settings ====================

        /** @brief Maximum time for complete download (ms) */
        std::uint32_t downloadTimeoutMs{300000};  // 5 minutes

        /** @brief Chunk size for streaming download (bytes) */
        std::uint32_t chunkSize{4096};

        /** @brief Verify SHA256 checksum if provided */
        bool verifyChecksum{true};

        /** @brief Connection timeout for initial HTTP connection (ms) */
        std::uint32_t connectTimeoutMs{15000};

        // ==================== Retry/Backoff Settings ====================

        /** @brief Base backoff time after failure (ms) */
        std::uint32_t failureBackoffMs{300000};   // 5 minutes

        /** @brief Maximum consecutive failures before longer backoff */
        std::uint8_t maxConsecutiveFailures{3};

        /** @brief Maximum retry attempts before giving up */
        std::uint8_t maxRetryAttempts{5};

        // ==================== Update Window ====================

        /** @brief Restrict updates to specific hours */
        bool restrictUpdateWindow{false};

        /** @brief Start hour of update window (0-23, local time) */
        std::uint8_t updateWindowStartHour{2};    // 2 AM

        /** @brief End hour of update window (0-23, local time) */
        std::uint8_t updateWindowEndHour{5};      // 5 AM

        // ==================== Rollback Settings ====================

        /**
         * @brief Automatically validate partition after successful boot.
         *
         * If true, the new partition is marked valid after:
         * - Successful WiFi connection
         * - Successful MQTT connection
         * - No critical errors for validationDelayMs
         *
         * If false, partition must be manually validated via MQTT command.
         */
        bool autoValidatePartition{true};

        /** @brief Delay before marking partition valid (ms) */
        std::uint32_t validationDelayMs{30000};   // 30 seconds

        // ==================== Task Settings ====================

        /** @brief Stack size for OTA task (bytes) */
        std::uint32_t taskStackSize{defaults::OTA_TASK_STACK};

        /** @brief Priority for OTA task (lower = less priority) */
        std::uint8_t taskPriority{defaults::OTA_TASK_PRIORITY};
    };

    // ================== PN532 Configuration ==================
    struct Pn532Config {
        // Pin configuration
        std::uint8_t irqPin{4};
        std::uint8_t resetPin{5};

        // Polling settings
        std::uint32_t pollIntervalMs{100};
        std::uint32_t cardReadTimeoutMs{500};

        // Health monitoring
        std::uint32_t healthCheckIntervalMs{5000};
        std::uint32_t communicationTimeoutMs{1000};
        std::uint8_t maxConsecutiveErrors{5};
        std::uint32_t recoveryDelayMs{2000};
        std::uint8_t maxRecoveryAttempts{3};

        // Wake configuration
        bool wakeOnCardEnabled{true};

        // Task tuning
        std::uint32_t taskStackSize{defaults::PN532_TASK_STACK};
        std::uint8_t taskPriority{defaults::PN532_TASK_PRIORITY};
        std::uint8_t taskCore{defaults::PN532_TASK_CORE};
    };

    // ================== Power/Sleep Configuration ==================
    struct PowerConfig {
        bool sleepEnabled{true};

        enum class SleepType : std::uint8_t {
            None,
            Light,
            Deep
        };
        SleepType sleepType{SleepType::Light};

        std::uint32_t idleTimeoutMs{30000};
        std::uint32_t wakeCheckIntervalMs{100};

        // Wake sources
        bool wakeSourcePn532Enabled{true};
        bool wakeSourceTimerEnabled{true};
        std::uint32_t timerWakeIntervalMs{60000};

        // Power save modes during active operation
        bool wifiPowerSaveEnabled{false};
        std::uint8_t cpuFrequencyMhz{160};  // 80, 160, or 240

        // Task tuning
        std::uint32_t taskStackSize{defaults::POWER_TASK_STACK};
        std::uint8_t taskPriority{defaults::POWER_TASK_PRIORITY};
    };

    // ================== User Feedback Configuration ==================
    struct FeedbackConfig {
        bool enabled{true};

        // LED configuration
        bool ledEnabled{true};
        std::uint8_t ledSuccessPin{2};      // Usually built-in LED
        std::uint8_t ledErrorPin{15};        // Separate error LED (optional)
        bool ledActiveHigh{true};            // true = HIGH turns LED on

        // Buzzer configuration
        bool buzzerEnabled{true};
        std::uint8_t buzzerPin{12};

        // Timing (in milliseconds)
        std::uint16_t successBlinkMs{100};
        std::uint16_t successBeepMs{50};
        std::uint16_t errorBlinkMs{200};
        std::uint16_t errorBeepCount{3};
        std::uint16_t processingPulseMs{500};

        // Buzzer frequency
        std::uint16_t beepFrequencyHz{2000};

        // Task tuning
        std::uint32_t taskStackSize{defaults::FEEDBACK_TASK_STACK};
        std::uint8_t taskPriority{defaults::FEEDBACK_TASK_PRIORITY};
        std::size_t queueSize{defaults::FEEDBACK_QUEUE_SIZE};
    };

    // ================== Health Monitoring Configuration ==================
    struct HealthConfig {
        std::uint32_t checkIntervalMs{10000};
        std::uint32_t reportIntervalMs{60000};
        bool publishToMqtt{true};
        bool logToSerial{true};

        // Component-specific thresholds
        std::uint32_t mqttUnhealthyAfterMs{30000};
        std::uint32_t wifiUnhealthyAfterMs{60000};

        // Task tuning
        std::uint32_t taskStackSize{defaults::HEALTH_TASK_STACK};
        std::uint8_t taskPriority{defaults::HEALTH_TASK_PRIORITY};
    };

    // ================== EventBus Configuration ==================
    struct EventBusConfig {
        std::size_t queueSize{defaults::EVENTBUS_QUEUE_SIZE};
        std::uint32_t taskStackSize{defaults::EVENTBUS_TASK_STACK};
        std::uint8_t taskPriority{defaults::EVENTBUS_TASK_PRIORITY};
        std::uint8_t taskCore{defaults::EVENTBUS_TASK_CORE};
    };

    // ================== Logging Configuration ==================
    struct LogConfig {
        enum class Level : std::uint8_t {
            Trace = 0,
            Debug = 1,
            Info = 2,
            Warn = 3,
            Error = 4,
            None = 5
        };
        Level serialLevel{Level::Info};
        Level mqttLevel{Level::Warn};
        bool includeTimestamps{true};
        bool colorOutput{true};
    };

    // ================== Module Enable Configuration ==================
    struct ModulesConfig {
        bool attendanceEnabled{true};
        bool otaEnabled{true};
        // Future modules can be added here
        // bool diagnosticsEnabled{false};
        // bool remoteDebugEnabled{false};
    };

    // ================== Main Application Configuration ==================
    struct AppConfig {
        WifiConfig wifi{};
        MqttConfig mqtt{};
        DeviceConfig device{};
        AttendanceConfig attendance{};
        OtaConfig ota{};
        Pn532Config pn532{};
        PowerConfig power{};
        FeedbackConfig feedback{};
        HealthConfig health{};
        EventBusConfig eventBus{};
        LogConfig log{};
        ModulesConfig modules{};

        [[nodiscard]] bool validate() const noexcept {
            if (wifi.ssid.empty()) {
                return false;
            }

            if (mqtt.broker.empty()) {
                return false;
            }

            if (device.deviceId.empty()) {
                return false;
            }

            if (attendance.offlineBufferSize == 0) {
                return false;
            }

            if (attendance.debounceMs < 100) {
                return false;  // Minimum debounce of 100ms
            }

            if (pn532.pollIntervalMs < 10) {
                return false;  // Minimum poll interval
            }

            if (power.idleTimeoutMs < 1000) {
                return false;  // Minimum idle timeout
            }

            if (attendance.batchMaxSize == 0 || attendance.batchMaxSize > 100) {
                return false;  // Reasonable batch limits
            }

            return true;
        }

        static AppConfig makeDefault() noexcept {
            AppConfig cfg{};

            // WiFi defaults
            cfg.wifi.ssid = "CHANGE_ME";
            cfg.wifi.password = "CHANGE_ME";
            cfg.wifi.connectTimeoutMs = 10000;
            cfg.wifi.maxRetries = 5;

            // MQTT defaults
            cfg.mqtt.broker = "192.168.1.10";
            cfg.mqtt.port = 1883;
            cfg.mqtt.baseTopic = "device";
            cfg.mqtt.keepAliveSeconds = 60;
            cfg.mqtt.reconnectBackoffMinMs = 1000;
            cfg.mqtt.reconnectBackoffMaxMs = 30000;
            cfg.mqtt.outboundQueueSize = defaults::MQTT_OUTBOUND_QUEUE_SIZE;

            // Device defaults
            cfg.device.deviceId = "isic-esp32-001";
            cfg.device.locationId = "lab-1";
            cfg.device.firmwareVersion = std::string(defaults::FIRMWARE_VERSION);

            // Attendance defaults
            cfg.attendance.debounceMs = 2000;
            cfg.attendance.offlineBufferSize = 256;
            cfg.attendance.eventQueueSize = defaults::CARD_EVENT_QUEUE_SIZE;
            cfg.attendance.queueHighWatermark = 24;
            cfg.attendance.batchingEnabled = true;
            cfg.attendance.batchMaxSize = defaults::ATTENDANCE_BATCH_SIZE;
            cfg.attendance.batchFlushIntervalMs = 3000;

            // OTA defaults
            cfg.ota.enabled = true;
            cfg.ota.autoCheck = false;
            cfg.ota.autoUpdate = false;
            cfg.ota.checkIntervalMs = 3600 * 1000;  // 1 hour
            cfg.ota.requireHttps = true;
            cfg.ota.allowDowngrade = false;
            cfg.ota.downloadTimeoutMs = 300000;     // 5 minutes
            cfg.ota.chunkSize = 4096;
            cfg.ota.verifyChecksum = true;
            cfg.ota.connectTimeoutMs = 15000;
            cfg.ota.failureBackoffMs = 300000;
            cfg.ota.maxConsecutiveFailures = 3;
            cfg.ota.maxRetryAttempts = 5;
            cfg.ota.autoValidatePartition = true;
            cfg.ota.validationDelayMs = 30000;

            // PN532 defaults
            cfg.pn532.irqPin = 4;
            cfg.pn532.resetPin = 5;
            cfg.pn532.pollIntervalMs = 100;
            cfg.pn532.healthCheckIntervalMs = 5000;
            cfg.pn532.maxConsecutiveErrors = 5;
            cfg.pn532.wakeOnCardEnabled = true;

            // Power defaults
            cfg.power.sleepEnabled = true;
            cfg.power.sleepType = PowerConfig::SleepType::Light;
            cfg.power.idleTimeoutMs = 30000;
            cfg.power.wakeSourcePn532Enabled = true;
            cfg.power.wakeSourceTimerEnabled = true;
            cfg.power.timerWakeIntervalMs = 60000;
            cfg.power.cpuFrequencyMhz = 160;

            // Feedback defaults
            cfg.feedback.enabled = true;
            cfg.feedback.ledEnabled = true;
            cfg.feedback.buzzerEnabled = true;
            cfg.feedback.ledSuccessPin = 2;
            cfg.feedback.buzzerPin = 12;

            // Health defaults
            cfg.health.checkIntervalMs = 10000;
            cfg.health.reportIntervalMs = 60000;
            cfg.health.publishToMqtt = true;
            cfg.health.logToSerial = true;

            // Log defaults
            cfg.log.serialLevel = LogConfig::Level::Info;
            cfg.log.mqttLevel = LogConfig::Level::Warn;
            cfg.log.includeTimestamps = true;
            cfg.log.colorOutput = true;

            // Module defaults
            cfg.modules.attendanceEnabled = true;
            cfg.modules.otaEnabled = true;

            return cfg;
        }
    };

}  // namespace isic

#endif  // HARDWARE_APPCONFIG_HPP
