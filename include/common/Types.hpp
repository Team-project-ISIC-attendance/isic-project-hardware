#ifndef ISIC_CORE_TYPES_HPP
#define ISIC_CORE_TYPES_HPP

#include <array>
#include <memory>
#include <string>
#include <variant>

namespace isic
{
/// Maximum size of NFC card UID in bytes (ISO14443A supports up to 10, we use 7)
inline constexpr std::uint8_t CARD_UID_MAX_SIZE{7};

/// Fixed-size array for storing card UIDs
using CardUid = std::array<std::uint8_t, CARD_UID_MAX_SIZE>;

/**
 * @brief Convert card UID to hexadecimal string representation
 *
 * @param uid Card UID array
 * @param length Number of valid bytes in UID (default: full array)
 * @return Uppercase hexadecimal string (e.g., "04A1B2C3")
 *
 * @note This function is inline for zero-cost abstraction
 * @note Pre-allocates exact memory needed to avoid reallocations
 */
inline std::string cardUidToString(const CardUid &uid, const std::uint8_t length = CARD_UID_MAX_SIZE)
{
    constexpr char hexChars[] = "0123456789ABCDEF";

    std::string result;
    result.reserve(length * 2);

    for (std::uint8_t i = 0; i < length; ++i)
    {
        result += hexChars[(uid[i] >> 4) & 0x0F];
        result += hexChars[uid[i] & 0x0F];
    }
    return result;
}

enum class StatusCode : std::uint8_t
{
    Ok = 0, ///< Operation completed successfully
    Error, ///< Generic error occurred
    Timeout, ///< Operation timed out
    NotReady, ///< Resource not ready for operation
    InvalidArg, ///< Invalid argument provided
    NoMemory, ///< Insufficient memory
    NotFound, ///< Requested resource not found
    Busy, ///< Resource is busy

    _Count, // NOLINT - Sentinel value for enum iteration
};

/**
 * @brief Status result wrapper for operations
 *
 * Combines a status code with an optional error message.
 * Designed for lightweight error propagation in embedded contexts.
 *
 * Example usage:
 * @code
 * Status result = someOperation();
 * if (result.failed()) {
 *     LOG_ERROR(TAG, "Operation failed: %s", result.message);
 * }
 * @endcode
 */
struct Status
{
    StatusCode code{StatusCode::Ok};
    const char *message{nullptr};

    // Convenience query methods
    [[nodiscard]] bool ok() const
    {
        return code == StatusCode::Ok;
    }

    [[nodiscard]] bool failed() const
    {
        return code != StatusCode::Ok;
    }

    // Factory methods for common status results
    static Status Ok()
    {
        return {StatusCode::Ok, nullptr};
    }

    static Status Error(const char *msg = nullptr)
    {
        return {StatusCode::Error, msg};
    }

    static Status Timeout(const char *msg = nullptr)
    {
        return {StatusCode::Timeout, msg};
    }

    static Status NotReady(const char *msg = nullptr)
    {
        return {StatusCode::NotReady, msg};
    }
};

enum class HealthState : std::uint8_t
{
    Unknown = 0, ///< Health status not yet determined
    Healthy, ///< Functioning normally
    Degraded, ///< Operating with reduced functionality
    Unhealthy, ///< Significant issues, may fail soon
    Warning, ///< Minor issues detected
    Critical, ///< System failure imminent or occurred

    _Count, // NOLINT - Sentinel value
};

inline const char *toString(const HealthState healthState)
{
    switch (healthState)
    {
        case HealthState::Healthy: return "healthy";
        case HealthState::Degraded: return "degraded";
        case HealthState::Unhealthy: return "unhealthy";
        case HealthState::Warning: return "warning";
        case HealthState::Critical: return "critical";
        default: return "unknown";
    }
}

struct ServiceHealth
{
    HealthState state{HealthState::Unknown}; ///< Current health state
    const char *name{nullptr}; ///< Service identifier
    const char *message{nullptr}; ///< Human-readable status message
    std::uint32_t errorCount{0}; ///< Cumulative error count
    std::uint32_t lastUpdateMs{0}; ///< Timestamp of last update
};

enum class WiFiState : std::uint8_t
{
    Disconnected = 0, ///< Not connected to any network
    Connecting, ///< Attempting to connect
    Connected, ///< Successfully connected to WiFi
    ApMode, ///< Operating as access point
    WaitingRetry, ///< Waiting before retry attempt (non-blocking delay)
    Error, ///< Connection error occurred

