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
// TODO: remake thius module, becouse it is bad designed and hard to use
class HealthService : public ServiceBase
{
public:
    HealthService(EventBus& bus, HealthConfig& config);
    ~HealthService() override = default;

    HealthService(const HealthService&) = delete;
    HealthService& operator=(const HealthService&) = delete;
    HealthService(HealthService&&) = delete;
    HealthService& operator=(HealthService&&) = delete;

    // IService implementation
    Status begin() override;
    void loop() override;
    void end() override;

    // Component registration
    void registerComponent(IHealthReporter* component);
    void unregisterComponent(IHealthReporter* component);

    // Health queries
    [[nodiscard]] ServiceHealth getComponentHealth(const char* name) const noexcept;

    [[nodiscard]] const SystemHealth& getSystemHealth() const noexcept
    {
        return m_systemHealth;
    }

    [[nodiscard]] HealthState getOverallState() const noexcept
    {
        return m_systemHealth.overallState;
    }

    [[nodiscard]] bool isHealthy() const noexcept
    {
        return m_systemHealth.overallState == HealthState::Healthy;
    }

    // Manual operations
    void checkNow();
    void publishNow();

    // Uptime
    [[nodiscard]] std::uint32_t getUptimeMs() const noexcept
    {
        return millis() - m_startTimeMs;
    }

    [[nodiscard]] std::uint32_t getUptimeSec() const noexcept
    {
        return getUptimeMs() / 1000;
    }

private:
    void checkComponents();
    void updateSystemHealth();
    void publishStatusUpdate();
    void publishOnlineStatus();
    void setComponentHealth(const char *component, HealthState state, const char *message);
    void aggregateHealth();

    // Dependencies
    EventBus &m_bus;
    HealthConfig &m_config;

    // Registered health reporters
    std::array<IHealthReporter *, HealthConfig::Constants::kMaxComponentsCount> m_components{};
    std::size_t m_componentCount{0};

    // Component health status
    std::array<ServiceHealth, HealthConfig::Constants::kMaxComponentsCount> m_componentHealth{};

    // System health summary
    SystemHealth m_systemHealth{};

    // Timing
    std::uint32_t m_startTimeMs{0};
    std::uint32_t m_lastCheckMs{0};
    std::uint32_t m_lastStatusUpdateMs{0};

    // Cache previous state to detect changes
    HealthState m_lastPublishedState{HealthState::Unknown};

    // Event subscriptions
    std::vector<EventBus::ScopedConnection> m_eventConnections{};
};
} // namespace isic

#endif // ISIC_SERVICES_HEALTHSERVICE_HPP
