#ifndef ISIC_CORE_TYPES_HPP
#define ISIC_CORE_TYPES_HPP

#include <array>
#include <cstdint>
#include <string>
#include <variant>

#include "Config.hpp"

namespace isic
{
// ============================================================================
// Constants
// ============================================================================

inline constexpr std::uint8_t kCardUidMaxSize = 7; // ISO14443A: max 10, we use 7

// ============================================================================
// Type Aliases
// ============================================================================

using CardUid = std::array<std::uint8_t, kCardUidMaxSize>;

// ============================================================================
// Enumerations
// ============================================================================

enum class ServiceState : std::uint8_t
{
    Uninitialized, // Created, begin() not called
    Initializing, // begin() in progress
    Ready, // Initialized, waiting for dependencies
    Running, // Fully operational
    Stopping, // end() in progress
    Stopped, // Cleanly shut down
    Error, // Cannot operate
};

enum class HealthState : std::uint8_t
{
    Unknown,
    Healthy,
    Degraded,
    Unhealthy,
    Warning,
    Critical,
};

enum class WiFiState : std::uint8_t
{
    Disconnected,
    Connecting,
    Connected,
    ApMode,
    WaitingRetry,
    Error,
};

enum class MqttState : std::uint8_t
{
    Disconnected,
    Connecting,
    Connected,
    Error,
};

enum class Pn532State : std::uint8_t
{
    Uninitialized,
    Ready,
    Reading,
    Error,
    Offline,
    Disabled,
};

enum class OtaState : std::uint8_t
{
    Idle,
    Downloading,
    Completed,
    Error,
};

enum class PowerState : std::uint8_t
{
    Active, // Full power
    LightSleep, // CPU paused, WiFi connected
    ModemSleep, // WiFi RF off, CPU running
    DeepSleep, // RTC only (~20µA)
    Hibernating, // Extended deep sleep
};

enum class WakeupReason : std::uint8_t
{
    PowerOn,
    Timer,
    External,
    WatchdogReset,
    Unknown,
};

enum class FeedbackSignal : std::uint8_t
{
    None,
    Success,
    Error,
    Processing,
    Connected,
    Disconnected,
    OtaStart,
    OtaComplete,
};

enum class EventType : std::uint8_t
{
    None,

    // System
    SystemReady,
    SystemError,

    // Config
    ConfigChanged,
    ConfigError,

    // WiFi
    WifiConnected,
    WifiDisconnected,
    WifiError,
    WifiApStarted,
    WifiApStopped,
    WifiApError,
    WifiApClientConnected,

    // MQTT
    MqttConnected,
    MqttDisconnected,
    MqttError,
    MqttMessage,
    MqttPublishRequest,
    MqttSubscribeRequest,

    // NFC
    NfcReady,
    CardScanned,
    CardRemoved,
    NfcError,

    // Attendance
    AttendanceRecorded,
    AttendanceError,

    // OTA
    OtaStarted,
    OtaProgress,
    OtaCompleted,
    OtaError,

    // Feedback
    FeedbackRequest,

    // Health
    HealthChanged,

    // Power
    PowerStateChange,
    SleepRequested,
    WakeupOccurred,

