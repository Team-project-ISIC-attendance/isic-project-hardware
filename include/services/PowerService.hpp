#ifndef ISIC_SERVICES_POWERSERVICE_HPP
#define ISIC_SERVICES_POWERSERVICE_HPP

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"
#include "platform/PlatformWiFi.hpp"

namespace isic
{

class ConfigService;

/**
 * @brief RTC memory structure for persistence across deep sleep
 *
 * ESP8266 has 512 bytes of RTC memory that survives deep sleep.
 * We use a small portion to track sleep state and wakeup count.
 */
struct RtcData
{
    static constexpr std::uint32_t MAGIC{0x504F5752};

    std::uint32_t magic{0};
    std::uint32_t wakeupCount{0};
    std::uint32_t totalSleepMs{0};
    PowerState lastRequestedState{PowerState::Active};
    std::uint32_t remainingSleepMs{0}; // For chained deep sleep
    std::uint32_t crc32{0};

    [[nodiscard]] bool isValid() const
    {
        return magic == MAGIC;
    }
    void invalidate()
    {
        magic = 0;
    }
};

class PowerService : public ServiceBase
{
public:
    enum ActivityType : std::uint8_t
    {
        CardScanned = (1 << 0), // Bit 0: Card scanned event
        MqttMessage = (1 << 1), // Bit 1: MQTT message received
        WifiConnected = (1 << 2), // Bit 2: WiFi connected
        MqttConnected = (1 << 3), // Bit 3: MQTT connected
        NfcReady = (1 << 4), // Bit 4: NFC reader ready
    };

    PowerService(EventBus &bus, const PowerConfig &config);
    ~PowerService() override;

    PowerService(const PowerService &) = delete;
    PowerService &operator=(const PowerService &) = delete;
    PowerService(PowerService &&) = delete;
    PowerService &operator=(PowerService &&) = delete;

    // IService implementation
    Status begin() override;
    void loop() override;
    void end() override;

    [[nodiscard]] PowerState getCurrentState() const noexcept
    {
        return m_currentState;
    }
    [[nodiscard]] WakeupReason getLastWakeupReason() const noexcept
    {
        return m_wakeupReason;
    }
    [[nodiscard]] std::uint32_t getTimeSinceLastActivityMs() const noexcept
    {
        return millis() - m_lastActivityMs;
    }
    [[nodiscard]] bool isSleepPending() const noexcept
    {
        return m_sleepPending;
    }
    [[nodiscard]] std::uint32_t getWakeupCount() const noexcept
    {
        return m_metrics.wakeupCount;
    }
    [[nodiscard]] const PowerServiceMetrics &getMetrics() const noexcept
    {
        return m_metrics;
    }

    void requestSleep(PowerState state, std::uint32_t durationMs = 0);
    void cancelSleepRequest();
    void recordActivity();

private:
    void handleReadyState();
    void handleRunningState();
    void handleWifiConnected(const Event &event);
    void handleWifiDisconnected(const Event &event);
    void handleMqttConnected(const Event &event);
    void handleMqttDisconnected(const Event &event);
    void handleCardScanned(const Event &event);
    void handleMqttMessage(const Event &event);
    void handleNfcReady(const Event &event);

    PowerState selectSmartSleepDepth();
    [[nodiscard]] bool canEnterSleep() const;
    [[nodiscard]] std::uint32_t estimateIdleDuration() const;

    void executePendingSleep();

    void enterLightSleepAsync(std::uint32_t durationMs);
    void enterModemSleepAsync(std::uint32_t durationMs);
    void enterDeepSleepAsync(std::uint32_t durationMs);

    void wakeFromModemSleep();

    void checkIdleTimeout();
    void checkChainedSleep();

    void prepareForSleep(PowerState state);
    void notifyWakeup();
    [[nodiscard]] WakeupReason detectWakeupReason();

    void saveToRtcMemory();
    bool loadFromRtcMemory();
    [[nodiscard]] std::uint32_t calculateCrc32(const RtcData &data);

    void publishStateChange(PowerState newState, PowerState oldState);
    void publishSleepRequested(PowerState state, uint32_t durationMs);
    void publishWakeupOccurred(WakeupReason reason);

    void recordActivityInternal(ActivityType type);
    [[nodiscard]] bool isActivityTypeEnabled(ActivityType type) const;

    EventBus &m_bus;
    const PowerConfig &m_config;

    bool m_wifiReady{false}; // Set by WifiConnected/Disconnected events
    bool m_mqttReady{false}; // Set by MqttConnected/Disconnected events

    PowerState m_currentState{PowerState::Active};
    WakeupReason m_wakeupReason{WakeupReason::Unknown};

    PowerServiceMetrics m_metrics{};

    std::uint32_t m_lastActivityMs{0};

    // Sleep request state
    bool m_sleepPending{false};
    PowerState m_pendingSleepState{PowerState::Active};
    std::uint32_t m_pendingSleepDurationMs{0};
    std::uint32_t m_sleepRequestedAtMs{0};

    // Light sleep state machine
    bool m_lightSleepActive{false};
    std::uint32_t m_lightSleepStartMs{0};
    std::uint32_t m_lightSleepDurationMs{0};

    // Modem sleep state
    bool m_modemSleepActive{false};
    std::uint32_t m_modemSleepStartMs{0};
    std::uint32_t m_modemSleepDurationMs{0};

    // RTC data for deep sleep persistence
    RtcData rtcData_{};

    std::vector<EventBus::ScopedConnection> eventConnections_;
};
} // namespace isic

#endif // ISIC_SERVICES_POWERSERVICE_HPP
