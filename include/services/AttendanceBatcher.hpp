#ifndef HARDWARE_ATTENDANCEBATCHER_HPP
#define HARDWARE_ATTENDANCEBATCHER_HPP

/**
 * @file AttendanceBatcher.hpp
 * @brief Smart attendance event batcher for efficient MQTT publishing.
 *
 * Collects attendance records and batches them for reduced MQTT overhead.
 * Supports offline buffering and graceful degradation.
 */

#include <Arduino.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "AppConfig.hpp"
#include "services/PowerService.hpp"
#include "core/EventBus.hpp"
#include "core/Result.hpp"

namespace isic {

    // Forward declarations
    class MqttService;
    class PowerService;

    /**
     * @brief Metrics for attendance batching.
     */
    struct BatcherMetrics {
        std::uint32_t recordsReceived{0};
        std::uint32_t recordsBatched{0};
        std::uint32_t batchesSent{0};
        std::uint32_t batchesFailed{0};
        std::uint32_t recordsDropped{0};

        std::size_t currentBatchSize{0};
        std::size_t peakBatchSize{0};
        std::size_t pendingBufferCount{0};

        std::uint64_t lastBatchSentMs{0};
        std::uint64_t lastRecordReceivedMs{0};
    };

    /**
     * @brief Smart attendance event batcher for MQTT.
     *
     * Collects attendance records and batches them for efficient MQTT publishing.
     *
     * Features:
     * - Configurable batch size and flush interval
     * - Automatic flush on idle (no new events for X seconds)
     * - Flush before sleep
     * - Buffer pending batches during MQTT disconnection
     * - Ordering guarantees (events sent in order of occurrence)
     * - Graceful handling of buffer overflow
     *
     * Design:
     * - Non-blocking: never blocks the PN532 scanning task
     * - Independent: operates via EventBus, no direct coupling
     * - Efficient: minimizes MQTT messages while maintaining responsiveness
     */
    class AttendanceBatcher : public IEventListener {
    public:
        explicit AttendanceBatcher(EventBus& bus);
        ~AttendanceBatcher() override;

        // Non-copyable, non-movable
        AttendanceBatcher(const AttendanceBatcher&) = delete;
        AttendanceBatcher& operator=(const AttendanceBatcher&) = delete;
        AttendanceBatcher(AttendanceBatcher&&) = delete;
        AttendanceBatcher& operator=(AttendanceBatcher&&) = delete;

        /**
         * @brief Initialize the batcher.
         */
        [[nodiscard]] Status begin(const AppConfig& cfg, MqttService& mqtt, PowerService& power);

        /**
         * @brief Stop the batcher.
         */
        void stop();

        // ==================== Batching Control ====================

        /**
         * @brief Add a record to the current batch.
         * @return true if record was added, false if dropped
         */
        [[nodiscard]] bool addRecord(const AttendanceRecord& record);

        /**
         * @brief Force flush the current batch.
         */
        void flush();

        /**
         * @brief Flush and prepare for sleep.
         */
        void flushForSleep();

        // ==================== State Queries ====================

        /**
         * @brief Get current batch size.
         */
        [[nodiscard]] std::size_t getCurrentBatchSize() const;

        /**
         * @brief Get pending buffer count (batches waiting for MQTT).
         */
        [[nodiscard]] std::size_t getPendingBufferCount() const;

        /**
         * @brief Get metrics.
         */
        [[nodiscard]] BatcherMetrics getMetrics() const;

        /**
         * @brief Check if batcher is enabled.
         */
        [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(); }

        // ==================== Configuration ====================

        /**
         * @brief Update configuration.
         */
        void updateConfig(const AttendanceConfig& cfg);

        /**
         * @brief Enable/disable batching.
         */
        void setEnabled(bool enabled);

        // ==================== IEventListener ====================

        void onEvent(const Event& event) override;

    private:
        // Task for batch management
        static void batcherTaskThunk(void* arg);
        void batcherTask();

        // Batch operations
        void processIncomingRecord(const AttendanceRecord& record);
        bool shouldFlushBatch() const;
        void flushCurrentBatch();
        bool sendBatch(const AttendanceBatch& batch);
        void bufferBatch(const AttendanceBatch& batch);
        void tryFlushPendingBuffers();

        // JSON serialization
        std::string serializeBatch(const AttendanceBatch& batch) const;

        EventBus& m_bus;
        MqttService* m_mqtt{nullptr};
        PowerService* m_power{nullptr};
        const AttendanceConfig* m_cfg{nullptr};
        const AppConfig* m_appCfg{nullptr};

        // Task management
        TaskHandle_t m_taskHandle{nullptr};
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_enabled{true};

        // Current batch
        mutable SemaphoreHandle_t m_batchMutex{nullptr};
        AttendanceBatch m_currentBatch{};
        std::uint64_t m_batchStartMs{0};
        std::uint64_t m_lastRecordMs{0};

        // Pending buffer (for when MQTT is disconnected)
        static constexpr std::size_t MAX_PENDING_BATCHES = 10;
        std::array<AttendanceBatch, MAX_PENDING_BATCHES> m_pendingBuffer{};
        std::size_t m_pendingHead{0};
        std::size_t m_pendingTail{0};
        std::size_t m_pendingCount{0};
        mutable SemaphoreHandle_t m_pendingMutex{nullptr};

        // Incoming record queue
        QueueHandle_t m_recordQueue{nullptr};
        static constexpr std::size_t RECORD_QUEUE_SIZE = 32;

        // Metrics
        BatcherMetrics m_metrics{};
        mutable SemaphoreHandle_t m_metricsMutex{nullptr};

        // MQTT connection state
        std::atomic<bool> m_mqttConnected{false};

        // Wake lock for flushing
        WakeLockHandle m_wakeLock{};

        // EventBus subscription
        EventBus::ListenerId m_subscriptionId{0};
    };
}

#endif  // HARDWARE_ATTENDANCEBATCHER_HPP

