#ifndef HARDWARE_EVENTS_HPP
#define HARDWARE_EVENTS_HPP

/**
 * @file Types.hpp
 * @brief Event types and payloads for the EventBus system.
 *
 * This file defines all events used for inter-component communication.
 *
 * @note All event structs use brace initialization and are trivially copyable.
 */

#include <array>
#include <string>
#include <variant>
#include <cstdint>
#include <string_view>

// Forward declaration to avoid circular dependency
namespace isic {
    struct AppConfig;
    enum class OtaState : std::uint8_t;
}

namespace isic {
    //------------------------ Card Identifiers ------------------------
    constexpr auto CARD_ID_SIZE{7};
    using CardId = std::array<std::uint8_t, CARD_ID_SIZE>;

    //------------------------ Health States ------------------------
    enum class HealthState : std::uint8_t {
        Healthy,
        Degraded,
        Unhealthy,
        Unknown,
    };
    [[nodiscard]] constexpr const char *toString(const HealthState state) noexcept {
        switch (state) {
            case HealthState::Healthy:   return "healthy";
            case HealthState::Degraded:  return "degraded";
            case HealthState::Unhealthy: return "unhealthy";
            default:                     return "unknown";
        }
    }

    // ------------------------ PN532 Status ------------------------
    enum class Pn532Status : std::uint8_t {
        Uninitialized,
        Initializing,
        Ready,
        Error,
        Offline,
        Recovering,
    };
    [[nodiscard]] constexpr const char *toString(const Pn532Status status) noexcept {
        switch (status) {
            case Pn532Status::Uninitialized: return "uninitialized";
            case Pn532Status::Initializing:  return "initializing";
            case Pn532Status::Ready:         return "ready";
            case Pn532Status::Error:         return "error";
            case Pn532Status::Offline:       return "offline";
            case Pn532Status::Recovering:    return "recovering";
            default:                         return "unknown";
        }
    }

    // ------------------------ Card Read Errors ------------------------
    enum class CardReadError : std::uint8_t {
        None,
        Timeout,
        CrcError,
        CommunicationError,
        InvalidCardFormat,
        CardRemoved,
        InternalError,
    };
    [[nodiscard]] constexpr const char *toString(const CardReadError error) noexcept {
        switch (error) {
            case CardReadError::None:               return "none";
            case CardReadError::Timeout:            return "timeout";
            case CardReadError::CrcError:           return "crc_error";
            case CardReadError::CommunicationError: return "comm_error";
            case CardReadError::InvalidCardFormat:  return "invalid_format";
            case CardReadError::CardRemoved:        return "card_removed";
            case CardReadError::InternalError:      return "internal_error";
            default:                                return "unknown";
        }
    }

    // ------------------------ Power States ------------------------
    enum class PowerState : std::uint8_t {
        Active,
        Idle,
        LightSleep,
        DeepSleep,
        WakingUp,
    };
    [[nodiscard]] constexpr const char *toString(const PowerState state) noexcept {
        switch (state) {
            case PowerState::Active:     return "active";
            case PowerState::Idle:       return "idle";
            case PowerState::LightSleep: return "light_sleep";
            case PowerState::DeepSleep:  return "deep_sleep";
            case PowerState::WakingUp:   return "waking_up";
            default:                     return "unknown";
        }
    }

    // ------------------------ Wakeup Reasons ------------------------
    enum class WakeupReason : std::uint8_t {
        PowerOn,
        Timer,
        Pn532Interrupt,
        GpioPin,
        TouchPad,
        UlpCoprocessor,
        Unknown,
    };

    [[nodiscard]] inline constexpr const char *toString(const WakeupReason reason) noexcept {
        switch (reason) {
            case WakeupReason::PowerOn:         return "power_on";
            case WakeupReason::Timer:           return "timer";
            case WakeupReason::Pn532Interrupt:  return "pn532_interrupt";
            case WakeupReason::GpioPin:         return "gpio_pin";
            case WakeupReason::TouchPad:        return "touch_pad";
            case WakeupReason::UlpCoprocessor:  return "ulp_coprocessor";
            default:                            return "unknown";
        }
    }