    _Count, // NOLINT
};

inline const char *toString(const WiFiState wifiState)
{
    switch (wifiState)
    {
        case WiFiState::Disconnected: return "disconnected";
        case WiFiState::Connecting: return "connecting";
        case WiFiState::Connected: return "connected";
        case WiFiState::ApMode: return "ap_mode";
        case WiFiState::WaitingRetry: return "waiting_retry";
        case WiFiState::Error: return "error";
        default: return "unknown";
    }
}

enum class MqttState : std::uint8_t
{
    Disconnected = 0, ///< Not connected to broker
    Connecting, ///< Attempting to connect
    Connected, ///< Successfully connected
    Error, ///< Connection/publish error

    _Count, // NOLINT
};

inline const char *toString(const MqttState mqttState)
{
    switch (mqttState)
    {
        case MqttState::Disconnected: return "disconnected";
        case MqttState::Connecting: return "connecting";
        case MqttState::Connected: return "connected";
        case MqttState::Error: return "error";
        default: return "unknown";
    }
}

enum class Pn532State : std::uint8_t
{
    Uninitialized = 0, ///< Not yet initialized
    Ready, ///< Ready to read cards
    Reading, ///< Currently reading a card
    Error, ///< Hardware error detected
    Offline, ///< Hardware not responding
    Disabled, ///< Intentionally disabled

    _Count, // NOLINT
};

inline const char *toString(const Pn532State nfsState)
{
    switch (nfsState)
    {
        case Pn532State::Uninitialized: return "uninitialized";
        case Pn532State::Ready: return "ready";
        case Pn532State::Reading: return "reading";
        case Pn532State::Error: return "error";
        case Pn532State::Offline: return "offline";
        case Pn532State::Disabled: return "disabled";
        default: return "unknown";
    }
}

enum class OtaState : std::uint8_t
{
    Idle = 0, ///< No update in progress
    Downloading, ///< Downloading firmware
    Completed, ///< Update completed successfully
    Error, ///< Update failed

    _Count, // NOLINT
};

inline const char *toString(const OtaState otaState)
{
    switch (otaState)
    {
        case OtaState::Idle: return "idle";
        case OtaState::Downloading: return "downloading";
        case OtaState::Completed: return "completed";
        case OtaState::Error: return "error";
        default: return "unknown";
    }
}

enum class PowerState : std::uint8_t
{
    Active = 0, ///< Full power, all systems running
    LightSleep, ///< CPU paused, WiFi maintains connection
    ModemSleep, ///< WiFi RF off, CPU running
    DeepSleep, ///< Everything off except RTC (~20µA)
    Hibernating, ///< Deep sleep with extended duration (chained)

    _Count, // NOLINT
};

inline const char *toString(const PowerState powerState)
{
    switch (powerState)
    {
        case PowerState::Active: return "active";
        case PowerState::LightSleep: return "light_sleep";
        case PowerState::ModemSleep: return "modem_sleep";
        case PowerState::DeepSleep: return "deep_sleep";
        case PowerState::Hibernating: return "hibernating";
        default: return "unknown";
    }
}

enum class WakeupReason : std::uint8_t
{
    PowerOn = 0, ///< Initial power on / hard reset
    Timer, ///< RTC timer wakeup from deep sleep
    External, ///< External reset (PN532 IRQ, button)
    WatchdogReset, ///< Watchdog timer reset
    Unknown, ///< Unknown or unhandled reason

    _Count, // NOLINT
};

inline const char *toString(const WakeupReason wakeupReason)
{
    switch (wakeupReason)
    {
        case WakeupReason::PowerOn: return "power_on";
        case WakeupReason::Timer: return "timer";
        case WakeupReason::External: return "external";
        case WakeupReason::WatchdogReset: return "watchdog";
        default: return "unknown";
    }
}

enum class FeedbackSignal : std::uint8_t
{
    None = 0, ///< No feedback
    Success, ///< Operation successful
    Error, ///< Error occurred
    Processing, ///< Processing in progress
    Connected, ///< Connected to network
    Disconnected, ///< Disconnected from network
    OtaStart, ///< OTA update starting
    OtaComplete, ///< OTA update completed

    _Count, // NOLINT
};

inline const char *toString(const FeedbackSignal signal)
{
    switch (signal)
    {
        case FeedbackSignal::Success: return "success";
        case FeedbackSignal::Error: return "error";
        case FeedbackSignal::Processing: return "processing";
        case FeedbackSignal::Connected: return "connected";
        case FeedbackSignal::Disconnected: return "disconnected";
        case FeedbackSignal::OtaStart: return "ota_start";
        case FeedbackSignal::OtaComplete: return "ota_complete";
        default: return "none";
    }
}

enum class EventType : std::uint8_t
{
    None = 0, ///< Invalid/uninitialized event

