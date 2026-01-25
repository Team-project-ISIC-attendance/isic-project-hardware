#ifndef ISIC_SERVICES_HEALTHSERVICE_HPP
#define ISIC_SERVICES_HEALTHSERVICE_HPP

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <Arduino.h>
#include <array>
#include <vector>

namespace isic
{
class HealthService : public ServiceBase
{
public:
    HealthService(EventBus &bus, HealthConfig &config);
    ~HealthService() override = default;

    HealthService(const HealthService &) = delete;
    HealthService &operator=(const HealthService &) = delete;
    HealthService(HealthService &&) = delete;
    HealthService &operator=(HealthService &&) = delete;

    // IService implementation
    Status begin() override;
    void loop() override;
    void end() override;

    // Component registration (for health reporting)
    void registerComponent(const IService *component);
    void unregisterComponent(const IService *component);

    [[nodiscard]] const SystemHealth &getSystemHealth() const noexcept
    {
        return m_systemHealth;
    }

    [[nodiscard]] bool isHealthy() const noexcept
    {
        return m_systemHealth.overallState == HealthState::Healthy;
    }

    [[nodiscard]] std::uint32_t getUptimeMs() const noexcept
    {
        return millis() - m_startTimeMs;
    }

private:
    void publishHealthUpdate();
    void publishMetricsUpdate();

    void updateSystemHealth();

    // Dependencies
    EventBus &m_bus;
    HealthConfig &m_config;

    // Registered health reporters
    std::vector<const IService *> m_components{};

    // System health summary
    SystemHealth m_systemHealth{};

    // Timing
    std::uint32_t m_startTimeMs{0};
    std::uint32_t m_lastHealthCheckMs{0};
    std::uint32_t m_lastHealthPublishMs{0};
    std::uint32_t m_lastMetricsPublishMs{0};
    bool m_mqttConnected{false};
    bool m_pendingHealthPublish{false};
    bool m_pendingMetricsPublish{false};

    // Event subscriptions
    std::vector<EventBus::Subscription> m_eventConnections{};
};
} // namespace isic

#endif // ISIC_SERVICES_HEALTHSERVICE_HPP
