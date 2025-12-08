#ifndef HARDWARE_MQTTSERVICE_HPP
#define HARDWARE_MQTTSERVICE_HPP

/**
 * @file MqttService.hpp
 * @brief Production-grade MQTT service with async publishing.
 *
 * Provides non-blocking MQTT publishing with queue-based backpressure,
 * automatic reconnection, and health monitoring integration.
 *
 * @note Thread-safe via FreeRTOS primitives.
 */

#include <WiFi.h>
#include <PubSubClient.h>

#include <string>
#include <atomic>
#include <cstdint>

#include "AppConfig.hpp"
#include "PowerService.hpp"
#include "core/EventBus.hpp"
#include "core/IHealthCheck.hpp"
#include "core/Result.hpp"

// Forward declaration for dependencies
namespace isic {
    class PowerService;
}

namespace isic {
    /**
     * @brief Metrics for MQTT service performance monitoring.
     */
    struct MqttMetrics {
        std::uint32_t messagesPublished{0};
        std::uint32_t messagesFailed{0};
        std::uint32_t messagesDropped{0};
        std::uint32_t messagesReceived{0};

        std::uint32_t connectAttempts{0};
        std::uint32_t connectFailures{0};
        std::uint32_t disconnects{0};

        std::size_t currentQueueSize{0};
        std::size_t peakQueueSize{0};

        std::uint64_t lastConnectedMs{0};
        std::uint64_t lastDisconnectedMs{0};
        std::uint64_t lastPublishMs{0};
        std::uint64_t totalConnectedMs{0};

        bool isConnected{false};
    };

    /**
     * @brief Message to be published via MQTT.
     */
    struct MqttOutboundMessage {
        std::string topic{};
        std::string payload{};
        bool retained{false};
        std::uint8_t qos{0};
        std::uint64_t enqueuedMs{0};
    };

    /**
     * @brief Production-grade MQTT service with async publishing and health monitoring.
     *
     * Responsibilities:
     * - Connect to MQTT broker with automatic reconnection
     * - Non-blocking message publishing via outbound queue
     * - Handle inbound messages and dispatch events
     * - Integrate with PowerService for wake locks during publishing
     * - Provide health status
     * - Graceful handling of network issues
     */
    class MqttService : public IEventListener, public IHealthCheck {
    public:
        explicit MqttService(EventBus& bus);
        ~MqttService() override;

        // Non-copyable, non-movable
        MqttService(const MqttService&) = delete;
        MqttService& operator=(const MqttService&) = delete;
        MqttService(MqttService&&) = delete;
        MqttService& operator=(MqttService&&) = delete;

        /**
         * @brief Initialize the MQTT service.
         */
        [[nodiscard]] Status begin(const AppConfig& cfg, PowerService& powerService);

        /**
         * @brief Stop the MQTT service.
         */
        void stop();

        // ==================== Publishing ====================

        /**
         * @brief Publish a message asynchronously (non-blocking).
         */
        [[nodiscard]] bool publishAsync(const std::string& topic, const std::string& payload, bool retained = false, std::uint8_t qos = 0);

        /**
         * @brief Publish a message synchronously (blocks until sent).
         */
        [[nodiscard]] bool publishSync(const std::string& topic, const std::string& payload);

        // ==================== Status & Health ====================

        [[nodiscard]] bool isConnected() const noexcept {
            return m_connected.load();
        }
        [[nodiscard]] MqttMetrics getMetrics() const;
        [[nodiscard]] std::size_t getQueueSize() const;

        // ==================== IHealthCheck Interface ====================

        [[nodiscard]] HealthStatus getHealth() const override;
        [[nodiscard]] std::string_view getComponentName() const noexcept override {
            return "MQTT";
        }
        bool performHealthCheck() override;

        // ==================== IEventListener Interface ====================

        void onEvent(const Event& event) override;

    private:
        // Task functions
        static void mqttTaskThunk(void* arg);
        void mqttTask();

        // Connection management
        void ensureWifiConnected();
        void connectMqtt();
        void handleDisconnect();
        std::uint32_t calculateBackoff() const;

        // Message handling
        void onMqttMessage(char* topic, uint8_t* payload, unsigned int length);
        void processOutboundQueue();
        bool sendMessage(const MqttOutboundMessage& msg);

        // Topic helpers
        [[nodiscard]] std::string makeBaseTopic() const;
        [[nodiscard]] std::string topicConfigSet() const;
        [[nodiscard]] std::string topicConfig() const;
        [[nodiscard]] std::string topicOtaSet() const;
        [[nodiscard]] std::string topicOtaStatus() const;
        [[nodiscard]] std::string topicOtaProgress() const;
        [[nodiscard]] std::string topicOtaError() const;
        [[nodiscard]] std::string topicAttendance() const;
        [[nodiscard]] std::string topicAttendanceBatch() const;
        [[nodiscard]] std::string topicStatus() const;
        [[nodiscard]] std::string topicHealth() const;
        [[nodiscard]] std::string topicHealthReport() const;
        [[nodiscard]] std::string topicMetrics() const;
        [[nodiscard]] std::string topicModules() const;

        // Publishing helpers
        void handleStatus(std::string_view status);
        void handleHealth(const HealthStatusChangedEvent& event);
        void handleHealthReport(const std::string& report);
        void handleAttendanceEvent(const AttendanceRecord& record);
        void handleOtaStateChanged(const OtaStateChangedEvent& event);
        void handleOtaProgress(const OtaProgressEvent& event);

        // References
        EventBus& m_bus;
        PowerService* m_powerService{nullptr};

        // Configuration
        const AppConfig* m_cfg{nullptr};
        const MqttConfig* m_mqttCfg{nullptr};

        // Network
        WiFiClient m_wifiClient{};
        PubSubClient m_client{};

        // Task management
        TaskHandle_t m_taskHandle{nullptr};
        std::atomic<bool> m_running{false};

        // Outbound message queue
        QueueHandle_t m_outboundQueue{nullptr};

        // State
        std::atomic<bool> m_connected{false};
        std::atomic<bool> m_wifiConnected{false};
        std::uint64_t m_lastReconnectAttempt{0};
        std::uint64_t m_connectionStartMs{0};
        std::uint32_t m_consecutiveFailures{0};

        // Metrics
        MqttMetrics m_metrics{};
        mutable SemaphoreHandle_t m_metricsMutex{nullptr};

        // Wake lock
        WakeLockHandle m_wakeLock{};

        // EventBus subscription
        EventBus::ListenerId m_subscriptionId{0};
    };
}

#endif  // HARDWARE_MQTTSERVICE_HPP