    // ------------------------ Feedback Signals ------------------------
    enum class FeedbackSignal : std::uint8_t {
        Success,      // Card read success
        Error,        // Read error
        Processing,   // Busy/processing
        Connected,    // Network connected
        Disconnected, // Network lost
        OtaStarted,   // OTA update starting
        OtaComplete,  // OTA complete
        Custom,       // Custom pattern
    };
    [[nodiscard]] constexpr const char *toString(const FeedbackSignal signal) noexcept {
        switch (signal) {
            case FeedbackSignal::Success:      return "success";
            case FeedbackSignal::Error:        return "error";
            case FeedbackSignal::Processing:   return "processing";
            case FeedbackSignal::Connected:    return "connected";
            case FeedbackSignal::Disconnected: return "disconnected";
            case FeedbackSignal::OtaStarted:   return "ota_started";
            case FeedbackSignal::OtaComplete:  return "ota_complete";
            case FeedbackSignal::Custom:       return "custom";
            default:                           return "unknown";
        }
    }

    // ------------------------ Event Types ------------------------
    enum class EventType : std::uint8_t {
        // Card/Attendance Events
        CardScanned,
        CardReadError,
        AttendanceRecorded,
        AttendanceBatchReady,

        // Config Events
        ConfigUpdated,

        // MQTT Events
        MqttConnected,
        MqttDisconnected,
        MqttMessageReceived,
        MqttQueueOverflow,

        // OTA Events
        OtaRequested,
        OtaStateChanged,
        OtaProgress,
        OtaVersionInfo,

        // PN532 Events
        Pn532StatusChanged,
        Pn532Error,
        Pn532Recovered,
        Pn532CardPresent,
        Pn532CardRemoved,

        // Power Events
        PowerStateChanged,
        SleepEntering,
        WakeupOccurred,
        WakeLockAcquired,
        WakeLockReleased,

        // Health Events
        HealthStatusChanged,
        HighLoadDetected,
        QueueOverflow,

        // Feedback Events
        FeedbackRequested,

        // System Events
        Heartbeat,
        SystemError,
        SystemWarning,
        ModuleStateChanged,

        Count
    };
    [[nodiscard]] constexpr const char *toString(const EventType type) noexcept {
        switch (type) {
            case EventType::CardScanned:            return "card_scanned";
            case EventType::CardReadError:          return "card_read_error";
            case EventType::AttendanceRecorded:     return "attendance_recorded";
            case EventType::AttendanceBatchReady:   return "attendance_batch_ready";
            case EventType::ConfigUpdated:          return "config_updated";
            case EventType::MqttConnected:          return "mqtt_connected";
            case EventType::MqttDisconnected:       return "mqtt_disconnected";
            case EventType::MqttMessageReceived:    return "mqtt_message_received";
            case EventType::MqttQueueOverflow:      return "mqtt_queue_overflow";
            case EventType::OtaRequested:           return "ota_requested";
            case EventType::OtaStateChanged:        return "ota_state_changed";
            case EventType::OtaProgress:            return "ota_progress";
            case EventType::OtaVersionInfo:         return "ota_version_info";
            case EventType::Pn532StatusChanged:     return "pn532_status_changed";
            case EventType::Pn532Error:             return "pn532_error";
            case EventType::Pn532Recovered:         return "pn532_recovered";
            case EventType::Pn532CardPresent:       return "pn532_card_present";
            case EventType::Pn532CardRemoved:       return "pn532_card_removed";
            case EventType::PowerStateChanged:      return "power_state_changed";
            case EventType::SleepEntering:          return "sleep_entering";
            case EventType::WakeupOccurred:         return "wakeup_occurred";
            case EventType::WakeLockAcquired:       return "wake_lock_acquired";
            case EventType::WakeLockReleased:       return "wake_lock_released";
            case EventType::HealthStatusChanged:    return "health_status_changed";
            case EventType::HighLoadDetected:       return "high_load_detected";
            case EventType::QueueOverflow:          return "queue_overflow";
            case EventType::FeedbackRequested:      return "feedback_requested";
            case EventType::Heartbeat:              return "heartbeat";
            case EventType::SystemError:            return "system_error";
            case EventType::SystemWarning:          return "system_warning";
            case EventType::ModuleStateChanged:     return "module_state_changed";
            default:                                return "unknown";
        }
    }