    // System events
    SystemReady, ///< System initialization complete
    SystemError, ///< Critical system error

    // Configuration events
    ConfigChanged, ///< Configuration updated (triggers service reconfiguration)
    ConfigError, ///< Configuration load/save error

    // Wifi events (standardized naming)
    WifiConnected, ///< Connected to Wifi network
    WifiDisconnected, ///< Disconnected from Wifi
    WifiError, ///< Wifi error occurred
    WifiApStarted, ///< Access point mode started
    WifiApStopped, ///< Access point mode stopped
    WifiApError, ///< Access point error
    WifiApClientConnected, ///< Client connected to our AP

    // MQTT events
    MqttConnected, ///< Connected to MQTT broker
    MqttDisconnected, ///< Disconnected from MQTT broker
    MqttError, ///< MQTT communication error
    MqttMessage, ///< Incoming MQTT message received
    MqttPublishRequest, ///< Request to publish MQTT message
    MqttSubscribeRequest, ///< Request to subscribe to MQTT topic

    // NFC events
    NfcReady, ///< NFC reader initialized and ready
    CardScanned, ///< NFC card detected and read
    CardRemoved, ///< NFC card removed from field
    NfcError, ///< NFC reader error

    // Attendance events
    AttendanceRecorded, ///< Attendance record created
    AttendanceError, ///< Attendance processing error

    // OTA events
    OtaStarted, ///< Firmware update started
    OtaProgress, ///< Firmware download progress update
    OtaCompleted, ///< Firmware update completed
    OtaError, ///< Firmware update error

    // Feedback events
    FeedbackRequest, ///< Request user feedback (LED/buzzer)

    // Health events
    HealthChanged, ///< System or component health state changed

    // Power management events
    PowerStateChange, ///< Power state transition
    SleepRequested, ///< Sleep mode requested
    WakeupOccurred, ///< Woke up from sleep

    _Count, // NOLINT - Sentinel value for array sizing
};

inline const char *toString(const EventType eventType)
{
    switch (eventType)
    {
        case EventType::None: return "none";
        case EventType::SystemReady: return "system_ready";
        case EventType::SystemError: return "system_error";
        case EventType::ConfigChanged: return "config_changed";
        case EventType::ConfigError: return "config_error";
        case EventType::WifiConnected: return "wifi_connected";
        case EventType::WifiDisconnected: return "wifi_disconnected";
        case EventType::WifiError: return "wifi_error";
        case EventType::WifiApStarted: return "wifi_ap_started";
        case EventType::WifiApStopped: return "wifi_ap_stopped";
        case EventType::WifiApError: return "wifi_ap_error";
        case EventType::WifiApClientConnected: return "wifi_ap_client";
        case EventType::MqttConnected: return "mqtt_connected";
        case EventType::MqttDisconnected: return "mqtt_disconnected";
        case EventType::MqttError: return "mqtt_error";
        case EventType::MqttMessage: return "mqtt_message";
        case EventType::MqttPublishRequest: return "mqtt_publish_req";
        case EventType::NfcReady: return "nfc_ready";
        case EventType::CardScanned: return "card_scanned";
        case EventType::CardRemoved: return "card_removed";
        case EventType::NfcError: return "nfc_error";
        case EventType::AttendanceRecorded: return "attendance_recorded";
        case EventType::AttendanceError: return "attendance_error";
        case EventType::OtaStarted: return "ota_started";
        case EventType::OtaProgress: return "ota_progress";
        case EventType::OtaCompleted: return "ota_completed";
        case EventType::OtaError: return "ota_error";
        case EventType::FeedbackRequest: return "feedback_request";
        case EventType::HealthChanged: return "health_changed";
        case EventType::PowerStateChange: return "power_state_change";
        case EventType::SleepRequested: return "sleep_requested";
        case EventType::WakeupOccurred: return "wakeup_occurred";
        default: return "unknown";
    }
}

/**
 * @brief NFC card event data
 *
 * Carries information about scanned NFC cards.
 */
struct CardEvent
{
    CardUid uid{}; ///< Card unique identifier
    std::uint8_t uidLength{0}; ///< Valid bytes in UID (4 or 7 typically)
    std::uint32_t timestampMs{0}; ///< When card was read
};

/**
 * @brief MQTT event data (unified for both incoming messages and publish requests)
 *
 * @note Direction is determined by EventType:
 *       - EventType::MqttMessage: Incoming message from broker
 *       - EventType::MqttPublishRequest: Outgoing publish request
 */
struct MqttEvent
{
    std::string topic; ///< MQTT topic
    std::string payload; ///< Message payload
    bool retain{false}; ///< Retain flag for published messages
};

/**
 * @brief OTA firmware update progress event data
 */
struct OtaProgressEvent
{
    std::uint8_t percent{0}; ///< Download progress percentage (0-100)
    std::uint32_t bytesReceived{0}; ///< Bytes downloaded so far
    std::uint32_t totalBytes{0}; ///< Total firmware size
};

/**
 * @brief User feedback request event data
 */
struct FeedbackEvent
{
    FeedbackSignal signal{FeedbackSignal::None}; ///< Type of feedback to provide
    std::uint8_t repeatCount{1}; ///< Number of times to repeat signal
};

/**
 * @brief Health status change event data
 */
struct HealthEvent
{
    HealthState state{HealthState::Unknown}; ///< New health state
    const char *component{nullptr}; ///< Component name (optional)
    const char *message{nullptr}; ///< Human-readable status message
};

/**
 * @brief Power state change event data
 */
struct PowerEvent
{
    PowerState targetState{PowerState::Active}; ///< Target power state
    PowerState previousState{PowerState::Active}; ///< Previous power state
    WakeupReason wakeupReason{WakeupReason::Unknown}; ///< Why we woke up (if applicable)
    std::uint32_t durationMs{0}; ///< Requested sleep duration (0 = indefinite)
};

/**
 * @brief Enterprise-grade event container with optimal memory layout
 *
 * @note Size: sizeof(Event) ≈ 48-64 bytes (check with static_assert)
 * @note Alignment: Natural alignment for optimal cache performance
 */
struct alignas(8) Event // 8-byte alignment for variant optimization
{
    std::uint32_t timestampMs{0}; ///< Event timestamp in milliseconds (4 bytes)
    EventType eventType{EventType::None}; ///< Event type (1 byte)
    std::uint8_t version{1}; ///< Schema version (1 byte)
    std::uint8_t priority{0}; ///< Priority hint 0-255 (higher = more urgent) (1 byte)
    std::uint8_t flags{0}; ///< Reserved flags for future use (1 byte)

