#ifndef ISIC_PN532_SERVICE_NO_CARD_EVENT
#define ISIC_PN532_SERVICE_NO_CARD_EVENT

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"
#include "services/ConfigService.hpp"

#include <Adafruit_PN532.h>
#include <memory>
#include <vector>

namespace isic
{
class Pn532Service : public ServiceBase
{
public:
    Pn532Service(EventBus &bus, ConfigService &configService);
    ~Pn532Service() override = default;

    Pn532Service(const Pn532Service &) = delete;
    Pn532Service &operator=(const Pn532Service &) = delete;
    Pn532Service(Pn532Service &&) = delete;
    Pn532Service &operator=(Pn532Service &&) = delete;

    Status begin() override;
    void loop() override;
    void end() override;

    [[nodiscard]] bool isAsleep() const
    {
        return m_isAsleep;
    }
    [[nodiscard]] bool isReady() const
    {
        return m_pn532State == Pn532State::Ready;
    }
    [[nodiscard]] Pn532State getNfcState() const
    {
        return m_pn532State;
    }
    [[nodiscard]] const CardUid &getLastCardUid() const
    {
        return m_lastCardUid;
    }
    [[nodiscard]] std::uint8_t getLastCardUidLength() const
    {
        return m_lastCardUidLength;
    }
    [[nodiscard]] const Pn532Metrics &getMetrics() const
    {
        return m_metrics;
    }

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
        obj["power_mode"] = toString(m_powerMode);
        obj["card_reads"] = m_metrics.cardsRead;
        obj["reads_successful"] = m_metrics.successfulReads;
        obj["reads_failed"] = m_metrics.readErrors;
        obj["recoveries"] = m_metrics.recoveryAttempts;
        obj["active_irq_failures"] = m_metrics.activeIrqFailures;
        obj["active_poll_fallback_entries"] = m_metrics.activePollFallbackEntries;
        obj["sleep_entries"] = m_metrics.sleepEntries;
        obj["sleep_wake_reads"] = m_metrics.sleepWakeReads;
    }

private:
    void startDetection();
    void handleCardDetected();
    void pollForCard(std::uint32_t timeoutMs);
    void pollWhileAsleep();
    bool enterSleep();
    bool wakeup();
    void noteActiveIrqFailure(std::uint32_t nowMs, const char *reason);
    void activateActivePollingFallback(std::uint32_t nowMs, const char *reason);
    void maybeRecoverActiveIrq(std::uint32_t nowMs);
    [[nodiscard]] bool isUsingActiveIrqPrimary() const;
    [[nodiscard]] std::uint32_t getActivePollingIntervalMs() const;
    [[nodiscard]] std::uint32_t getActivePollingTimeoutMs() const;
    void publishCardEvent(const std::uint8_t *uid, std::uint8_t uidLength);
    void handlePowerStateChange(const PowerEvent &power);
    [[nodiscard]] bool shouldSleepBetweenScans() const;
    [[nodiscard]] bool shouldDelaySleepAfterRead(std::uint32_t nowMs) const;
    [[nodiscard]] std::uint32_t getPollIntervalWhileAsleep() const;
    void enterRecovering(std::uint32_t nowMs);
    bool reinitializePn532();
    bool recoverIrqMode();
    bool waitForIrqHigh(std::uint32_t timeoutMs);

    EventBus &m_bus;
    ConfigService &m_configService;
    const Pn532Config &m_config;

    std::unique_ptr<Adafruit_PN532> m_pn532{nullptr};

    Pn532State m_pn532State{Pn532State::Uninitialized};
    Pn532Metrics m_metrics{};

    CardUid m_lastCardUid{};
    std::uint8_t m_lastCardUidLength{0};
    std::uint32_t m_lastCardReadMs{0};
    std::uint32_t m_lastPollMs{0};
    std::vector<EventBus::ScopedConnection> m_eventConnections{};

    bool m_isAsleep{false};
    bool m_activeIrqConfigured{false};
    bool m_activeIrqEnabled{false};
    bool m_activePollingFallback{false};
    bool m_detectionStarted{false};
    PowerState m_powerState{PowerState::Active};
    Pn532PowerMode m_targetPowerMode{Pn532PowerMode::ActiveScan};
    Pn532PowerMode m_powerMode{Pn532PowerMode::ActiveScan};
    std::uint32_t m_activeIrqRetryAtMs{0};
    std::uint32_t m_lastDetectionFailureMs{0};
    std::uint32_t m_pollIntervalMs{0};
    std::uint32_t m_wakeRetryAtMs{0};
    std::uint8_t m_consecutiveErrors{0};
    int m_irqCurr{HIGH};
    int m_irqPrev{HIGH};
};
} // namespace isic

#endif // ISIC_PN532_SERVICE_NO_CARD_EVENT
