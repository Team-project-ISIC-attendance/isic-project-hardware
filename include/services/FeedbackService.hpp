#ifndef ISIC_SERVICES_FEEDBACKSERVICE_HPP
#define ISIC_SERVICES_FEEDBACKSERVICE_HPP

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <array>
#include <vector>

namespace isic
{
class FeedbackService : public ServiceBase
{
public:
    FeedbackService(EventBus &bus, FeedbackConfig &config);
    ~FeedbackService() override = default;

    FeedbackService(const FeedbackService &) = delete;
    FeedbackService &operator=(const FeedbackService &) = delete;
    FeedbackService(FeedbackService &&) = delete;
    FeedbackService &operator=(FeedbackService &&) = delete;

    // IService implementation
    Status begin() override;
    void loop() override;
    void end() override;

    void signalSuccess();
    void signalError();
    void signalProcessing();
    void signalConnected();
    void signalDisconnected();
    void signalOtaStart();
    void signalOtaComplete();
    void signalCustom(const FeedbackPattern &pattern);

    void beepOnce(std::uint16_t durationMs = 100);
    void ledOnce(std::uint16_t durationMs = 100);

    void clearQueue() noexcept;
    void stopCurrent() noexcept;

    [[nodiscard]] bool isEnabled() const noexcept
    {
        return m_enabled;
    }

    [[nodiscard]] std::uint8_t getQueueCount() const noexcept
    {
        return m_queueCount;
    }
    [[nodiscard]] bool isBusy() const noexcept
    {
        return m_inPattern || m_queueCount > 0;
    }

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
    }

private:
    void queuePattern(const FeedbackPattern &pattern);
    void executePattern(const FeedbackPattern &pattern);

    void setLed(bool on);
    void setBuzzer(bool on, std::uint16_t frequencyHz = 0);

    EventBus &m_bus;
    FeedbackConfig &m_config;

    // Pattern queue (circular buffer)
    std::array<FeedbackPattern, FeedbackConfig::Constants::kPatternQueueSize> m_patternQueue{};
    std::uint8_t m_queueHead{0}; ///< Read index
    std::uint8_t m_queueTail{0}; ///< Write index
    std::uint8_t m_queueCount{0}; ///< Current queue size

    // Current pattern execution state
    FeedbackPattern m_currentPattern{}; ///< Currently executing pattern
    std::uint8_t m_currentRepeat{0}; ///< Current repeat count
    std::uint32_t m_cycleStartMs{0}; ///< Start time of current cycle
    bool m_inPattern{false}; ///< Whether a pattern is currently being executed

    bool m_enabled{true}; ///< Service enabled flag
    bool m_ledCurrentState{false}; ///< Current LED state
    bool m_buzzerCurrentState{false}; ///< Current buzzer state

    // Subscribed events
    std::vector<EventBus::ScopedConnection> m_eventConnections{};
};
} // namespace isic

#endif // ISIC_SERVICES_FEEDBACKSERVICE_HPP
