#ifndef ISIC_CORE_ISERVICE_HPP
#define ISIC_CORE_ISERVICE_HPP

/**
 * @file IService.hpp
 * @brief Core service interfaces and base implementation
 *
 * Defines the common service lifecycle contract, metrics reporting interface,
 * and a shared base class for services.
 */

#include "common/Types.hpp"

#include <ArduinoJson.h>

namespace isic
{
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
     * @brief Get the service name
     */
    [[nodiscard]] virtual const char *getName() const = 0;

    /**
     * @brief Get current service state
     */
    [[nodiscard]] virtual ServiceState getState() const = 0;

    /**
     * @brief Serialize service metrics into JSON object
     * @param obj JSON object to populate with metrics
     */
    virtual void serializeMetrics(JsonObject &obj) const = 0;

    /**
     * @brief Check if service is running
     */
    [[nodiscard]] virtual bool isRunning() const
    {
        return getState() == ServiceState::Running;
    }
};

/**
 * @brief Base class combining all service interfaces
 *
 * @note Inherit from this class to create a service with basic functionality
 */
class ServiceBase : public IService
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

protected:
    explicit ServiceBase(const char *name)
        : m_name(name)
    {
    }

    void setState(const ServiceState serviceState)
    {
        m_state = serviceState;
    }

    const char *m_name{nullptr};
    ServiceState m_state{ServiceState::Uninitialized};
};
} // namespace isic

#endif // ISIC_CORE_ISERVICE_HPP
