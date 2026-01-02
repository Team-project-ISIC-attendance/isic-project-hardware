#ifndef ISIC_SERVICES_FEEDBACKSERVICE_HPP
#define ISIC_SERVICES_FEEDBACKSERVICE_HPP

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <array>
#include <vector>

namespace isic
{
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

    void setEnabled(const bool enabled) noexcept
    {
        m_enabled = enabled;
    }
    [[nodiscard]] bool isEnabled() const noexcept
    {
        return m_enabled;
    }

    void clearQueue() noexcept;
    void stopCurrent() noexcept;

    [[nodiscard]] std::uint8_t getQueueCount() const noexcept
    {
        return m_queueCount;
    }
    [[nodiscard]] bool isBusy() const noexcept
    {
        return m_inPattern || m_queueCount > 0;
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
    FeedbackPattern m_currentPattern{};
    std::uint8_t m_currentRepeat{0};
    std::uint32_t m_cycleStartMs{0};
    bool m_inPattern{false};

    bool m_enabled{true};
    bool m_ledCurrentState{false};
    bool m_buzzerCurrentState{false};

    std::vector<EventBus::ScopedConnection> m_eventConnections{};
};
} // namespace isic

#endif // ISIC_SERVICES_FEEDBACKSERVICE_HPP
