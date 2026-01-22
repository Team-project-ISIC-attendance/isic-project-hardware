#ifndef ISIC_PN532_SERVICE_NO_CARD_EVENT
#define ISIC_PN532_SERVICE_NO_CARD_EVENT

#include "services/ConfigService.hpp"
#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <Adafruit_PN532.h>
#include <atomic>
#include <vector>
#include <memory>

namespace isic
{
class Pn532Service : public ServiceBase
{
public:
    Pn532Service(EventBus &bus, ConfigService& confiService);
    ~Pn532Service() override = default;

    Pn532Service(const Pn532Service &) = delete;
    Pn532Service &operator=(const Pn532Service &) = delete;
    Pn532Service(Pn532Service &&) = delete;
    Pn532Service &operator=(Pn532Service &&) = delete;

    // IService implementation
    Status begin() override;
    void loop() override;
    void end() override;

    bool enterSleep();
    bool wakeup();

    bool enableIrqWakeup();
    void disableIrqWakeup();

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
        obj["card_reads"] = m_metrics.cardsRead;
        obj["reads_successful"] = m_metrics.successfulReads;
        obj["reads_failed"] = m_metrics.readErrors;
        obj["recoveries"] = m_metrics.recoveryAttempts;
    }

private:
    void startDetection();
    void handleCardDetected();
    void pollForCard();
    void publishCardEvent(const std::uint8_t* uid, std::uint8_t uidLength);
    void handlePowerStateChange(const PowerEvent &power);
    bool reinitializePn532();
    bool recoverIrqMode();
    bool waitForIrqHigh(std::uint32_t timeoutMs);
    bool attachIrqInterrupt();
    void detachIrqInterrupt();

    static void IRAM_ATTR isrTrampoline();
    inline static Pn532Service* s_activeInstance{nullptr};

    EventBus &m_bus;
    ConfigService &m_configService;
    const Pn532Config &m_config; // cached config reference

    std::unique_ptr<Adafruit_PN532> m_pn532{nullptr};

    Pn532State m_pn532State{Pn532State::Uninitialized};
    Pn532Metrics m_metrics{};

    CardUid m_lastCardUid{};
    std::uint8_t m_lastCardUidLength{0};
    std::uint32_t m_lastCardReadMs{0};
    std::uint32_t m_lastPollMs{0};
    std::vector<EventBus::ScopedConnection> m_eventConnections{};

    std::atomic_bool m_irqTriggered{false};
    bool m_isAsleep{false};
    bool m_irqWakeupEnabled{false};
    bool m_detectionStarted{false};
    bool m_useIrqMode{false};
    std::uint32_t m_lastDetectionFailureMs{0};
    std::uint32_t m_pollIntervalMs{0};
    std::uint8_t m_consecutiveErrors{0};
    int m_irqCurr{HIGH};  // Current IRQ pin state for edge detection
    int m_irqPrev{HIGH};  // Previous IRQ pin state for edge detection
};
} // namespace isic

#endif // ISIC_PN532_SERVICE_NO_CARD_EVENT