    _Count, // NOLINT (must be last)
};

enum class StatusCode : std::uint8_t
{
    Ok,
    Error,
    Timeout,
    NotReady,
    InvalidArg,
    NoMemory,
    NotFound,
    Busy,
};

// ============================================================================
// String Conversion - Lookup Table Pattern (Cache-Friendly)
// ============================================================================

namespace detail
{
inline constexpr const char *kServiceStateNames[]{"uninitialized", "initializing", "ready", "running", "stopping", "stopped", "error"};

inline constexpr const char *kHealthStateNames[]{"unknown", "healthy", "degraded", "unhealthy", "warning", "critical"};

inline constexpr const char *kWiFiStateNames[]{"disconnected", "connecting", "connected", "ap_mode", "waiting_retry", "error"};

inline constexpr const char *kMqttStateNames[]{"disconnected", "connecting", "connected", "error"};

inline constexpr const char *kPn532StateNames[]{"uninitialized", "ready", "reading", "error", "offline", "disabled"};

inline constexpr const char *kOtaStateNames[]{"idle", "downloading", "completed", "error"};

inline constexpr const char *kPowerStateNames[]{"active", "light_sleep", "modem_sleep", "deep_sleep", "hibernating"};

inline constexpr const char *kWakeupReasonNames[]{"power_on", "timer", "external", "watchdog", "unknown"};

inline constexpr const char *kFeedbackSignalNames[]{"none", "success", "error", "processing", "connected", "disconnected", "ota_start", "ota_complete"};

inline constexpr const char *kEventTypeNames[]{"none", "system_ready", "system_error", "config_changed", "config_error", "wifi_connected", "wifi_disconnected", "wifi_error", "wifi_ap_started", "wifi_ap_stopped", "wifi_ap_error", "wifi_ap_client", "mqtt_connected", "mqtt_disconnected", "mqtt_error", "mqtt_message", "mqtt_publish_req", "mqtt_subscribe_req", "nfc_ready", "card_scanned", "card_removed", "nfc_error", "attendance_recorded", "attendance_error", "ota_started", "ota_progress", "ota_completed", "ota_error", "feedback_request", "health_changed", "power_state_change", "sleep_requested", "wakeup_occurred"};

inline constexpr const char *kStatusCodeNames[]{"ok", "error", "timeout", "not_ready", "invalid_arg", "no_memory", "not_found", "busy"};

template<typename EnumType, std::size_t N>
constexpr const char *enumToString(EnumType value, const char *const (&names)[N], const char *fallback = "unknown")
{
    const auto idx = static_cast<std::size_t>(value);
    return idx < N ? names[idx] : fallback;
}
} // namespace detail

constexpr const char *toString(const ServiceState state) { return detail::enumToString(state, detail::kServiceStateNames); }
constexpr const char *toString(const HealthState state) { return detail::enumToString(state, detail::kHealthStateNames); }
constexpr const char *toString(const WiFiState state) { return detail::enumToString(state, detail::kWiFiStateNames); }
constexpr const char *toString(const MqttState state) { return detail::enumToString(state, detail::kMqttStateNames); }
constexpr const char *toString(const Pn532State state) { return detail::enumToString(state, detail::kPn532StateNames); }
constexpr const char *toString(const OtaState state) { return detail::enumToString(state, detail::kOtaStateNames); }
constexpr const char *toString(const PowerState state) { return detail::enumToString(state, detail::kPowerStateNames); }
constexpr const char *toString(const WakeupReason state) { return detail::enumToString(state, detail::kWakeupReasonNames); }
constexpr const char *toString(const FeedbackSignal signal) { return detail::enumToString(signal, detail::kFeedbackSignalNames); }
constexpr const char *toString(const EventType type) { return detail::enumToString(type, detail::kEventTypeNames); }
constexpr const char *toString(const StatusCode code) { return detail::enumToString(code, detail::kStatusCodeNames); }

// ============================================================================
// Core Structures
// ============================================================================

struct Status
{
    const char *message{nullptr}; // 4/8 bytes (pointer)
    StatusCode code{StatusCode::Ok}; // 1 byte
    // 3/7 bytes padding

    [[nodiscard]] constexpr bool ok() const { return code == StatusCode::Ok; }
    [[nodiscard]] constexpr bool failed() const { return code != StatusCode::Ok; }

