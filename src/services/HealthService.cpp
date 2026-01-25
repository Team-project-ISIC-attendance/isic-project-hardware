#include "services/HealthService.hpp"

#include "common/Logger.hpp"
#include "platform/PlatformESP.hpp"
#include "platform/PlatformWiFi.hpp" // TODO: is bad for our architecture to depend on WiFi here, but we need signal strength

#include <ArduinoJson.h>

namespace isic
{
namespace
{
constexpr bool isStateHealthy(const HealthState state) noexcept
{
    return (state == HealthState::Healthy) || (state == HealthState::Unknown); // Treat Unknown as healthy for overall state
}

constexpr auto *kHealthRequestTopic{"health/request"};
constexpr auto *kMetricsRequestTopic{"metrics/request"};
constexpr auto *kHealthPublishTopic{"health"};
constexpr auto *kMetricsPublishTopic{"metrics"};
} // namespace

HealthService::HealthService(EventBus &bus, HealthConfig &config)
    : ServiceBase("HealthService")
    , m_bus(bus)
    , m_config(config)
{
    m_components.reserve(HealthConfig::Constants::kMaxComponentsCount);

    // Subscribers
    m_eventConnections.reserve(3);

    // MQTT connected - subscribe to health/request topic and publish status
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttConnected, [this](const Event &) {
        m_mqttConnected = true;

        m_bus.publish({EventType::MqttSubscribeRequest, MqttEvent{.topic = kHealthRequestTopic, .payload = "", .retain = false}});
        m_bus.publish({EventType::MqttSubscribeRequest, MqttEvent{.topic = kMetricsRequestTopic, .payload = "", .retain = false}});

        if (m_config.publishToMqtt)
        {
            LOG_DEBUG(m_name, "MQTT connected - scheduling initial status update");
            m_pendingHealthPublish = true;
        }
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttDisconnected, [this](const Event &) {
        m_mqttConnected = false;
    }));

    // Handle incoming status requests via MQTT
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttMessage, [this](const Event &e) {
        if (const auto *mqtt = e.get<MqttEvent>())
        {
            if (mqtt->topic.find(kHealthRequestTopic) != std::string::npos)
            {
                LOG_DEBUG(m_name, "Status update requested via MQTT");
                m_pendingHealthPublish = true;
            }
            else if (mqtt->topic.find(kMetricsRequestTopic) != std::string::npos)
            {
                LOG_DEBUG(m_name, "Metrics update requested via MQTT");
                m_pendingMetricsPublish = true;
            }
        }
    }));
}

Status HealthService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing...");

    m_startTimeMs = millis();
    m_lastHealthCheckMs = m_startTimeMs;
    m_lastHealthPublishMs = m_startTimeMs;
    m_lastMetricsPublishMs = m_startTimeMs;

    // Initial state
    m_systemHealth.overallState = HealthState::Healthy;

    setState(ServiceState::Running);
    LOG_INFO(m_name, "Health service started");
    return Status::Ok();
}

void HealthService::loop()
{
    if (m_state != ServiceState::Running)
    {
        return;
    }

    const auto now{millis()};
    auto updatedForInterval{false};

    if ((now - m_lastHealthCheckMs) >= m_config.healthCheckIntervalMs)
    {
        updateSystemHealth();
        m_lastHealthCheckMs = now;
        updatedForInterval = true;
    }

    if (m_pendingHealthPublish)
    {
        if (!updatedForInterval)
        {
            updateSystemHealth();
        }
        publishHealthUpdate();
        m_pendingHealthPublish = false;
    }

    if (m_pendingMetricsPublish)
    {
        publishMetricsUpdate();
        m_pendingMetricsPublish = false;
    }

    if (m_config.publishToMqtt && m_mqttConnected)
    {
        const auto isHealthIntervalElapsed{((now - m_lastHealthPublishMs) >= m_config.statusUpdateIntervalMs)};
        const auto isMetricsIntervalElapsed{((now - m_lastMetricsPublishMs) >= m_config.metricsPublishIntervalMs)};

        if (isHealthIntervalElapsed)
        {
            LOG_DEBUG(m_name, "Periodic health status update");
            publishHealthUpdate();
            m_lastHealthPublishMs = now;
        }

        if (isMetricsIntervalElapsed)
        {
            LOG_DEBUG(m_name, "Periodic metrics update");
            publishMetricsUpdate();
            m_lastMetricsPublishMs = now;
        }
    }
}

void HealthService::end()
{
    setState(ServiceState::Stopping);
    LOG_INFO(m_name, "Shutting down...");

    m_eventConnections.clear();

    setState(ServiceState::Stopped);
    LOG_INFO(m_name, "Stopped");
}

void HealthService::registerComponent(const IService *component)
{
    if (!component)
    {
        return;
    }

    // Check if already registered
    if (std::find(m_components.begin(), m_components.end(), component) != m_components.end())
    {
        LOG_WARN(m_name, "Component %s already registered", component->getName());
        return;
    }

    m_components.push_back(component);
    LOG_DEBUG(m_name, "Registered component, count=%u", m_components.size());
}

