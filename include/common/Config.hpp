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
    static constexpr auto kStationConnectRetryDelayMs{500}; // 500 milliseconds
    static constexpr auto kStationConnectionTimeoutMs{10'000}; // 10 seconds
    static constexpr auto kStationMaxFastConnectionAttempts{10};
    static constexpr auto kStationFastReconnectIntervalMs{5'000}; // 5 seconds
    static constexpr auto kStationSlowReconnectIntervalMs{600'000}; // 10 minutes
    static constexpr auto kStationHasEverConnected{false};
    static constexpr auto kStationPowerSaveEnabled{false};
    static constexpr auto kAccessPointSsidPrefix{"ISIC-Setup-"};
    static constexpr auto kAccessPointDefaultPassword{"isic1234"};
    static constexpr auto kAccessPointModeTimeoutMs{300'000}; // 5 minutes

    std::string stationSsid{};
    std::string stationPassword{};
#ifdef ISIC_WIFI_EDUROAM
    std::string stationUsername{};
#endif
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
#ifdef ISIC_WIFI_EDUROAM
        stationUsername.clear();
#endif
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
#ifdef ISIC_PLATFORM_ESP8266
        static constexpr auto kMaxPayloadSizeBytes{4024};
#else
        static constexpr auto kMaxPayloadSizeBytes{4024};
#endif
    };
    static constexpr auto kDefaultBrokerPort{1883};
    static constexpr auto kDefaultBaseTopic{"device"};
    static constexpr auto kDefaultKeepAliveIntervalSec{60}; // 60 seconds
    static constexpr auto kDefaultReconnectMinIntervalMs{1'000}; // 1 second
    static constexpr auto kDefaultReconnectMaxIntervalMs{30'000}; // 30 seconds

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
#ifdef ISIC_PLATFORM_ESP8266
    static constexpr auto kDefaultSpiSckPin{14}; // D5
    static constexpr auto kDefaultSpiMisoPin{12}; // D6
    static constexpr auto kDefaultSpiMosiPin{13}; // D7
    static constexpr auto kDefaultSpiCsPin{5}; // D1
    static constexpr auto kDefaultIrqPin{4}; // D2
#elif defined(ISIC_PLATFORM_ESP32)
    static constexpr auto kDefaultSpiSckPin{14};
    static constexpr auto kDefaultSpiMisoPin{12};
    static constexpr auto kDefaultSpiMosiPin{13};
    static constexpr auto kDefaultSpiCsPin{5};
    static constexpr auto kDefaultIrqPin{27};
#else
#error "Unsupported platform"
#endif
    static constexpr auto kDefaultReadTimeoutMs{200}; // 200 milliseconds
    static constexpr auto kDefaultRecoveryDelayMs{2'000}; // 2 seconds
    static constexpr auto kDefaultMaxConsecutiveErrors{5};
    static constexpr auto kDefaultPollIntervalMs{0}; // Fallback polling interval when IRQ disabled

    std::uint32_t readTimeoutMs{kDefaultReadTimeoutMs};
    std::uint32_t recoveryDelayMs{kDefaultRecoveryDelayMs};
    std::uint32_t pollIntervalMs{kDefaultPollIntervalMs}; // 0 = use IRQ (default behavior when irqPin valid)
    std::uint8_t spiSckPin{kDefaultSpiSckPin};
    std::uint8_t spiMisoPin{kDefaultSpiMisoPin};
    std::uint8_t spiMosiPin{kDefaultSpiMosiPin};
    std::uint8_t spiCsPin{kDefaultSpiCsPin};
    std::uint8_t irqPin{kDefaultIrqPin};

    std::uint8_t maxConsecutiveErrors{kDefaultMaxConsecutiveErrors};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // PN532 always considered configured
    }

    [[nodiscard]] constexpr bool useIrq() const
    {
        return irqPin != 0xFF && (pollIntervalMs == 0);
    }

    constexpr void restoreDefaults()
    {
        readTimeoutMs = kDefaultReadTimeoutMs;
        recoveryDelayMs = kDefaultRecoveryDelayMs;
        pollIntervalMs = kDefaultPollIntervalMs;
        spiSckPin = kDefaultSpiSckPin;
        spiMisoPin = kDefaultSpiMisoPin;
        spiMosiPin = kDefaultSpiMosiPin;
        spiCsPin = kDefaultSpiCsPin;
        irqPin = kDefaultIrqPin;
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

    static constexpr auto kDefaultDebounceIntervalMs{3'000}; // 3 seconds
    static constexpr auto kDefaultBatchMaxSize{5};
    static constexpr auto kDefaultOfflineBufferSize{20};
    static constexpr auto kDefaultBatchFlushIntervalMs{10'000}; // 10 seconds
    static constexpr auto kDefaultBatchingEnabled{false}; // Disabled by default
    static constexpr auto kDefaultOfflineBufferFlushIntervalMs{5'000}; // 5 seconds
    static constexpr auto kDefaultOfflineQueuePolicy{OfflineQueuePolicy::DropOldest}; // Drop oldest by default

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
#ifdef ISIC_PLATFORM_ESP8266
    static constexpr auto kDefaultLedEnabled{true};
    static constexpr auto kDefaultLedPin{2}; // D4 / built-in ESP-12F LED
    static constexpr auto kDefaultLedRedPin{0xFF};
    static constexpr auto kDefaultLedGreenPin{0xFF};
    static constexpr auto kDefaultLedBluePin{0xFF};
    static constexpr auto kDefaultBuzzerPin{16}; // D0
#elif defined(ISIC_PLATFORM_ESP32)
    static constexpr auto kDefaultLedEnabled{false};
    static constexpr auto kDefaultLedPin{0xFF};
    static constexpr auto kDefaultLedRedPin{25};
    static constexpr auto kDefaultLedGreenPin{26};
    static constexpr auto kDefaultLedBluePin{23};
    static constexpr auto kDefaultBuzzerPin{32};
#else
#error "Unsupported platform"
#endif
    static constexpr auto kDefaultBuzzerEnabled{true};
#ifdef ISIC_PLATFORM_ESP8266
    static constexpr auto kDefaultLedActiveHigh{false}; // built-in ESP-12F LED is active low
#else
    static constexpr auto kDefaultLedActiveHigh{true}; // common cathode = active high
#endif
    static constexpr auto kDefaultBeepFrequencyHz{2'000};
    static constexpr auto kDefaultSuccessBlinkDurationMs{100};
    static constexpr auto kDefaultErrorBlinkDurationMs{200};

    std::uint16_t beepFrequencyHz{kDefaultBeepFrequencyHz};
    std::uint16_t successBlinkDurationMs{kDefaultSuccessBlinkDurationMs};
    std::uint16_t errorBlinkDurationMs{kDefaultErrorBlinkDurationMs};
    std::uint8_t ledPin{kDefaultLedPin};           ///< Single-color LED (fallback)
    std::uint8_t ledRedPin{kDefaultLedRedPin};
    std::uint8_t ledGreenPin{kDefaultLedGreenPin};
    std::uint8_t ledBluePin{kDefaultLedBluePin};
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
        ledRedPin = kDefaultLedRedPin;
        ledGreenPin = kDefaultLedGreenPin;
        ledBluePin = kDefaultLedBluePin;
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
        static constexpr auto kRssiCriticalThresholdDbm {-90};
        static constexpr auto kRssiWarningThresholdDbm{-80};
        static constexpr auto kFragmentationWarningThresholdPercent{50};
    };

    static constexpr auto kDefaultEnabled{true};
    static constexpr auto kDefaultHealthCheckIntervalMs{1'800'000}; // 30 minutes
    static constexpr auto kDefaultStatusUpdateIntervalMs{600'000}; // 10 minutes
    static constexpr auto kDefaultMetricsPublishIntervalMs{3'600'000}; // 1 hour
    static constexpr auto kDefaultPublishToMqtt{true};

    std::uint32_t healthCheckIntervalMs{kDefaultHealthCheckIntervalMs};
    std::uint32_t statusUpdateIntervalMs{kDefaultStatusUpdateIntervalMs};
    std::uint32_t metricsPublishIntervalMs{kDefaultMetricsPublishIntervalMs};
    bool publishToMqtt{kDefaultPublishToMqtt};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // Always considered configured
    }

    constexpr void restoreDefaults()
    {
        healthCheckIntervalMs = kDefaultHealthCheckIntervalMs;
        statusUpdateIntervalMs = kDefaultStatusUpdateIntervalMs;
        metricsPublishIntervalMs = kDefaultMetricsPublishIntervalMs;
        publishToMqtt = kDefaultPublishToMqtt;
    }
};

struct OtaConfig
{
    struct Constants
    {
        static constexpr auto kDefaultIntervalTimeDownload{100};
        static constexpr auto kDefaultCheckStuckTimeMs{2000};
        static constexpr auto kProgressPublishIntervalMs{500};
        static constexpr auto kManifestTimeoutMs{5000}; // Short timeout for JSON manifest only
    };
    static constexpr auto kDefaultEnabled{true};
    static constexpr auto kDefaultCheckOnConnect{true};
    static constexpr auto kDefaultTimeoutMs{30'000}; // 30 seconds

    std::string serverUrl{}; // e.g., "http://192.168.0.186:8080"
    std::string username{};
    std::string password{};
    std::uint32_t timeoutMs{kDefaultTimeoutMs};
    bool enabled{kDefaultEnabled};
    bool checkOnConnect{kDefaultCheckOnConnect};

    [[nodiscard]] bool isConfigured() const
    {
        return !serverUrl.empty();
    }

    void restoreDefaults()
    {
        enabled = kDefaultEnabled;
        checkOnConnect = kDefaultCheckOnConnect;
        timeoutMs = kDefaultTimeoutMs;
        serverUrl.clear();
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

    static constexpr auto kDefaultReaderIdleTimeoutMs{30'000}; // 30 seconds
    static constexpr auto kDefaultModemSleepAfterMs{300'000}; // 5 minutes
    static constexpr auto kDefaultPn532SleepAfterMs{10'000};  // 10 seconds
    static constexpr auto kDefaultReaderReadyHoldMs{5'000};   // 5 seconds
    static constexpr auto kDefaultBurstWindowMs{15'000};      // 15 seconds
    static constexpr auto kDefaultBurstScanCount{3};
    static constexpr auto kDefaultBurstHoldMs{45'000};        // 45 seconds
    static constexpr auto kDefaultEnableNfcWakeup{true};
    static constexpr auto kDefaultNfcWakeupPin{Pn532Config::kDefaultIrqPin};
    static constexpr auto kDefaultAutoSleepEnabled{true};
    static constexpr auto kDefaultDisableWiFiDuringSleep{true};
    static constexpr auto kDefaultPn532SleepBetweenScans{true};
    static constexpr auto kDefaultModemSleepOnMqttDisconnect{true};
    static constexpr auto kDefaultActivityTypeMask{0b00001}; // Card only by default

    std::uint32_t readerIdleTimeoutMs{kDefaultReaderIdleTimeoutMs};
    std::uint32_t modemSleepAfterMs{kDefaultModemSleepAfterMs};
    std::uint32_t pn532SleepAfterMs{kDefaultPn532SleepAfterMs};
    std::uint32_t readerReadyHoldMs{kDefaultReaderReadyHoldMs};
    std::uint32_t burstWindowMs{kDefaultBurstWindowMs};
    std::uint32_t burstHoldMs{kDefaultBurstHoldMs};
    std::uint8_t nfcWakeupPin{kDefaultNfcWakeupPin};
    std::uint8_t activityTypeMask{kDefaultActivityTypeMask};
    std::uint8_t burstScanCount{kDefaultBurstScanCount};
    bool enableNfcWakeup{kDefaultEnableNfcWakeup};
    bool autoSleepEnabled{kDefaultAutoSleepEnabled};
    bool disableWiFiDuringSleep{kDefaultDisableWiFiDuringSleep};
    bool pn532SleepBetweenScans{kDefaultPn532SleepBetweenScans};
    bool modemSleepOnMqttDisconnect{kDefaultModemSleepOnMqttDisconnect};

    [[nodiscard]] constexpr bool isConfigured() const // NOLINT
    {
        return true; // Always considered configured
    }

    constexpr void restoreDefaults()
    {
        readerIdleTimeoutMs = kDefaultReaderIdleTimeoutMs;
        modemSleepAfterMs = kDefaultModemSleepAfterMs;
        pn532SleepAfterMs = kDefaultPn532SleepAfterMs;
        readerReadyHoldMs = kDefaultReaderReadyHoldMs;
        burstWindowMs = kDefaultBurstWindowMs;
        burstHoldMs = kDefaultBurstHoldMs;
        enableNfcWakeup = kDefaultEnableNfcWakeup;
        nfcWakeupPin = kDefaultNfcWakeupPin;
        burstScanCount = kDefaultBurstScanCount;
        autoSleepEnabled = kDefaultAutoSleepEnabled;
        disableWiFiDuringSleep = kDefaultDisableWiFiDuringSleep;
        pn532SleepBetweenScans = kDefaultPn532SleepBetweenScans;
        modemSleepOnMqttDisconnect = kDefaultModemSleepOnMqttDisconnect;
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
