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
    std::uint8_t pendingNfcWakeup{0};  // Set when entering deep sleep with NFC wakeup
    std::uint8_t reserved[3]{};        // Padding for alignment
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
        return m_flags.sleepPending;
    }
    [[nodiscard]] std::uint32_t getWakeupCount() const noexcept
    {
        return m_metrics.wakeupCount;
    }
    [[nodiscard]] const PowerMetrics &getMetrics() const noexcept
    {
        return m_metrics;
    }
    [[nodiscard]] bool isPendingNfcWakeup() const noexcept
    {
        return m_pendingNfcWakeup;
    }
    void clearPendingNfcWakeup() noexcept
    {
        m_pendingNfcWakeup = false;
    }

    void requestSleep(PowerState state, std::uint32_t durationMs = 0);
    void cancelSleepRequest();
    void recordActivity();

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
        obj["last_wakeup_reason"] = toString(getLastWakeupReason());
        obj["time_since_last_activity_ms"] = getTimeSinceLastActivityMs();

        obj["light_sleep_cycles"] = m_metrics.lightSleepCycles;
        obj["modem_sleep_cycles"] = m_metrics.modemSleepCycles;
        obj["deep_sleep_cycles"] = m_metrics.deepSleepCycles;

        obj["wakeup_count"] = m_metrics.wakeupCount;
        obj["smart_sleep_used"] = m_metrics.smartSleepUsed;
        obj["network_aware_sleeps"] = m_metrics.networkAwareSleeps;
    }

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

    void wakeFromSleep();
    [[nodiscard]] std::uint32_t getDurationForState(PowerState state) const;

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
    void setNfcWakeGate(bool enabled);

    EventBus &m_bus;
    const PowerConfig &m_config;

    // Packed flags to save RAM (6 bools -> 1 byte instead of 6 bytes)
    struct Flags
    {
        std::uint8_t wifiReady : 1;
        std::uint8_t mqttReady : 1;
        std::uint8_t sleepPending : 1;
        std::uint8_t sleepActive : 1;      // Unified: light or modem sleep active
        std::uint8_t isModemSleep : 1;     // If sleepActive: true=modem, false=light
        std::uint8_t reserved : 3;
    };
    Flags m_flags{};

    PowerState m_currentState{PowerState::Active};
    PowerState m_pendingSleepState{PowerState::Active};
    WakeupReason m_wakeupReason{WakeupReason::Unknown};

    PowerMetrics m_metrics{};

    std::uint32_t m_lastActivityMs{0};
    std::uint32_t m_pendingSleepDurationMs{0};
    std::uint32_t m_sleepRequestedAtMs{0};

    // Unified sleep timer (light/modem share same tracking)
    std::uint32_t m_sleepStartMs{0};
    std::uint32_t m_sleepDurationMs{0};

    // RTC data for deep sleep persistence
    RtcData rtcData_{};

    // Pending NFC wakeup - keep awake until card is read
    bool m_pendingNfcWakeup{false};

    std::vector<EventBus::ScopedConnection> eventConnections_;
};
} // namespace isic

#endif // ISIC_SERVICES_POWERSERVICE_HPP
