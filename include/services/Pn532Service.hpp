#ifndef ISIC_PN532_SERVICE_NO_CARD_EVENT
#define ISIC_PN532_SERVICE_NO_CARD_EVENT

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <Adafruit_PN532.h>
#include <vector>

namespace isic
{
class Pn532Service : public ServiceBase
{
public:
    Pn532Service(EventBus &bus, const Config& config);
    ~Pn532Service() override;

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
private:
    void scanForCard();
    void handlePowerStateChange(const PowerEvent &power);

    EventBus &m_bus;
    const Pn532Config &m_config;
    const PowerConfig& m_powerConfig;
    Adafruit_PN532 *m_pn532{nullptr}; // for now raw pointer due to Adafruit library design //TODO: wrap in unique_ptr with custom deleter

    Pn532State m_pn532State{Pn532State::Uninitialized};
    Pn532Metrics m_metrics{};

    CardUid m_lastCardUid{};
    std::uint8_t m_lastCardUidLength{0};
    std::uint32_t m_lastCardReadMs{0};
    std::uint32_t m_lastPollMs{0};
    std::vector<EventBus::ScopedConnection> m_eventConnections{};

    bool m_cardPresent{false};
    bool m_isAsleep{false};
    bool m_irqWakeupEnabled{false};
};
} // namespace isic

#endif // ISIC_PN532_SERVICE_NO_CARD_EVENT