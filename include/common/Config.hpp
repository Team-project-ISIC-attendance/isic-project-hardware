#ifndef ISIC_CONFIG_HPP
#define ISIC_CONFIG_HPP

#include <string>

namespace isic
{
struct WiFiConfig
{
    struct Constants
    {
        static constexpr auto kSystemRebootDelayMs{5'000};
    };
    static constexpr auto kStationConnectRetryDelayMs{500};
    static constexpr auto kStationConnectionTimeoutMs{10'000};
    static constexpr auto kStationMaxFastConnectionAttempts{10};
    static constexpr auto kStationFastReconnectIntervalMs{5'000};
    static constexpr auto kStationSlowReconnectIntervalMs{600'000};
    static constexpr auto kStationHasEverConnected{false};
    static constexpr auto kStationPowerSaveEnabled{false};
    static constexpr auto kAccessPointSsidPrefix{"ISIC-Setup-"};
    static constexpr auto kAccessPointDefaultPassword{"isic1234"};
    static constexpr auto kAccessPointModeTimeoutMs{300'000};

    std::string stationSsid{};
    std::string stationPassword{};
    std::uint32_t stationConnectRetryDelayMs{kStationConnectRetryDelayMs};
    std::uint32_t stationConnectionTimeoutMs{kStationConnectionTimeoutMs};
    std::uint32_t stationFastReconnectIntervalMs{kStationFastReconnectIntervalMs};
    std::uint32_t stationSlowReconnectIntervalMs{kStationSlowReconnectIntervalMs};
    std::uint8_t stationMaxFastConnectionAttempts{kStationMaxFastConnectionAttempts};
    bool stationHasEverConnected{kStationHasEverConnected};
    bool stationPowerSaveEnabled{kStationPowerSaveEnabled};
    std::string accessPointSsidPrefix{kAccessPointSsidPrefix};
    std::string accessPointPassword{kAccessPointDefaultPassword};
    std::uint32_t accessPointModeTimeoutMs{kAccessPointModeTimeoutMs};

    [[nodiscard]] bool isConfigured() const
    {
        return !stationSsid.empty() && !stationPassword.empty();
    }

    void restoreDefaults()
    {
        stationSsid.clear();
        stationPassword.clear();
        stationConnectRetryDelayMs = kStationConnectRetryDelayMs;
        stationConnectionTimeoutMs = kStationConnectionTimeoutMs;
        stationFastReconnectIntervalMs = kStationFastReconnectIntervalMs;
        stationSlowReconnectIntervalMs = kStationSlowReconnectIntervalMs;
        stationMaxFastConnectionAttempts = kStationMaxFastConnectionAttempts;
        stationPowerSaveEnabled = kStationPowerSaveEnabled;
        stationHasEverConnected = kStationHasEverConnected;
        accessPointSsidPrefix = kAccessPointSsidPrefix;
        accessPointPassword = kAccessPointDefaultPassword;
        accessPointModeTimeoutMs = kAccessPointModeTimeoutMs;
    }
};

struct MqttConfig
{
    struct Constants
    {
        static constexpr auto kMaxPayloadSizeBytes{4024};
    };
    static constexpr auto kDefaultBrokerPort{1883};
    static constexpr auto kDefaultBaseTopic{"device"};
    static constexpr auto kDefaultKeepAliveIntervalSec{60};
    static constexpr auto kDefaultReconnectMinIntervalMs{1'000};
    static constexpr auto kDefaultReconnectMaxIntervalMs{30'000};

    std::string brokerAddress{};
    std::string username{};
    std::string password{};
    std::string baseTopic{kDefaultBaseTopic};
    std::uint32_t reconnectMinIntervalMs{kDefaultReconnectMinIntervalMs};
    std::uint32_t reconnectMaxIntervalMs{kDefaultReconnectMaxIntervalMs};
    std::uint16_t port{kDefaultBrokerPort};
    std::uint16_t keepAliveIntervalSec{kDefaultKeepAliveIntervalSec};

    [[nodiscard]] bool isConfigured() const
    {
        return !brokerAddress.empty();
    }

    void restoreDefaults()
    {
        brokerAddress.clear();
        username.clear();
        password.clear();
        baseTopic = kDefaultBaseTopic;
        reconnectMinIntervalMs = kDefaultReconnectMinIntervalMs;
        reconnectMaxIntervalMs = kDefaultReconnectMaxIntervalMs;
        port = kDefaultBrokerPort;
        keepAliveIntervalSec = kDefaultKeepAliveIntervalSec;
    }
};

struct DeviceConfig
{
    struct Constants
    {
        static constexpr auto kFirmwareVersion{FIRMWARE_VERSION};
    };
    static constexpr auto kDefaultDeviceId{"ISIC-ESP8266-001"};
    static constexpr auto kDefaultLocationId{"unknown"};

    std::string deviceId{kDefaultDeviceId};
    std::string locationId{kDefaultLocationId};

    [[nodiscard]] bool isConfigured() const
    {
        return !deviceId.empty();
    }

    void restoreDefaults()
    {
        deviceId = kDefaultDeviceId;
        locationId = kDefaultLocationId;
    }
};

struct Pn532Config
{
    static constexpr auto kDefaultSpiSckPin{14};
    static constexpr auto kDefaultSpiMisoPin{12};
    static constexpr auto kDefaultSpiMosiPin{13};
    static constexpr auto kDefaultSpiCsPin{5};
    static constexpr auto kDefaultIrqPin{4};
    static constexpr auto kDefaultResetPin{0xFF};
    static constexpr auto kDefaultPollIntervalMs{100};
    static constexpr auto kDefaultReadTimeoutMs{200};
    static constexpr auto kDefaultRecoveryDelayMs{2'000};
    static constexpr auto kDefaultMaxConsecutiveErrors{5};

    std::uint32_t pollIntervalMs{kDefaultPollIntervalMs};
    std::uint32_t readTimeoutMs{kDefaultReadTimeoutMs};
    std::uint32_t recoveryDelayMs{kDefaultRecoveryDelayMs};
    std::uint8_t spiSckPin{kDefaultSpiSckPin};
    std::uint8_t spiMisoPin{kDefaultSpiMisoPin};
    std::uint8_t spiMosiPin{kDefaultSpiMosiPin};
    std::uint8_t spiCsPin{kDefaultSpiCsPin};
    std::uint8_t irqPin{kDefaultIrqPin};
    std::uint8_t resetPin{kDefaultResetPin};
    std::uint8_t maxConsecutiveErrors{kDefaultMaxConsecutiveErrors};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // PN532 always considered configured
    }

    constexpr void restoreDefaults()
    {
        pollIntervalMs = kDefaultPollIntervalMs;
        readTimeoutMs = kDefaultReadTimeoutMs;
        recoveryDelayMs = kDefaultRecoveryDelayMs;
        spiSckPin = kDefaultSpiSckPin;
        spiMisoPin = kDefaultSpiMisoPin;
        spiMosiPin = kDefaultSpiMosiPin;
        spiCsPin = kDefaultSpiCsPin;
        irqPin = kDefaultIrqPin;
        resetPin = kDefaultResetPin;
        maxConsecutiveErrors = kDefaultMaxConsecutiveErrors;
    }
};

struct AttendanceConfig
{
    enum class OfflineQueuePolicy : std::uint8_t
    {
        DropOldest = 0, ///< Overwrite oldest records when buffer is full (circular buffer)
        DropNewest, ///< Reject new records when buffer is full
        DropAll, ///< Clear entire buffer when full (for critical-only mode)
    };
    
    struct Constants
    {
        static constexpr auto kDebounceCacheSize{8};
    };

    static constexpr auto kDefaultDebounceIntervalMs{2'000};
    static constexpr auto kDefaultBatchMaxSize{10};
    static constexpr auto kDefaultOfflineBufferSize{20};
    static constexpr auto kDefaultBatchFlushIntervalMs{30'000};
    static constexpr auto kDefaultBatchingEnabled{true};
    static constexpr auto kDefaultOfflineBufferFlushIntervalMs{5'000};
    static constexpr auto kDefaultOfflineQueuePolicy{OfflineQueuePolicy::DropOldest};

    std::uint32_t debounceIntervalMs{kDefaultDebounceIntervalMs};
    std::uint32_t batchFlushIntervalMs{kDefaultBatchFlushIntervalMs};
    std::uint32_t offlineBufferFlushIntervalMs{kDefaultOfflineBufferFlushIntervalMs};
    std::uint16_t offlineBufferSize{kDefaultOfflineBufferSize};
    std::uint8_t batchMaxSize{kDefaultBatchMaxSize};
    OfflineQueuePolicy offlineQueuePolicy{kDefaultOfflineQueuePolicy};
    bool batchingEnabled{kDefaultBatchingEnabled};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // Always considered configured
    }

    constexpr void restoreDefaults()
    {
        debounceIntervalMs = kDefaultDebounceIntervalMs;
        batchMaxSize = kDefaultBatchMaxSize;
        batchFlushIntervalMs = kDefaultBatchFlushIntervalMs;
        offlineBufferSize = kDefaultOfflineBufferSize;
        offlineBufferFlushIntervalMs = kDefaultOfflineBufferFlushIntervalMs;
        batchingEnabled = kDefaultBatchingEnabled;
        offlineQueuePolicy = kDefaultOfflineQueuePolicy;
    }
};

struct FeedbackConfig
{
    struct Constants
    {
        static constexpr auto kPatternQueueSize{8};
    };

    static constexpr auto kDefaultEnabled{true};
    static constexpr auto kDefaultLedEnabled{true};
    static constexpr auto kDefaultLedPin{2};
    static constexpr auto kDefaultBuzzerEnabled{true};
    static constexpr auto kDefaultBuzzerPin{14};
    static constexpr auto kDefaultLedActiveHigh{false};
    static constexpr auto kDefaultBeepFrequencyHz{2'000};
    static constexpr auto kDefaultSuccessBlinkDurationMs{100};
    static constexpr auto kDefaultErrorBlinkDurationMs{200};

    std::uint16_t beepFrequencyHz{kDefaultBeepFrequencyHz};
    std::uint16_t successBlinkDurationMs{kDefaultSuccessBlinkDurationMs};
    std::uint16_t errorBlinkDurationMs{kDefaultErrorBlinkDurationMs};
    std::uint8_t ledPin{kDefaultLedPin};
    std::uint8_t buzzerPin{kDefaultBuzzerPin};
    bool enabled{kDefaultEnabled};
    bool ledEnabled{kDefaultLedEnabled};
    bool buzzerEnabled{kDefaultBuzzerEnabled};
    bool ledActiveHigh{kDefaultLedActiveHigh};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // Always considered configured
    }

    constexpr void restoreDefaults()
    {
        enabled = kDefaultEnabled;
        ledEnabled = kDefaultLedEnabled;
        ledPin = kDefaultLedPin;
        buzzerEnabled = kDefaultBuzzerEnabled;
        buzzerPin = kDefaultBuzzerPin;
        ledActiveHigh = kDefaultLedActiveHigh;
        beepFrequencyHz = kDefaultBeepFrequencyHz;
        successBlinkDurationMs = kDefaultSuccessBlinkDurationMs;
        errorBlinkDurationMs = kDefaultErrorBlinkDurationMs;
    }
};

struct HealthConfig
{
    struct Constants
    {
        static constexpr auto kMaxComponentsCount{8};
        static constexpr auto kHeapCriticalThresholdBytes{4096};
        static constexpr auto kHeapWarningThresholdBytes{8192};
        static constexpr auto kRssiWarningThresholdDbm{-80};
        static constexpr auto kFragmentationWarningThresholdPercent{50};
    };

    static constexpr auto kDefaultEnabled{true};
    static constexpr auto kDefaultHealthCheckIntervalMs{300'000}; // 5 minutes
    static constexpr auto kDefaultStatusUpdateIntervalMs{60'000}; // 1 minute
    static constexpr auto kDefaultPublishToMqtt{true};
    static constexpr auto kDefaultPublishToLog{true};

    std::uint32_t healthCheckIntervalMs{kDefaultHealthCheckIntervalMs};
    std::uint32_t statusUpdateIntervalMs{kDefaultStatusUpdateIntervalMs};
    bool enabled{kDefaultEnabled};
    bool publishToMqtt{kDefaultPublishToMqtt};
    bool publishToLog{kDefaultPublishToLog};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // Always considered configured
    }

    constexpr void restoreDefaults()
    {
        healthCheckIntervalMs = kDefaultHealthCheckIntervalMs;
        statusUpdateIntervalMs = kDefaultStatusUpdateIntervalMs;
        enabled = kDefaultEnabled;
        publishToMqtt = kDefaultPublishToMqtt;
        publishToLog = kDefaultPublishToLog;
    }
};

struct OtaConfig
{
    static constexpr auto kDefaultEnabled{true};

    std::string updateServerUrl{};
    std::string username{};
    std::string password{};
    bool enabled{kDefaultEnabled};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // Always considered configured
    }

    void restoreDefaults()
    {
        enabled = kDefaultEnabled;
        updateServerUrl.clear();
        username.clear();
        password.clear();
    }
};

struct PowerConfig
{
    struct Constants
    {
        static constexpr auto kSleepDelayMs{100};
    };

    static constexpr auto kDefaultDeepSleepDurationMs{300'000}; // 5 minutes
    static constexpr auto kDefaultMaxDeepSleepMs{3'500'000}; // ~58 min (ESP8266 limit)
    static constexpr auto kDefaultLightSleepDurationMs{10'000}; // 10 seconds
    static constexpr auto kDefaultIdleTimeoutMs{60'000}; // 1 minute
    static constexpr auto kDefaultEnableTimerWakeup{true};
    static constexpr auto kDefaultEnableNfcWakeup{false};
    static constexpr auto kDefaultNfcWakeupPin{4}; // GPIO4 for PN532 IRQ
    static constexpr auto kDefaultAutoSleepEnabled{false};
    static constexpr auto kDefaultDisableWiFiDuringSleep{true};
    static constexpr auto kDefaultPn532SleepBetweenScans{true};
    static constexpr auto kDefaultSmartSleepEnabled{true};
    static constexpr auto kDefaultModemSleepOnMqttDisconnect{true};
    static constexpr auto kDefaultModemSleepDurationMs{30'000}; // 30 seconds
    static constexpr auto kDefaultSmartSleepShortThresholdMs{30'000}; // <30s = light sleep
    static constexpr auto kDefaultSmartSleepMediumThresholdMs{300'000}; // <5m = modem, >5m = deep
    static constexpr auto kDefaultActivityTypeMask{0b00111}; // Card, MQTT msg, WiFi. Activity type bitmask - which events reset idle timer Bit 0: CardScanned, Bit 1: MqttMessage, Bit 2: WifiConnected, Bit 3: MqttConnected, Bit 4: NfcReady

    std::uint32_t sleepIntervalMs{kDefaultDeepSleepDurationMs};
    std::uint32_t maxDeepSleepMs{kDefaultMaxDeepSleepMs};
    std::uint32_t lightSleepDurationMs{kDefaultLightSleepDurationMs};
    std::uint32_t idleTimeoutMs{kDefaultIdleTimeoutMs};
    std::uint32_t modemSleepDurationMs{kDefaultModemSleepDurationMs};
    std::uint32_t smartSleepShortThresholdMs{kDefaultSmartSleepShortThresholdMs};
    std::uint32_t smartSleepMediumThresholdMs{kDefaultSmartSleepMediumThresholdMs};
    std::uint8_t nfcWakeupPin{kDefaultNfcWakeupPin};
    std::uint8_t activityTypeMask{kDefaultActivityTypeMask};
    bool enableTimerWakeup{kDefaultEnableTimerWakeup};
    bool enableNfcWakeup{kDefaultEnableNfcWakeup};
    bool autoSleepEnabled{kDefaultAutoSleepEnabled};
    bool disableWiFiDuringSleep{kDefaultDisableWiFiDuringSleep};
    bool pn532SleepBetweenScans{kDefaultPn532SleepBetweenScans};
    bool smartSleepEnabled{kDefaultSmartSleepEnabled};
    bool modemSleepOnMqttDisconnect{kDefaultModemSleepOnMqttDisconnect};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // Always considered configured
    }

    constexpr void restoreDefaults()
    {
        sleepIntervalMs = kDefaultDeepSleepDurationMs;
        maxDeepSleepMs = kDefaultMaxDeepSleepMs;
        lightSleepDurationMs = kDefaultLightSleepDurationMs;
        idleTimeoutMs = kDefaultIdleTimeoutMs;
        enableTimerWakeup = kDefaultEnableTimerWakeup;
        enableNfcWakeup = kDefaultEnableNfcWakeup;
        nfcWakeupPin = kDefaultNfcWakeupPin;
        autoSleepEnabled = kDefaultAutoSleepEnabled;
        disableWiFiDuringSleep = kDefaultDisableWiFiDuringSleep;
        pn532SleepBetweenScans = kDefaultPn532SleepBetweenScans;
        smartSleepEnabled = kDefaultSmartSleepEnabled;
        modemSleepOnMqttDisconnect = kDefaultModemSleepOnMqttDisconnect;
        modemSleepDurationMs = kDefaultModemSleepDurationMs;
        smartSleepShortThresholdMs = kDefaultSmartSleepShortThresholdMs;
        smartSleepMediumThresholdMs = kDefaultSmartSleepMediumThresholdMs;
        activityTypeMask = kDefaultActivityTypeMask;
    }
};

struct Config
{
    static constexpr auto kMagicNumber{0x49534943}; // 'ISIC' in ASCII
    static constexpr auto kVersion{1};

    std::uint32_t magic{kMagicNumber};
    std::uint16_t version{kVersion};

    WiFiConfig wifi{};
    MqttConfig mqtt{};
    DeviceConfig device{};
    Pn532Config pn532{};
    AttendanceConfig attendance{};
    FeedbackConfig feedback{};
    HealthConfig health{};
    OtaConfig ota{};
    PowerConfig power{};

    [[nodiscard]] constexpr bool isValid() const
    {
        return magic == kMagicNumber && version == kVersion;
    }

    [[nodiscard]] bool isConfigured() const
    {
        return wifi.isConfigured() && mqtt.isConfigured() && device.isConfigured() && pn532.isConfigured() && attendance.isConfigured() && feedback.isConfigured() && health.isConfigured() && ota.isConfigured() && power.isConfigured(); // NOLINT
    }

    void restoreDefaults()
    {
        magic = kMagicNumber;
        version = kVersion;

        wifi.restoreDefaults();
        mqtt.restoreDefaults();
        device.restoreDefaults();
        pn532.restoreDefaults();
        attendance.restoreDefaults();
        feedback.restoreDefaults();
        health.restoreDefaults();
        ota.restoreDefaults();
        power.restoreDefaults();
    }

    static Config makeDefault()
    {
        return Config{};
    }
};
} // namespace isic

#endif // ISIC_CONFIG_HPP
