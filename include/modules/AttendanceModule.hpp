#ifndef HARDWARE_ATTENDANCEMODULE_HPP
#define HARDWARE_ATTENDANCEMODULE_HPP

/**
 * @file AttendanceModule.hpp
 * @brief High-throughput attendance processing module.
 *
 * Handles NFC card events, debouncing, and attendance record
 * enrichment before routing to the AttendanceBatcher.
 */

#include <Arduino.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "AppConfig.hpp"
#include "core/IModule.hpp"
#include "core/EventBus.hpp"
#include "core/IHealthCheck.hpp"
#include "core/Result.hpp"
#include "drivers/Pn532Driver.hpp"
#include "services/PowerService.hpp"

// Forward declarations
namespace isic {
    class UserFeedbackService;
    class AttendanceBatcher;
}

namespace isic {

    /**
     * @brief Metrics for monitoring attendance processing performance.
     */
    struct AttendanceMetrics {
        std::uint32_t totalCardsProcessed{0};
        std::uint32_t cardsDroppedDebounce{0};
        std::uint32_t cardsDroppedQueueFull{0};
        std::uint32_t eventsPublished{0};
        std::uint32_t eventsQueued{0};
        std::uint32_t eventsBatched{0};

        std::size_t currentQueueSize{0};
        std::size_t peakQueueSize{0};

        std::uint64_t lastCardProcessedMs{0};
        std::uint64_t lastEventPublishedMs{0};

        bool highLoadDetected{false};
    };

    /**
     * @brief Attendance record in the offline buffer.
     */
    struct BufferedAttendance {
        AttendanceRecord record{};
        bool pending{false};
        std::uint8_t sendAttempts{0};
    };

    /**
     * @brief High-throughput, non-blocking attendance processing module.
     *
     * Architecture:
     * - Listens for CardScanned events from PN532 driver
     * - Applies debounce and deduplication logic
     * - Enriches with metadata (timestamp, device ID, location)
     * - Routes to AttendanceBatcher for batched MQTT publishing
     * - Handles offline buffering when MQTT is unavailable
     * - Provides user feedback via UserFeedbackService
     * - Provides health status and metrics
     *
     * Design for high load:
     * - PN532 scanning is never blocked by MQTT publishing
     * - Separate FreeRTOS queue for card events
     * - Configurable backpressure strategies
     * - Graceful degradation under load
     */
    class AttendanceModule : public IModule, public IHealthCheck, public IEventListener {
    public:
        /**
         * @brief Construct AttendanceModule.
         */
        AttendanceModule(EventBus& bus,
                         Pn532Driver& pn532,
                         PowerService& powerService);

        ~AttendanceModule() override;

        // Non-copyable, non-movable
        AttendanceModule(const AttendanceModule&) = delete;
        AttendanceModule& operator=(const AttendanceModule&) = delete;
        AttendanceModule(AttendanceModule&&) = delete;
        AttendanceModule& operator=(AttendanceModule&&) = delete;

        /**
         * @brief Initialize the module with configuration.
         */
        [[nodiscard]] Status begin(const AppConfig& cfg);

        /**
         * @brief Set optional dependencies.
         */
        void setFeedbackService(UserFeedbackService* feedback) { m_feedback = feedback; }
        void setBatcher(AttendanceBatcher* batcher) { m_batcher = batcher; }

        // ==================== IModule Interface ====================

        void start() override;
        void stop() override;
        void handleEvent(const Event& event) override;
        void handleConfigUpdate(const AppConfig& config) override;

        // ==================== IEventListener Interface ====================

        void onEvent(const Event& event) override { handleEvent(event); }

        [[nodiscard]] ModuleInfo getInfo() const override {
            return ModuleInfo{
                .name = "Attendance",
                .version = "1.0.0",
                .description = "ISIC card attendance tracking",
                .enabledByDefault = true,
                .priority = 10  // High priority - core functionality
            };
        }

        // ==================== IHealthCheck Interface ====================

        [[nodiscard]] HealthStatus getHealth() const override;
        [[nodiscard]] std::string_view getComponentName() const noexcept override { return "Attendance"; }
        bool performHealthCheck() override;

        // ==================== Metrics & Status ====================

        [[nodiscard]] AttendanceMetrics getMetrics() const;
        [[nodiscard]] std::size_t getOfflineBufferCount() const;
        void flushOfflineBuffer();
        [[nodiscard]] bool isHighLoad() const;

    private:
        // Event handlers
        void onCardScanned(const CardScannedEvent& event);
        void onCardReadError(const CardReadErrorEvent& event);
        void onMqttConnected();
        void onMqttDisconnected();

        // Processing tasks
        static void processingTaskThunk(void* arg);
        void processingTask();

        // Card processing logic
        bool shouldProcessCard(const CardId& cardId, std::uint64_t timestamp);
        void processCard(const CardScannedEvent& event);
        void enqueueAttendance(const AttendanceRecord& record);
        void publishAttendance(const AttendanceRecord& record);

        // Debounce tracking
        struct RecentCard {
            CardId cardId{};
            std::uint64_t lastSeenMs{0};
            bool valid{false};
        };
        static constexpr std::size_t DEBOUNCE_CACHE_SIZE = 32;
        std::array<RecentCard, DEBOUNCE_CACHE_SIZE> m_recentCards{};
        std::size_t m_recentCardsIndex{0};

        // Offline buffer management
        void bufferForOffline(const AttendanceRecord& record);
        bool hasOfflineRecords() const;
        void tryFlushOfflineBuffer();

        // High load detection
        void checkHighLoad();
        void emitHighLoadEvent();

        // User feedback
        void signalSuccess();
        void signalError();

        // References
        EventBus& m_bus;
        Pn532Driver& m_pn532;
        PowerService& m_powerService;
        UserFeedbackService* m_feedback{nullptr};
        AttendanceBatcher* m_batcher{nullptr};

        // Configuration
        const AppConfig* m_cfg{nullptr};
        const AttendanceConfig* m_attCfg{nullptr};

        // Task management
        TaskHandle_t m_taskHandle{nullptr};
        std::atomic<bool> m_running{false};

        // Event queue for card processing
        QueueHandle_t m_cardQueue{nullptr};
        static constexpr std::size_t CARD_QUEUE_SIZE = 64;

        // Offline buffer (ring buffer)
        std::vector<BufferedAttendance> m_offlineBuffer{};
        std::size_t m_offlineHead{0};
        std::size_t m_offlineTail{0};
        mutable SemaphoreHandle_t m_offlineMutex{nullptr};

        // Metrics
        AttendanceMetrics m_metrics{};
        mutable SemaphoreHandle_t m_metricsMutex{nullptr};

        // State
        std::atomic<bool> m_mqttConnected{false};
        std::atomic<std::uint32_t> m_sequenceNumber{0};
        std::uint64_t m_startTimeMs{0};

        // Wake lock for processing
        WakeLockHandle m_processingWakeLock{};

        // EventBus subscription
        EventBus::ListenerId m_subscriptionId{0};

        static constexpr std::uint8_t MAX_SEND_ATTEMPTS = 3;
    };

}  // namespace isic

#endif  // HARDWARE_ATTENDANCEMODULE_HPP