void HealthService::unregisterComponent(const IService *component)
{
    if (!component)
    {
        return;
    }

    m_components.erase(std::remove(m_components.begin(), m_components.end(), component), m_components.end());
    LOG_DEBUG(m_name, "Unregistered component, count=%u", m_components.size());
}

void HealthService::updateSystemHealth()
{
    const auto now{millis()};

    const auto freeHeap{ESP.getFreeHeap()};
    const auto heapFrag{platform::getHeapFragmentation()};

    m_systemHealth.cpuFrequencyMs = ESP.getCpuFreqMHz();
    m_systemHealth.freeHeap = freeHeap;
    m_systemHealth.heapFragmentation = heapFrag;
    m_systemHealth.uptimeMs = now - m_startTimeMs;
    m_systemHealth.heapState = (freeHeap < HealthConfig::Constants::kHeapCriticalThresholdBytes)  ? HealthState::Critical
                               : (freeHeap < HealthConfig::Constants::kHeapWarningThresholdBytes) ? HealthState::Warning
                                                                                                  : HealthState::Healthy;
    m_systemHealth.fragmentationState = (heapFrag > HealthConfig::Constants::kFragmentationWarningThresholdPercent) ? HealthState::Warning
                                                                                                                    : HealthState::Healthy;

    // can be situation where WiFi is not connected but we still want to save health state
    if (WiFi.isConnected())
    {
        const auto rssi{WiFi.RSSI()};
        m_systemHealth.wifiRssi = rssi;
        m_systemHealth.wifiState = (rssi < HealthConfig::Constants::kRssiCriticalThresholdDbm)  ? HealthState::Critical
                                   : (rssi < HealthConfig::Constants::kRssiWarningThresholdDbm) ? HealthState::Warning
                                                                                                : HealthState::Healthy;
    }
    else
    {
        m_systemHealth.wifiRssi = 0;
        m_systemHealth.wifiState = HealthState::Unknown;
    }

    // Only log and publish if health state changed from last check
    const bool isCurrentlyUnhealthy = !isStateHealthy(m_systemHealth.heapState) || 
                                       !isStateHealthy(m_systemHealth.fragmentationState) || 
                                       !isStateHealthy(m_systemHealth.wifiState);
    
    static bool wasUnhealthy = false;
    
    if (isCurrentlyUnhealthy && !wasUnhealthy)
    {
        LOG_WARN(m_name, "System health degraded: heap=%s, frag=%s, wifi=%s", 
                 toString(m_systemHealth.heapState), 
                 toString(m_systemHealth.fragmentationState), 
                 toString(m_systemHealth.wifiState));
        m_pendingHealthPublish = true;
    }
    else if (!isCurrentlyUnhealthy && wasUnhealthy)
    {
        LOG_INFO(m_name, "System health recovered");
        m_pendingHealthPublish = true;
    }
    
    wasUnhealthy = isCurrentlyUnhealthy;
}

void HealthService::publishHealthUpdate()
{
    if (!m_config.publishToMqtt || !m_mqttConnected)
    {
        return;
    }

    JsonDocument doc;
    doc["device_id"] = DeviceConfig::kDefaultDeviceId;
    doc["firmware"] = DeviceConfig::Constants::kFirmwareVersion;

    doc["state"] = toString(m_systemHealth.overallState);
    doc["uptime_s"] = m_systemHealth.uptimeMs / 1000;
    doc["cpu_freq"] = m_systemHealth.cpuFrequencyMs;
    doc["free_heap"] = m_systemHealth.freeHeap;
    doc["heap_state"] = toString(m_systemHealth.heapState);
    doc["heap_fragm"] = m_systemHealth.heapFragmentation;
    doc["fragm_state"] = toString(m_systemHealth.fragmentationState);
    doc["wifi_rssi"] = m_systemHealth.wifiRssi;
    doc["wifi_rssi_state"] = toString(m_systemHealth.wifiState);

    std::string json;
    json.reserve(measureJson(doc) + 1);
    serializeJson(doc, json);

    m_bus.publish(Event{EventType::MqttPublishRequest, MqttEvent{
                                                               .topic = kHealthPublishTopic,
                                                               .payload = std::move(json),
                                                               .retain = true}});

    LOG_INFO(m_name, "Published health update");
}

void HealthService::publishMetricsUpdate()
{
    if (!m_config.publishToMqtt || !m_mqttConnected)
    {
        return;
    }

    JsonDocument doc;

    // Let each service serialize its metrics using its name as key
    for (const auto &component: m_components)
    {
        if (component)
        {
            auto serviceObj{doc[component->getName()].to<JsonObject>()};
            component->serializeMetrics(serviceObj);
        }
    }

    std::string json;
    json.reserve(measureJson(doc) + 1);
    serializeJson(doc, json);

    m_bus.publish(Event{EventType::MqttPublishRequest,
                        MqttEvent{
                                .topic = kMetricsPublishTopic,
                                .payload = std::move(json),
                                .retain = true}});

    LOG_INFO(m_name, "Publishing metrics update");
}
} // namespace isic