    /**
     * @brief Type-safe discriminated union for event payloads
     *
     * Uses std::variant for zero-cost abstraction with compile-time type safety.
     * Largest member determines total variant size (~32 bytes for MqttEvent).
     */
    std::variant<
            std::monostate, ///< Empty state (0 bytes) - for simple lifecycle events
            CardEvent, ///< NFC card data (~16 bytes)
            MqttEvent, ///< MQTT message (~32 bytes) - LARGEST
            OtaProgressEvent, ///< OTA progress (~12 bytes)
            FeedbackEvent, ///< Feedback request (~4 bytes)
            HealthEvent, ///< Health status (~4 bytes)
            PowerEvent ///< Power state (~16 bytes)
            >
            data{std::monostate{}}; ///< Event payload data


    Event() = default;
    explicit Event(const EventType type)
        : eventType(type)
    {
    }

    template<typename T>
    Event(const EventType type, T &&eventData)
        : eventType(type)
        , data(std::forward<T>(eventData))
    {
    }

    Event(const Event &other) = default;
    Event(Event &&other) noexcept = default;
    Event &operator=(const Event &other) = default;
    Event &operator=(Event &&other) noexcept = default;
    ~Event() = default;

    /**
     * @brief Helper to get event data by type
     *
     * @tparam T Event data type to retrieve
     * @return Pointer to data if active type matches, nullptr otherwise
     */
    template<typename T>
    [[nodiscard]] T *get()
    {
        return std::get_if<T>(&data);
    }

    /**
     * @brief Const version of get() to retrieve event data by type
     *
     * @tparam T Event data type to retrieve
     * @return Pointer to data if active type matches, nullptr otherwise
     */
    template<typename T>
    [[nodiscard]] const T *get() const
    {
        return std::get_if<T>(&data);
    }

