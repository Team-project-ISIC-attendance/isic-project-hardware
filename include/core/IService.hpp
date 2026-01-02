#ifndef ISIC_CORE_ISERVICE_HPP
#define ISIC_CORE_ISERVICE_HPP

#include "common/Types.hpp"

namespace isic
{
/**
 * @brief Service lifecycle states
 */
enum class ServiceState : uint8_t
{
    Uninitialized = 0,  ///< Service created but begin() not called
    Initializing,       ///< begin() executing, setting up
    Ready,              ///< Initialized successfully, waiting for dependencies (e.g., WiFi, MQTT)
    Running,            ///< Fully operational and connected
    Stopping,           ///< end() executing, cleaning up
    Stopped,            ///< Cleanly shut down
    Error,              ///< Cannot operate (config error, hardware failure)

    _Count, // NOLINT
};

inline const char *toString(const ServiceState serviceState)
{
    switch (serviceState)
    {
        case ServiceState::Uninitialized: return "uninitialized";
        case ServiceState::Initializing: return "initializing";
        case ServiceState::Ready: return "ready";
        case ServiceState::Running: return "running";
        case ServiceState::Stopping: return "stopping";
        case ServiceState::Stopped: return "stopped";
        case ServiceState::Error: return "error";
        default: return "unknown";
    }
}

/**
 * @brief Base interface for all services
 */
class IService
{
public:
    /**
     * @brief Virtual destructor
     */
    virtual ~IService() = default;

    /**
     * @brief Get the service name
     */
    [[nodiscard]] virtual const char *getName() const = 0;

    /**
     * @brief Initialize the service
     * @return Status indicating success or failure
     */
    [[nodiscard]] virtual Status begin() = 0;

    /**
     * @brief Process service tasks (called from scheduler)
     *
     * @note This method should be non-blocking because it is called from the main loop
     */
    virtual void loop() = 0;

    /**
     * @brief Stop the service
     */
    virtual void end() = 0;

    /**
     * @brief Get current service state
     */
    [[nodiscard]] virtual ServiceState getState() const = 0;

    /**
     * @brief Check if service is running
     */
    [[nodiscard]] virtual bool isRunning() const
    {
        return getState() == ServiceState::Running;
    }
};

/**
 * @brief Interface for services that report health
 */
class IHealthReporter
{
public:
    virtual ~IHealthReporter() = default;

    /**
     * @brief Get current health status
     */
    [[nodiscard]] virtual ServiceHealth getHealth() const = 0;

    /**
     * @brief Perform active health check
     * @return true if healthy
     */
    [[nodiscard]] virtual bool checkHealth() = 0;
};

/**
 * @brief Interface for services with metrics
 */
class IMetricsProvider
{
public:
    virtual ~IMetricsProvider() = default;

    /**
     * @brief Get base service metrics
     */
    [[nodiscard]] virtual ServiceMetrics getMetrics() const = 0;
};

/**
 * @brief Base class combining all service interfaces
 *
 * @note Inherit from this class to create a service with basic functionality
 */
class ServiceBase : public IService, public IHealthReporter
{
public:
    [[nodiscard]] const char *getName() const override
    {
        return m_name;
    }

    [[nodiscard]] ServiceState getState() const override
    {
        return m_state;
    }

    [[nodiscard]] ServiceHealth getHealth() const override
    {
        return {
                .state = (m_state == ServiceState::Running) ? HealthState::Healthy
                         : (m_state == ServiceState::Ready) ? HealthState::Warning  // Ready but not running
                         : (m_state == ServiceState::Error) ? HealthState::Unhealthy
                                                            : HealthState::Degraded,
                .name = m_name,
                .message = nullptr,
                .errorCount = m_errorCount,
                .lastUpdateMs = 0};
    }

    [[nodiscard]] bool checkHealth() override
    {
        return (m_state == ServiceState::Running || m_state == ServiceState::Ready);
    }

protected:
    explicit ServiceBase(const char *name)
        : m_name(name)
    {
    }

    void setState(const ServiceState serviceState)
    {
        m_state = serviceState;
    }

    void incrementErrors()
    {
        ++m_errorCount;
    }

    void resetErrors()
    {
        m_errorCount = 0;
    }

    const char *m_name{nullptr};
    ServiceState m_state{ServiceState::Uninitialized};
    std::uint32_t m_errorCount{0};
};
}

#endif // ISIC_CORE_ISERVICE_HPP