    static constexpr Status Ok() { return {}; }
    static constexpr Status Error(const char *msg = nullptr) { return {msg, StatusCode::Error}; }
    static constexpr Status Timeout(const char *msg = nullptr) { return {msg, StatusCode::Timeout}; }
    static constexpr Status NotReady(const char *msg = nullptr) { return {msg, StatusCode::NotReady}; }
    static constexpr Status InvalidArg(const char *msg = nullptr) { return {msg, StatusCode::InvalidArg}; }
    static constexpr Status NotFound(const char *msg = nullptr) { return {msg, StatusCode::NotFound}; }
    static constexpr Status Busy(const char *msg = nullptr) { return {msg, StatusCode::Busy}; }
};

struct SystemHealth
{
    std::uint32_t uptimeMs{0};
    std::uint32_t cpuFrequencyMs{0};
    std::uint32_t freeHeap{0};
    std::uint32_t heapFragmentation{0};
    std::int8_t wifiRssi{0};
    HealthState heapState{HealthState::Unknown};
    HealthState fragmentationState{HealthState::Unknown};
    HealthState wifiState{HealthState::Unknown};
    HealthState overallState{HealthState::Unknown};
};

struct AttendanceRecord
{
    std::uint32_t timestampMs{0}; ///< When card was scanned
    std::uint32_t sequence{0}; ///< Sequence number for ordering
    CardUid cardUid{}; ///< Scanned card UID
};

/**
 * @brief Feedback pattern definition
 *
 * Timeline per cycle:
 * |<-- ledOnMs -->|<-- ledOffMs -->|
 * |<-- beepMs -->|
 */
struct FeedbackPattern
{
    std::uint16_t ledOnMs{0}; ///< LED on duration per cycle
    std::uint16_t ledOffMs{0}; ///< LED off duration per cycle
    std::uint16_t beepMs{0}; ///< Buzzer duration per cycle (0 = silent)
    std::uint16_t beepFrequencyHz{2000}; ///< Buzzer frequency
    std::uint8_t repeatCount{1}; ///< Number of cycles (0xFF = infinite)
    bool useErrorLed{false}; ///< Use error LED instead of status LED (future)
};
// ============================================================================
// Event Payloads
// ============================================================================

struct CardEvent
{
    std::uint32_t timestampMs{0}; // 4 bytes
    CardUid uid{}; // 7 bytes
    // 1 byte padding
};
static_assert(sizeof(CardEvent) == 12, "CardEvent size changed");

struct MqttEvent
{
    std::string topic;
    std::string payload;
    bool retain{false};
};
// No static_assert, size may vary due to std::string

struct FeedbackEvent
{
    FeedbackSignal signal{FeedbackSignal::None};
    std::uint8_t repeatCount{1};
};
static_assert(sizeof(FeedbackEvent) == 2, "FeedbackEvent size changed");

struct PowerEvent
{
    std::uint32_t durationMs{0};
    PowerState targetState{PowerState::Active};
    PowerState previousState{PowerState::Active};
    WakeupReason wakeupReason{WakeupReason::Unknown};
    // 1 byte padding
};
static_assert(sizeof(PowerEvent) == 8, "PowerEvent size changed");

// ============================================================================
// Event Container
// ============================================================================

struct Event
{
    using Payload = std::variant<std::monostate, CardEvent, MqttEvent, FeedbackEvent, PowerEvent>;

    Payload data{std::monostate{}};
    std::uint32_t timestampMs{0};
    EventType type{EventType::None};
    std::uint8_t priority{0};

    Event() = default;
    explicit Event(EventType t)
        : type(t)
    {
    }

    template<typename T>
    Event(EventType t, T &&payload)
        : data(std::forward<T>(payload))
        , type(t)
    {
    }

    template<typename T>
    [[nodiscard]] T *get()
    {
        return std::get_if<T>(&data);
    }
    template<typename T>
    [[nodiscard]] const T *get() const
    {
        return std::get_if<T>(&data);
    }
    template<typename T>
    [[nodiscard]] bool holds() const
    {
        return std::holds_alternative<T>(data);
    }
};

// ============================================================================
// Metrics Structures
// ============================================================================
struct MqttMetrics
{
    std::uint32_t messagesPublished{0};
    std::uint32_t messagesFailed{0};
    std::uint32_t messagesReceived{0};
    std::uint32_t reconnectCount{0};
};

struct WiFiMetrics
{
    std::uint32_t disconnectCount{0};
    std::int8_t rssi{0};
};

struct AttendanceMetrics
{
    std::uint32_t cardsProcessed{0};
    std::uint32_t cardsDebounced{0};
    std::uint32_t batchesSent{0};
    std::uint32_t errorCount{0};
};

struct Pn532Metrics
{
    std::uint32_t cardsRead{0};
    std::uint32_t readErrors{0};
    std::uint32_t successfulReads{0};
    std::uint32_t recoveryAttempts{0};
};

struct PowerMetrics
{
    std::uint32_t lightSleepCycles{0};
    std::uint32_t modemSleepCycles{0};
    std::uint32_t deepSleepCycles{0};
    std::uint32_t wakeupCount{0};
    std::uint32_t smartSleepUsed{0};
    std::uint32_t networkAwareSleeps{0};
};

// ============================================================================
// Utility Functions
// ============================================================================

inline std::string cardUidToString(const CardUid &uid, const std::uint8_t length = kCardUidMaxSize)
{
    static constexpr char kHexChars[] = "0123456789ABCDEF";

    std::string result;
    result.reserve(length * 2);

    // Reverse byte order for standard NFC UID display format
    for (std::uint8_t i = length; i > 0; --i)
    {
        result += kHexChars[(uid[i - 1] >> 4) & 0x0F];
        result += kHexChars[uid[i - 1] & 0x0F];
    }
    return result;
}
} // namespace isic

#endif // ISIC_CORE_TYPES_HPP
