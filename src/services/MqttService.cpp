/**
 * @file MqttService.cpp
 * @brief MQTT communication service implementation
 */

#include "services/MqttService.hpp"
#include "common/Logger.hpp"

#include <ArduinoJson.h>

namespace isic
{
// Static instance for callback, for me antipattern, but PubSubClient requires a static function
// TODO: we can improve this later with a better design, like using a singleton or passing context or std::function
MqttService *MqttService::s_instance = nullptr;

MqttService::MqttService(EventBus &bus, const MqttConfig &config, const DeviceConfig &deviceConfig)
    : ServiceBase("MqttService")
    , m_bus(bus)
    , m_config(config)
    , m_deviceConfig(deviceConfig)
{
    s_instance = this;
    m_mqttClient.setClient(m_networkClient); // Bind transport client once during construction

    m_eventConnections.reserve(4);
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::WifiConnected, [this](const Event &e) {
        LOG_DEBUG(m_name, "WiFi connected, attempting MQTT connection");
        m_wifiReady = true;
        if (m_config.isConfigured())
        {
            connect();
        }
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::WifiDisconnected, [this](const Event &e) {
        LOG_DEBUG(m_name, "WiFi disconnected");
        m_wifiReady = false;

        if (m_mqttState == MqttState::Connected)
        {
            m_mqttState = MqttState::Disconnected;
            m_metrics.connected = false;
            setState(ServiceState::Ready);
            m_bus.publish(EventType::MqttDisconnected);
        }
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttPublishRequest, [this](const Event &e) {
        if (const auto *mqtt = e.get<MqttEvent>())
        {
            publish(mqtt->topic, mqtt->payload, mqtt->retain);
        }
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttSubscribeRequest, [this](const Event &e) {
        if (const auto *mqtt = e.get<MqttEvent>())
        {
            subscribe(mqtt->topic.c_str());
        }
    }));
}

MqttService::~MqttService()
{
    s_instance = nullptr;
}

Status MqttService::begin()
{
    LOG_INFO(m_name, "Initializing...");
    setState(ServiceState::Initializing);

    rebuildTopicPrefix();

    setState(ServiceState::Ready);
    LOG_INFO(m_name, "MQTT service ready (waiting for WiFi connection)");
    return Status::Ok();
}

void MqttService::loop()
{
    // Only loop if service is Ready or Running
    if (m_state != ServiceState::Ready && m_state != ServiceState::Running)
    {
        return;
    }

    if (!m_wifiReady)
    {
        return;
    }

    if (!m_mqttClient.connected())
    {
        if (m_mqttState == MqttState::Connected)
        {
            m_mqttState = MqttState::Disconnected;
            m_metrics.connected = false;

            // Service no longer fully operational - back to Ready state
            setState(ServiceState::Ready);

            m_bus.publish(EventType::MqttDisconnected);
            LOG_WARN(m_name, "MQTT disconnected - service now Ready (will reconnect)");
        }

        // Reconnect with exponential backoff
        std::uint32_t backoff = calculateBackoff();
        if (millis() - m_lastConnectAttemptMs >= backoff)
        {
            connect();
        }
    }
    else
    {
        m_mqttClient.loop();

        if (m_mqttState != MqttState::Connected)
        {
            m_mqttState = MqttState::Connected;
            m_metrics.connected = true;
        }
    }
}

void MqttService::end()
{
    setState(ServiceState::Stopping);
    LOG_INFO(m_name, "Shutting down...");

    if (m_mqttClient.connected())
    {
        // Pure transport - publish MqttDisconnected event
        // Services can handle their own cleanup/offline messages
        m_bus.publish(EventType::MqttDisconnected);
        m_mqttClient.disconnect();
    }

    m_mqttState = MqttState::Disconnected;
    m_wifiReady = false;

    setState(ServiceState::Stopped);
    LOG_INFO(m_name, "Stopped");
}

bool MqttService::publish(const char *topicSuffix, const char *payload, bool retained)
{
    if (!m_mqttClient.connected())
    {
        ++m_metrics.messagesFailed;
        return false;
    }

    std::string topic = buildTopic(topicSuffix);
    bool success = m_mqttClient.publish(topic.c_str(), payload, retained);

    if (success)
    {
        ++m_metrics.messagesPublished;
        m_metrics.lastOperationMs = millis();
    }
    else
    {
        ++m_metrics.messagesFailed;
    }

    return success;
}

bool MqttService::publish(const std::string &topicSuffix, const std::string &payload, bool retained)
{
    return publish(topicSuffix.c_str(), payload.c_str(), retained);
}

bool MqttService::subscribe(const char *topicSuffix)
{
    if (!m_mqttClient.connected())
        return false;

    std::string topic = buildTopic(topicSuffix);
    return m_mqttClient.subscribe(topic.c_str());
}

bool MqttService::unsubscribe(const char *topicSuffix)
{
    if (!m_mqttClient.connected())
        return false;

    std::string topic = buildTopic(topicSuffix);
    return m_mqttClient.unsubscribe(topic.c_str());
}

std::string MqttService::buildTopic(const char *suffix) const
{
    // Use cached prefix for better performance
    std::string topic;
    // Pre-reserve space to avoid reallocation: prefix + suffix + null terminator
    topic.reserve(m_topicPrefix.length() + std::strlen(suffix) + 1);
    topic = m_topicPrefix;
    topic += suffix;
    return topic;
}

void MqttService::rebuildTopicPrefix()
{
    // Pre-reserve space for typical topic prefix to avoid reallocations
    // Format: "baseTopic/deviceId/" (e.g., "home/attendance/device123/" = ~30 chars)
    const auto estimatedSize{m_config.baseTopic.length() + m_deviceConfig.deviceId.length() + 2};
    m_topicPrefix.reserve(estimatedSize);

    m_topicPrefix = m_config.baseTopic;
    if (!m_topicPrefix.empty() && m_topicPrefix.back() != '/')
    {
        m_topicPrefix += "/";
    }
    if (!m_deviceConfig.deviceId.empty())
    {
        m_topicPrefix += m_deviceConfig.deviceId;
        m_topicPrefix += "/";
    }
}

void MqttService::connect()
{
    if (!m_wifiReady)
    {
        return;
    }

    m_lastConnectAttemptMs = millis();
    m_mqttState = MqttState::Connecting;

    if (!m_config.isConfigured())
    {
        LOG_WARN(m_name, "MQTT not configured, cannot connect");
        m_mqttState = MqttState::Error;
        incrementErrors();
        return;
    }

    m_mqttClient.setServer(m_config.brokerAddress.c_str(), m_config.port);
    m_mqttClient.setKeepAlive(m_config.keepAliveIntervalSec);
    m_mqttClient.setCallback(messageCallback);
    m_mqttClient.setBufferSize(MqttConfig::Constants::kMaxPayloadSizeBytes);

    LOG_INFO(m_name, "Connecting to MQTT %s:%d...", m_config.brokerAddress.c_str(), m_config.port);

    bool connected = false;
    if (!m_config.username.empty())
    {
        connected = m_mqttClient.connect(m_deviceConfig.deviceId.c_str(), m_config.username.c_str(), m_config.password.c_str());
    }
    else
    {
        connected = m_mqttClient.connect(m_deviceConfig.deviceId.c_str());
    }

    if (connected)
    {
        m_consecutiveFailures = 0;
        m_mqttState = MqttState::Connected;
        m_metrics.connected = true;
        m_metrics.reconnectCount++;

        LOG_INFO(m_name, "MQTT connected - service now Running");
        setState(ServiceState::Running);
        m_bus.publish(EventType::MqttConnected);
    }
    else
    {
        ++m_consecutiveFailures;
        m_mqttState = MqttState::Error;
        LOG_ERROR(m_name, "MQTT connect failed, state=%d", m_mqttClient.state());
    }
}

void MqttService::handleMessage(const char *topic, std::uint8_t *payload, const unsigned int length)
{
    ++m_metrics.messagesReceived;

    LOG_DEBUG(m_name, "MQTT message: %s", topic);

    // Create event with MQTT data
    MqttEvent mqttData{
            .topic = topic,
            .payload = std::string(reinterpret_cast<char *>(payload), length),
            .retain = false};
    m_bus.publish({EventType::MqttMessage, std::move(mqttData)});
}

std::uint32_t MqttService::calculateBackoff() const noexcept
{
    if (m_consecutiveFailures == 0)
    {
        return 0;
    }

    // Exponential backoff with jitter
    auto backoff = m_config.reconnectMinIntervalMs * (1 << min(m_consecutiveFailures, 5U));
    if (backoff > m_config.reconnectMaxIntervalMs)
    {
        backoff = m_config.reconnectMaxIntervalMs;
    }

    // Add 10% jitter
    backoff += random(0, backoff / 10);
    return backoff;
}

void MqttService::disconnect()
{
    if (m_mqttClient.connected())
    {
        m_mqttClient.disconnect();
    }
    m_mqttState = MqttState::Disconnected;
    m_metrics.connected = false;
}

void MqttService::reconnect()
{
    m_consecutiveFailures = 0;
    m_lastConnectAttemptMs = 0;
    connect();
}

void MqttService::messageCallback(const char *topic, std::uint8_t *payload, const unsigned int length)
{
    if (s_instance)
    {
        s_instance->handleMessage(topic, payload, length);
    }
}
} // namespace isic