    // ------------------------ Attendance Records and Batches ------------------------
    struct AttendanceRecord {
        CardId cardId{};
        std::uint64_t timestampMs{0};
        std::uint32_t sequenceNumber{0};
        std::string deviceId{};
        std::string locationId{};
    };

    struct AttendanceBatch {
        static constexpr std::uint32_t MAX_BATCH_SIZE{35};

        std::array<AttendanceRecord, MAX_BATCH_SIZE> records{};
        std::uint64_t firstTimestampMs{0};
        std::uint64_t lastTimestampMs{0};
        std::uint32_t count{0};

        [[nodiscard]] constexpr bool isFull() const noexcept {
            return count >= MAX_BATCH_SIZE;
        }

        [[nodiscard]] constexpr bool isEmpty() const noexcept {
            return count == 0;
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return count;
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept {
            return MAX_BATCH_SIZE;
        }

        constexpr void add(const AttendanceRecord &record) noexcept {
            if (count < MAX_BATCH_SIZE) {
                if (count == 0) {
                    firstTimestampMs = record.timestampMs;
                }

                records[count++] = record;
                lastTimestampMs = record.timestampMs;
            }
        }

        constexpr void clear() noexcept {
            count = 0;
            firstTimestampMs = 0;
            lastTimestampMs = 0;
        }
    };