    /**
     * @brief Check if event contains specific data type
     *
     * @tparam T Event data type to check
     * @return true if variant holds type T
     */
    template<typename T>
    [[nodiscard]] bool holds() const
    {
        return std::holds_alternative<T>(data);
    }
};

/**
 * @brief System-wide health status
 */
struct SystemHealth
{
    HealthState overallState{HealthState::Unknown};
    std::uint32_t freeHeap{0};
    std::uint32_t heapFragmentation{0};
    std::int32_t wifiRssi{0};
    std::uint32_t uptimeMs{0};
    std::uint32_t lastUpdateMs{0};
};

/**
 * @brief Attendance record for batch transmission
 */
struct AttendanceRecord
{
    std::uint32_t timestampMs{0}; ///< When card was scanned
    std::uint32_t sequence{0}; ///< Sequence number for ordering
    CardUid cardUid{}; ///< Scanned card UID
    std::uint8_t uidLength{0}; ///< Valid UID length
};

/**
 * @brief Base metrics structure for all services
 *
 * Provides common telemetry fields inherited by service-specific metrics.
 */
struct ServiceMetrics
{
    std::uint32_t operationCount{0}; ///< Total operations performed
    std::uint32_t errorCount{0}; ///< Total errors encountered
    std::uint32_t lastOperationMs{0}; ///< Timestamp of last operation
    std::uint32_t uptimeMs{0}; ///< Service uptime
};

/**
 * @brief MQTT service metrics
 *
 * Tracks MQTT connection health and message throughput.
 */
struct MqttMetrics : ServiceMetrics
{
    std::uint32_t messagesPublished{0}; ///< Successful publishes
    std::uint32_t messagesFailed{0}; ///< Failed publish attempts
    std::uint32_t messagesReceived{0}; ///< Messages received from broker
    std::uint32_t reconnectCount{0}; ///< Connection recovery count
    bool connected{false}; ///< Current connection status
};

/**
 * @brief WiFi service metrics
 *
 * Monitors WiFi connectivity and signal quality.
 */
struct WiFiMetrics : ServiceMetrics
{
    std::int8_t rssi{0}; ///< Signal strength (dBm)
    std::uint32_t disconnectCount{0}; ///< Disconnect event count
    bool connected{false}; ///< Current connection status
};

/**
 * @brief Attendance service metrics
 *
 * Tracks card processing and batch transmission.
 */
struct AttendanceMetrics : ServiceMetrics
{
    std::uint32_t cardsProcessed{0}; ///< Total cards processed
    std::uint32_t cardsDebounced{0}; ///< Duplicate cards filtered
    std::uint32_t batchesSent{0}; ///< Batches transmitted to MQTT
    std::uint16_t currentBatchSize{0}; ///< Records in current batch
    std::uint16_t pendingRecords{0}; ///< Records awaiting transmission
};

/**
 * @brief NFC service metrics
 *
 * Monitors NFC reader health and read success rate.
 */
struct Pn532Metrics : ServiceMetrics
{
    std::uint32_t cardsRead{0}; ///< Successful card reads
    std::uint32_t readErrors{0}; ///< Failed read attempts
    std::uint32_t recoveryAttempts{0}; ///< Hardware recovery attempts
    std::uint32_t successfulReads{0}; ///< Valid UID reads
    std::uint32_t lastReadMs{0}; ///< Timestamp of last read
    Pn532State state{Pn532State::Uninitialized}; ///< Current reader state
};

/**
 * @brief Power service metrics
 *
 * Tracks sleep cycles, power states, and energy management.
 */
struct PowerServiceMetrics : ServiceMetrics
{
    // Sleep cycle tracking
    std::uint32_t lightSleepCycles{0}; ///< Number of light sleep entries
    std::uint32_t modemSleepCycles{0}; ///< Number of modem sleep entries
    std::uint32_t deepSleepCycles{0}; ///< Number of deep sleep entries

    // Duration tracking (milliseconds)
    std::uint32_t totalLightSleepMs{0}; ///< Total time in light sleep
    std::uint32_t totalModemSleepMs{0}; ///< Total time in modem sleep
    std::uint32_t totalDeepSleepMs{0}; ///< Total time in deep sleep

    // Activity tracking
    std::uint32_t idleTimeoutsTriggered{0}; ///< Times idle timeout triggered sleep
    std::uint32_t wakeupCount{0}; ///< Total wakeups (survives deep sleep)
    std::uint32_t lastActivityMs{0}; ///< Last activity timestamp

    // Smart sleep decisions
    std::uint32_t smartSleepDecisions{0}; ///< Times smart sleep depth was chosen
    std::uint32_t networkAwareSleeps{0}; ///< Times modem sleep was used for MQTT down

    // Current state
    PowerState currentState{PowerState::Active};
    WakeupReason lastWakeupReason{WakeupReason::Unknown};
};
} // namespace isic

#endif // ISIC_CORE_TYPES_HPP
