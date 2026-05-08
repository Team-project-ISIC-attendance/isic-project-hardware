#ifndef ISIC_SERVICES_POWERSERVICE_HPP
#define ISIC_SERVICES_POWERSERVICE_HPP

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <array>
#include <vector>

namespace isic
{
class AttendanceService;

class PowerService : public ServiceBase
{
public:
    enum ActivityType : std::uint8_t
    {
        CardScanned = (1 << 0), // Bit 0: card scanned event
        MqttMessage = (1 << 1), // Bit 1: MQTT message received
        WifiConnected = (1 << 2), // Bit 2: WiFi connected
        MqttConnected = (1 << 3), // Bit 3: MQTT connected
        NfcReady = (1 << 4), // Bit 4: NFC reader ready
    };

    PowerService(EventBus &bus, const PowerConfig &config, const AttendanceService &attendanceService);
    ~PowerService() override;

    PowerService(const PowerService &) = delete;
    PowerService &operator=(const PowerService &) = delete;
    PowerService(PowerService &&) = delete;
    PowerService &operator=(PowerService &&) = delete;

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

    [[nodiscard]] const PowerMetrics &getMetrics() const noexcept
    {
        return m_metrics;
    }

    void requestSleep(PowerState state, std::uint32_t durationMs = 0);
    void cancelSleepRequest();
    void recordActivity();
    void wakeToActive();

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
        obj["power_state"] = toString(getCurrentState());
        obj["pn532_target_mode"] = toString(m_pn532TargetMode);
        obj["last_wakeup_reason"] = toString(getLastWakeupReason());
        obj["time_since_last_activity_ms"] = getTimeSinceLastActivityMs();
        obj["light_sleep_entries"] = m_metrics.lightSleepEntries;
        obj["modem_sleep_entries"] = m_metrics.modemSleepEntries;
        obj["light_sleep_wakeups"] = m_metrics.lightSleepWakeups;
        obj["modem_sleep_wakeups"] = m_metrics.modemSleepWakeups;
        obj["burst_mode"] = m_burstMode;
        obj["burst_entries"] = m_metrics.burstEntries;
        obj["burst_exits"] = m_metrics.burstExits;
        obj["sleep_blocked"] = m_metrics.sleepBlocked;
        obj["sleep_blocked_ap"] = m_metrics.sleepBlockedByAp;
        obj["sleep_blocked_ota"] = m_metrics.sleepBlockedByOta;
        obj["sleep_suppressed_by_burst"] = m_metrics.sleepSuppressedByBurst;
        obj["network_aware_sleeps"] = m_metrics.networkAwareSleeps;
    }

private:
    enum class SleepBlockReason : std::uint8_t
    {
        None,
        AccessPoint,
        OtaUpdate,
        OfflinePending,
    };

    void handleWifiConnected(const Event &event);
    void handleWifiDisconnected(const Event &event);
    void handleMqttConnected(const Event &event);
    void handleMqttDisconnected(const Event &event);
    void handleCardScanned(const Event &event);
    void handleMqttMessage(const Event &event);
    void handleNfcReady(const Event &event);
    void handleWifiApStarted(const Event &event);
    void handleWifiApStopped(const Event &event);
    void handleOtaStarted(const Event &event);
    void handleOtaCompleted(const Event &event);
    void handleOtaError(const Event &event);

    [[nodiscard]] bool canEnterSleep(PowerState state, SleepBlockReason *reason = nullptr) const;
    void noteSleepBlocked(SleepBlockReason reason);
    void evaluateIdleState();
    void updateBurstState(std::uint32_t nowMs);
    void recordCardActivity(std::uint32_t timestampMs);
    [[nodiscard]] std::uint8_t countRecentCardScans(std::uint32_t nowMs) const;
    [[nodiscard]] bool isReaderReadyHoldActive(std::uint32_t nowMs) const;
    [[nodiscard]] bool shouldKeepEspAwakeForTraffic(std::uint32_t nowMs) const;
    [[nodiscard]] PowerState computeDesiredEspState(std::uint32_t nowMs) const;
    [[nodiscard]] Pn532PowerMode computeDesiredPn532Mode(std::uint32_t nowMs) const;
    void updatePn532Target(Pn532PowerMode targetMode);
    void executePendingSleep();

    void enterLightSleep();
    void enterModemSleep();

    void prepareForSleep(PowerState state);
    [[nodiscard]] WakeupReason detectWakeupReason();

    void publishStateChange(PowerState newState, PowerState oldState, Pn532PowerMode pn532TargetMode);
    void publishSleepRequested(PowerState state, std::uint32_t durationMs);
    void publishWakeupOccurred(WakeupReason reason);

    void recordActivityInternal(ActivityType type);
    [[nodiscard]] bool isActivityTypeEnabled(ActivityType type) const;

    EventBus &m_bus;
    const PowerConfig &m_config;
    const AttendanceService &m_attendanceService;

    bool m_wifiReady{false};
    bool m_mqttReady{false};
    bool m_apModeActive{false};
    bool m_otaUpdateActive{false};
    bool m_burstMode{false};
    bool m_burstSleepSuppressionActive{false};

    PowerState m_currentState{PowerState::Active};
    Pn532PowerMode m_pn532TargetMode{Pn532PowerMode::ActiveScan};
    WakeupReason m_wakeupReason{WakeupReason::Unknown};
    PowerMetrics m_metrics{};
    std::uint32_t m_lastActivityMs{0};
    std::uint32_t m_lastCardActivityMs{0};

    bool m_sleepPending{false};
    PowerState m_pendingSleepState{PowerState::Active};
    std::uint32_t m_pendingSleepDurationMs{0};
    std::uint32_t m_sleepRequestedAtMs{0};

    static constexpr std::size_t kRecentCardHistorySize{16};
    std::array<std::uint32_t, kRecentCardHistorySize> m_recentCardScanMs{};
    std::size_t m_recentCardScanIndex{0};

    std::vector<EventBus::ScopedConnection> eventConnections_;
};
} // namespace isic

#endif // ISIC_SERVICES_POWERSERVICE_HPP