    // ------------------------ Event Payloads ------------------------
    struct CardScannedEvent {
        CardId cardId{};
        std::uint64_t timestampMs{0};
    };
    struct CardReadErrorEvent {
        CardReadError error{CardReadError::None};
        std::uint64_t timestampMs{0};
    };
    struct AttendanceBatchReadyEvent {
        std::size_t recordCount{0};
        std::uint64_t timestampMs{0};
    };
    struct ConfigUpdatedEvent {
        const AppConfig *config{nullptr};
    };
    struct MqttMessageEvent {
        std::string topic{};
        std::string payload{};
    };
    struct MqttQueueOverflowEvent {
        std::size_t droppedCount{0};
        std::size_t currentQueueSize{0};
    };
    struct OtaRequestEvent {
        std::string version{};
        std::string url{};
        bool force{false}; // Force update even if same version, // TODO: remove or use??
    };
    struct OtaStateChangedEvent {
        std::uint8_t oldState{0}; // Cast from OtaState, need to reverse when reading
        std::uint8_t newState{0};
        std::string message{};
        std::uint64_t timestampMs{0};
    };
    struct OtaProgressEvent {
        std::uint8_t percent{0};
        std::uint32_t bytesDownloaded{0};
        std::uint32_t totalBytes{0};
        bool success{false};
        std::string message{};
    };
    struct OtaVersionInfoEvent {
        std::string currentVersion{};
        std::string availableVersion{};
        bool updateAvailable{false};
    };
    struct Pn532StatusChangedEvent {
        Pn532Status oldStatus{Pn532Status::Uninitialized};
        Pn532Status newStatus{Pn532Status::Uninitialized};
        std::uint64_t timestampMs{0};
    };
    struct Pn532ErrorEvent {
        std::string errorMessage{};
        std::uint32_t errorCode{0};
        std::uint8_t consecutiveErrors{0};
        std::uint64_t timestampMs{0};
    };
    struct Pn532RecoveredEvent {
        std::uint8_t recoveryAttempts{0};
        std::uint64_t downtimeMs{0};
    };
    struct Pn532CardEvent {
        CardId cardId{};
        std::uint64_t timestampMs{0};
    };
    struct PowerStateChangedEvent {
        PowerState oldState{PowerState::Active};
        PowerState newState{PowerState::Active};
        std::uint64_t timestampMs{0};
    };
    struct SleepEnteringEvent {
        PowerState targetState{PowerState::LightSleep};
        std::uint32_t expectedDurationMs{0};
    };
    struct WakeupEvent {
        WakeupReason reason{WakeupReason::Unknown};
        std::uint64_t sleepDurationMs{0};
        std::uint64_t timestampMs{0};
    };
    struct WakeLockEvent {
        const char *lockName{""};
        std::uint32_t lockId{0};
        std::uint8_t totalActiveLocks{0};
    };
    struct HealthStatusChangedEvent {
        std::string_view componentName{};
        HealthState oldState{HealthState::Unknown};
        HealthState newState{HealthState::Unknown};
        std::string message{};
        std::uint64_t timestampMs{0};
    };
    struct HighLoadEvent {
        std::string_view source{};
        std::size_t currentLoad{0};
        std::size_t threshold{0};
        std::uint64_t timestampMs{0};
    };
    struct QueueOverflowEvent {
        std::string_view queueName{};
        std::size_t droppedItems{0};
        std::size_t queueCapacity{0};
    };
    struct FeedbackRequestEvent {
        FeedbackSignal signal{FeedbackSignal::Success};
        std::uint8_t repeatCount{1}; // How many times to repeat the signal
    };
    struct SystemErrorEvent {
        std::string_view component{};
        std::uint32_t errorCode{0};
        std::string message{};
        bool recoverable{true};
    };
    struct SystemWarningEvent {
        std::string_view component{};
        std::string message{};
    };
    struct ModuleStateChangedEvent {
        std::string_view moduleName{};
        bool enabled{false};
        std::uint64_t timestampMs{0};
    };
    using EventPayload = std::variant<
        std::monostate,
        // Card/Attendance
        CardScannedEvent,
        CardReadErrorEvent,
        AttendanceRecord,
        AttendanceBatchReadyEvent,
        // Config
        ConfigUpdatedEvent,
        // MQTT
        MqttMessageEvent,
        MqttQueueOverflowEvent,
        // OTA
        OtaRequestEvent,
        OtaStateChangedEvent,
        OtaProgressEvent,
        OtaVersionInfoEvent,
        // PN532
        Pn532StatusChangedEvent,
        Pn532ErrorEvent,
        Pn532RecoveredEvent,
        Pn532CardEvent,
        // Power
        PowerStateChangedEvent,
        SleepEnteringEvent,
        WakeupEvent,
        WakeLockEvent,
        // Health
        HealthStatusChangedEvent,
        HighLoadEvent,
        QueueOverflowEvent,
        // Feedback
        FeedbackRequestEvent,
        // System
        SystemErrorEvent,
        SystemWarningEvent,
        ModuleStateChangedEvent
    >;
    struct Event {
        EventType type{EventType::Heartbeat};
        EventPayload payload{};
        std::uint64_t timestampMs{0};
        std::uint8_t priority{0}; // 0 = normal, higher = more urgent
    };

    // ------------------------ Event Priority Levels ------------------------
    namespace EventPriority {
        inline constexpr std::uint8_t E_LOW = 0;
        inline constexpr std::uint8_t E_NORMAL = 1;
        inline constexpr std::uint8_t E_HIGH = 2;
        inline constexpr std::uint8_t E_CRITICAL = 3;
    }
}

#endif  // HARDWARE_EVENTS_HPP
