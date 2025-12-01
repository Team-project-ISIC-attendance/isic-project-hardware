#ifndef HARDWARE_USERFEEDBACKSERVICE_HPP
#define HARDWARE_USERFEEDBACKSERVICE_HPP

/**
 * @file UserFeedbackService.hpp
 * @brief User feedback via LED and buzzer for the ISIC system.
 *
 * Provides non-blocking, queued feedback patterns for user notification.
 */

#include <Arduino.h>

#include <atomic>
#include <cstdint>

#include "AppConfig.hpp"
#include "core/EventBus.hpp"
#include "core/Result.hpp"

namespace isic {

    /**
     * @brief User feedback signal pattern.
     */
    struct FeedbackPattern {
        std::uint16_t ledOnMs{0};
        std::uint16_t ledOffMs{0};
        std::uint16_t beepMs{0};
        std::uint16_t beepFrequencyHz{2000};
        std::uint8_t repeatCount{1};
        bool useErrorLed{false};
    };

    /**
     * @brief User feedback service for LED and buzzer control.
     *
     * Provides non-blocking, queued feedback to users for:
     * - Card read success/failure
     * - System state changes
     * - OTA progress
     *
     * Design:
     * - All operations are non-blocking (queued)
     * - Separate RTOS task handles actual GPIO operations
     * - Configurable patterns for each signal type
     */
    class UserFeedbackService : public IEventListener {
    public:
        explicit UserFeedbackService(EventBus& bus);
        ~UserFeedbackService() override;

        // Non-copyable, non-movable
        UserFeedbackService(const UserFeedbackService&) = delete;
        UserFeedbackService& operator=(const UserFeedbackService&) = delete;
        UserFeedbackService(UserFeedbackService&&) = delete;
        UserFeedbackService& operator=(UserFeedbackService&&) = delete;

        /**
         * @brief Initialize the feedback service.
         */
        [[nodiscard]] Status begin(const AppConfig& cfg);

        /**
         * @brief Stop the feedback service.
         */
        void stop();

        // ==================== Quick Signal Methods ====================

        /**
         * @brief Signal successful card read.
         * Short green LED blink + short beep.
         */
        void signalSuccess();

        /**
         * @brief Signal card read error.
         * Red LED pattern + multi-beep.
         */
        void signalError();

        /**
         * @brief Signal system is processing.
         * Slow LED pulse.
         */
        void signalProcessing();

        /**
         * @brief Signal network connected.
         */
        void signalConnected();

        /**
         * @brief Signal network disconnected.
         */
        void signalDisconnected();

        /**
         * @brief Signal OTA update started.
         */
        void signalOtaStarted();

        /**
         * @brief Signal OTA update completed.
         */
        void signalOtaComplete();

        /**
         * @brief Signal with custom pattern.
         */
        void signalCustom(const FeedbackPattern& pattern);

        // ==================== Direct Control (bypasses queue) ====================

        /**
         * @brief Turn success LED on/off directly.
         */
        void setSuccessLed(bool on);

        /**
         * @brief Turn error LED on/off directly.
         */
        void setErrorLed(bool on);

        /**
         * @brief Play a tone directly.
         */
        void playTone(std::uint16_t frequencyHz, std::uint16_t durationMs);

        // ==================== Configuration ====================

        /**
         * @brief Update configuration at runtime.
         */
        void updateConfig(const FeedbackConfig& cfg);

        /**
         * @brief Enable/disable all feedback.
         */
        void setEnabled(bool enabled);

        /**
         * @brief Check if feedback is enabled.
         */
        [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(); }

        // ==================== IEventListener ====================

        void onEvent(const Event& event) override;

    private:
        // Task for processing feedback queue
        static void feedbackTaskThunk(void* arg);
        void feedbackTask();

        // Internal signal execution
        void executePattern(const FeedbackPattern& pattern);
        void blinkLed(std::uint8_t pin, bool activeHigh, std::uint16_t onMs, std::uint16_t offMs);
        void beep(std::uint16_t frequencyHz, std::uint16_t durationMs);

        // Queue a pattern for async execution
        bool queuePattern(const FeedbackPattern& pattern);

        // Predefined patterns
        static constexpr FeedbackPattern PATTERN_SUCCESS{100, 0, 50, 2000, 1, false};
        static constexpr FeedbackPattern PATTERN_ERROR{200, 100, 100, 1500, 3, true};
        static constexpr FeedbackPattern PATTERN_PROCESSING{500, 500, 0, 0, 0, false};
        static constexpr FeedbackPattern PATTERN_CONNECTED{50, 50, 30, 2500, 2, false};
        static constexpr FeedbackPattern PATTERN_DISCONNECTED{300, 0, 200, 1000, 1, true};
        static constexpr FeedbackPattern PATTERN_OTA_START{100, 100, 100, 1800, 3, false};
        static constexpr FeedbackPattern PATTERN_OTA_COMPLETE{50, 50, 50, 2500, 5, false};

        EventBus& m_bus;
        const FeedbackConfig* m_cfg{nullptr};

        // Task management
        TaskHandle_t m_taskHandle{nullptr};
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_enabled{true};

        // Feedback queue
        QueueHandle_t m_feedbackQueue{nullptr};

        // GPIO state
        std::uint8_t m_successLedPin{0};
        std::uint8_t m_errorLedPin{0};
        std::uint8_t m_buzzerPin{0};
        bool m_ledActiveHigh{true};

        // LEDC channel for buzzer PWM
        static constexpr std::uint8_t BUZZER_LEDC_CHANNEL = 0;
        static constexpr std::uint8_t BUZZER_LEDC_RESOLUTION = 8;

        // EventBus subscription
        EventBus::ListenerId m_subscriptionId{0};
    };

}  // namespace isic

#endif  // HARDWARE_USERFEEDBACKSERVICE_HPP

