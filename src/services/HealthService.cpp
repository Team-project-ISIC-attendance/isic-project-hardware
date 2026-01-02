#include "services/HealthService.hpp"

#include "common/Logger.hpp"
#include "core/IService.hpp"
#include "platform/PlatformESP.hpp"
#include "services/ConfigService.hpp"
#include "services/MqttService.hpp"

#include <ArduinoJson.h>

#include "services/OtaService.hpp"

namespace isic
{
namespace
{
[[nodiscard]] std::size_t findComponentSlot(const std::array<ServiceHealth, HealthConfig::Constants::kMaxComponentsCount> &health, const char *name) noexcept
{
    if (!name)
    {
        return HealthConfig::Constants::kMaxComponentsCount;
    }

    for (std::size_t i = 0; i < HealthConfig::Constants::kMaxComponentsCount; ++i)
    {
        if (health[i].name && strcmp(health[i].name, name) == 0)
        {
            return i;
        }
    }
    return HealthConfig::Constants::kMaxComponentsCount;
}

[[nodiscard]] std::size_t findEmptySlot(const std::array<ServiceHealth, HealthConfig::Constants::kMaxComponentsCount> &health) noexcept
{
    for (std::size_t i = 0; i < HealthConfig::Constants::kMaxComponentsCount; ++i)
    {
        if (health[i].name == nullptr)
        {
            return i;
        }
    }
    return HealthConfig::Constants::kMaxComponentsCount;
}
} // namespace

HealthService::HealthService(EventBus &bus, HealthConfig &config)
    : ServiceBase("HealthService")
    , m_bus(bus)
    , m_config(config)
{
    // Subscribers
    m_eventConnections.reserve(2);

    // MQTT connected - subscribe to health/request topic and publish status
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttConnected, [this](const Event &) {
        // Subscribe to health control topics
        m_bus.publish(Event{
            EventType::MqttSubscribeRequest,
            MqttEvent{.topic = "health/request", .payload = "", .retain = false}
        });

        // Immediately publish status when MQTT comes online
        if (m_config.publishToMqtt)
        {
            publishStatusUpdate();
            publishOnlineStatus();
        }
    }));

    // Handle incoming status requests via MQTT
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttMessage, [this](const Event &e) {
        if (const auto *mqtt = e.get<MqttEvent>())
        {
            // Check if it's a status request
            if (mqtt->topic.find("health/request") != std::string::npos)
            {
                LOG_DEBUG(m_name, "Status update requested via MQTT");
                publishStatusUpdate();
            }
        }
    }));
}

Status HealthService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing...");

    m_startTimeMs = millis();
    m_lastCheckMs = m_startTimeMs;
    m_lastStatusUpdateMs = m_startTimeMs;

    // Initial state
    m_systemHealth.overallState = HealthState::Healthy;
    m_systemHealth.lastUpdateMs = m_startTimeMs;

    // Initial system health check
    updateSystemHealth();

    // Service is operational after initialization
    setState(ServiceState::Running);
    LOG_INFO(m_name, "Ready (health=%s, check=5min, status=1min), heap=%u",
             toString(m_systemHealth.overallState), ESP.getFreeHeap());
    return Status::Ok();
}

