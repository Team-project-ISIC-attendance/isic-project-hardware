#ifndef ISIC_SERVICES_MQTTSERVICE_HPP
#define ISIC_SERVICES_MQTTSERVICE_HPP

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"
#include "platform/PlatformWiFi.hpp"

#include <PubSubClient.h>
#include <vector>

namespace isic
{
class MqttService : public ServiceBase
{
public:
    MqttService(EventBus &bus, const MqttConfig &config, const DeviceConfig& deviceConfig);
    ~MqttService() override;

    MqttService(const MqttService &) = delete;
    MqttService &operator=(const MqttService &) = delete;
    MqttService(MqttService &&) = delete;
    MqttService &operator=(MqttService &&) = delete;

    // IService implementation
    Status begin() override;
    void loop() override;
    void end() override;

    [[nodiscard]] const MqttMetrics &getMqttMetrics() const noexcept
    {
        return m_metrics;
    }
    [[nodiscard]] MqttState getMqttState() const noexcept
    {
        return m_mqttState;
    }
    [[nodiscard]] bool isConnected() const noexcept
    {
        return m_mqttState == MqttState::Connected;
    }
    [[nodiscard]] const std::string &getTopicPrefix() const noexcept
    {
        return m_topicPrefix;
    }

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
        obj["published"] = m_metrics.messagesPublished;
        obj["failed"] = m_metrics.messagesFailed;
        obj["received"] = m_metrics.messagesReceived;
        obj["reconnects"] = m_metrics.reconnectCount;
    }

    bool publish(const char *topicSuffix, const char *payload, bool retained = false);
    bool publish(const std::string &topicSuffix, const std::string &payload, bool retained = false);

    bool subscribe(const char *topicSuffix);
    bool unsubscribe(const char *topicSuffix);

    [[nodiscard]] std::string buildTopic(const char *suffix) const;

    void disconnect();
    void reconnect();

private:
    const char *buildTopicBuffer(const char *suffix);
    void connect();
    void handleMessage(const char *topic, std::uint8_t *payload, unsigned int length);
    void rebuildTopicPrefix();

    [[nodiscard]] std::uint32_t calculateBackoff() const noexcept;

    static void messageCallback(const char *topic, std::uint8_t *payload, unsigned int length);

    EventBus &m_bus;
    const MqttConfig &m_config;
    const DeviceConfig& m_deviceConfig;

    WiFiClient m_networkClient;
    PubSubClient m_mqttClient;

    std::string m_topicPrefix{};
    std::string m_topicBuffer{};

    MqttState m_mqttState{MqttState::Disconnected};
    MqttMetrics m_metrics{};
    bool m_wifiReady{false};

    std::uint32_t m_lastConnectAttemptMs{0};
    std::uint32_t m_consecutiveFailures{0};

    std::vector<EventBus::Subscription> m_eventConnections{};

    static inline MqttService *s_instance{nullptr};
};
} // namespace isic

#endif // ISIC_SERVICES_MQTTSERVICE_HPP