void HealthService::loop()
{
    // Only loop if service is Running
    if (m_state != ServiceState::Running)
        return;

    const auto now{millis()};

    // Periodic system health check (every 5 minutes)
    // Checks heap, fragmentation, WiFi signal, component health
    if (now - m_lastCheckMs >= m_config.healthCheckIntervalMs)
    {
        checkComponents();
        updateSystemHealth();
        m_lastCheckMs = now;
    }

    // Periodic status publishing (every 1 minute) OR when state changes
    const bool intervalElapsed = (now - m_lastStatusUpdateMs >= m_config.statusUpdateIntervalMs);
    const bool stateChanged = (m_systemHealth.overallState != m_lastPublishedState);

    if (m_config.publishToMqtt && (intervalElapsed || stateChanged))
    {
        publishStatusUpdate();
        m_lastStatusUpdateMs = now;
        m_lastPublishedState = m_systemHealth.overallState;

        if (stateChanged)
        {
            LOG_INFO(m_name, "State changed to %s - publishing immediate update",
                     toString(m_systemHealth.overallState));
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

void HealthService::registerComponent(IHealthReporter *component)
{
    if (!component)
    {
        return;
    }

    // Check if already registered
    for (std::size_t i = 0; i < m_componentCount; ++i)
    {
        if (m_components[i] == component)
        {
            return; // Already registered
        }
    }

    // Add if space available
    if (m_componentCount < HealthConfig::Constants::kMaxComponentsCount)
    {
        m_components[m_componentCount++] = component;
        LOG_DEBUG(m_name, "Registered component, count=%u", m_componentCount);
    }
    else
    {
        LOG_WARN(m_name, "Max components reached");
    }
}

void HealthService::unregisterComponent(IHealthReporter *component)
{
    if (!component)
    {
        return;
    }

    for (std::size_t i = 0; i < m_componentCount; ++i)
    {
        if (m_components[i] == component)
        {
            // Move last element to fill gap (order doesn't matter)
            m_componentCount--;
            if (i < m_componentCount)
            {
                m_components[i] = m_components[m_componentCount];
            }
            m_components[m_componentCount] = nullptr;
            LOG_DEBUG(TAG, "Unregistered component, count=%u", m_componentCount);
            return;
        }
    }
}

ServiceHealth HealthService::getComponentHealth(const char* name) const noexcept
{
    const auto idx = findComponentSlot(m_componentHealth, name);
    if (idx < HealthConfig::Constants::kMaxComponentsCount)
    {
        return m_componentHealth[idx];
    }
    return ServiceHealth{};
}

void HealthService::checkNow()
{
    checkComponents();
    m_lastCheckMs = millis();
}

void HealthService::publishNow()
{
    publishStatusUpdate();
    m_lastStatusUpdateMs = millis();
}

void HealthService::checkComponents()
{
    // Check all registered IHealthReporter components (includes all services)
    // All services inherit from ServiceBase which implements IHealthReporter::getHealth()
    // This automatically reports their ServiceState (Running/Ready/Error) as HealthState
    for (std::size_t i = 0; i < m_componentCount; ++i)
    {
        auto* component{m_components[i]};
        if (!component)
        {
            continue;
        }

        // Get health from component - ServiceBase maps ServiceState to HealthState:
        // Running -> Healthy, Ready -> Warning, Error -> Unhealthy, others -> Degraded
        const bool healthy{component->checkHealth()};
        const auto status{component->getHealth()};

        // Update our tracking with service-provided status
        const char* message = status.message ? status.message : toString(status.state);
        setComponentHealth(status.name, status.state, message);

        (void)healthy;  // Status already captured in getHealth()
    }

    // Update system-level health metrics (memory, fragmentation, WiFi signal)
    updateSystemHealth();
}

void HealthService::updateSystemHealth()
{
    const auto now{millis()};

    // Get heap info
    const auto freeHeap{ESP.getFreeHeap()};
    const auto heapFrag{platform::getHeapFragmentation()};

    m_systemHealth.freeHeap = freeHeap;
    m_systemHealth.heapFragmentation = heapFrag;
    m_systemHealth.uptimeMs = now - m_startTimeMs;
    m_systemHealth.lastUpdateMs = now;

    // Check heap health
    if (freeHeap < HealthConfig::Constants::kHeapCriticalThresholdBytes)
    {
        setComponentHealth("memory", HealthState::Critical, "Very low heap");
    }
    else if (freeHeap < HealthConfig::Constants::kHeapWarningThresholdBytes)
    {
        setComponentHealth("memory", HealthState::Warning, "Low heap");
    }
    else
    {
        setComponentHealth("memory", HealthState::Healthy, "OK");
    }

    // Check fragmentation
    if (heapFrag > HealthConfig::Constants::kFragmentationWarningThresholdPercent)
    {
        setComponentHealth("heap_frag", HealthState::Warning, "High fragmentation");
    }
    else
    {
        setComponentHealth("heap_frag", HealthState::Healthy, "OK");
    }

    // Check WiFi signal strength
    if (WiFi.isConnected())
    {
        const auto rssi{WiFi.RSSI()};
        m_systemHealth.wifiRssi = rssi;

        if (rssi < HealthConfig::Constants::kRssiWarningThresholdDbm)
        {
            setComponentHealth("wifi_signal", HealthState::Warning, "Weak signal");
        }
        else
        {
            setComponentHealth("wifi_signal", HealthState::Healthy, "Good signal");
        }
    }
    else
    {
        m_systemHealth.wifiRssi = 0;
    }
}

void HealthService::setComponentHealth(const char* component, HealthState state, const char* message)
{
    if (!component)
    {
        return;
    }

    const auto now{millis()};

    // Find existing slot
    auto idx{findComponentSlot(m_componentHealth, component)};

    // If not found, find empty slot
    if (idx >= HealthConfig::Constants::kMaxComponentsCount)
    {
        idx = findEmptySlot(m_componentHealth);
        if (idx >= HealthConfig::Constants::kMaxComponentsCount)
        {
            LOG_WARN(m_name, "No slot for component: %s", component);
            return;
        }
    }

    // Update slot
    auto& health = m_componentHealth[idx];
    health.name = component;  // Pointer to static string
    health.state = state;
    health.message = message; // Pointer to static string
    health.lastUpdateMs = now;

    // Re-aggregate overall health
    aggregateHealth();
}

void HealthService::aggregateHealth()
{
    auto worstState{HealthState::Healthy};
    const auto* worstMessage{"All systems operational"};

    // Find worst state across all components
    for (const auto& health : m_componentHealth)
    {
        if (!health.name)
        {
            continue;
        }

        if (static_cast<std::uint8_t>(health.state) > static_cast<std::uint8_t>(worstState))
        {
            worstState = health.state;
            worstMessage = health.message ? health.message : "Unknown issue";
        }
    }

    // Check if state changed
    const auto oldState{m_systemHealth.overallState};
    m_systemHealth.overallState = worstState;
    m_systemHealth.lastUpdateMs = millis();

    // Publish event if state changed
    if (oldState != worstState)
    {
        LOG_INFO(m_name, "State changed: %s -> %s (%s)", toString(oldState), toString(worstState), worstMessage);

        m_bus.publish(Event{
            EventType::HealthChanged,
            HealthEvent{
                .state = worstState,
                // .component = nullptr,
                .message = worstMessage
            }
        });
    }
}
void HealthService::publishOnlineStatus()
{
    // Publish comprehensive online status (replaces old MqttService::publishStatus)
    JsonDocument doc;
    doc["status"] = "online";
    doc["device_id"] = DeviceConfig::kDefaultDeviceId;
    doc["firmware"] = DeviceConfig::Constants::kFirmwareVersion;
    doc["ip"] = WiFi.localIP().toString().c_str();
    doc["uptime_s"] = getUptimeSec();
    doc["free_heap"] = ESP.getFreeHeap();

    std::string json;
    serializeJson(doc, json);

    m_bus.publish(Event{
        EventType::MqttPublishRequest,
        MqttEvent{
            .topic = "status",
            .payload = std::move(json),
            .retain = true  // Retained so new subscribers see it
        }
    });

    LOG_INFO(m_name, "Published online status");
}

void HealthService::publishStatusUpdate()
{
    // Log to serial if enabled
    if (m_config.publishToLog)
    {
        LOG_INFO(m_name, "Status: state=%s, heap=%u/%u%%, rssi=%d, uptime=%us",
                 toString(m_systemHealth.overallState),
                 m_systemHealth.freeHeap,
                 m_systemHealth.heapFragmentation,
                 m_systemHealth.wifiRssi,
                 getUptimeSec());
    }

    // Skip MQTT if not enabled
    if (!m_config.publishToMqtt)
    {
        return;
    }

    // Publish consolidated status with all service states
    JsonDocument doc;

    // System metrics
    doc["uptime_s"] = getUptimeSec();
    doc["free_heap"] = m_systemHealth.freeHeap;
    doc["heap_frag"] = m_systemHealth.heapFragmentation;
    doc["cpu_freq"] = ESP.getCpuFreqMHz();
    doc["rssi"] = m_systemHealth.wifiRssi;
    doc["fw"] = DeviceConfig::Constants::kFirmwareVersion;
    doc["state"] = toString(m_systemHealth.overallState);

    // All service/component states in one object
    auto services = doc["services"].to<JsonObject>();
    for (const auto& health : m_componentHealth)
    {
        if (!health.name)
        {
            continue;
        }

        auto component = services[health.name].to<JsonObject>();
        component["state"] = toString(health.state);
        if (health.message)
        {
            component["msg"] = health.message;
        }
        component["errors"] = health.errorCount;
    }

    std::string json;
    serializeJson(doc, json);

    m_bus.publish(Event{
        EventType::MqttPublishRequest,
        MqttEvent{
            .topic = "health",
            .payload = std::move(json),
            .retain = false
        }
    });
}
} // namespace isic
