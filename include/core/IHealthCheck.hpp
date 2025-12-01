#ifndef HARDWARE_IHEALTHCHECK_HPP
#define HARDWARE_IHEALTHCHECK_HPP

/**
 * @file IHealthCheck.hpp
 * @brief Health monitoring interface for system components.
 *
 * Components implementing this interface can be monitored by the
 * HealthMonitorService for system-wide health reporting.
 */

#include <cstdint>
#include <string>
#include <string_view>

#include "Events.hpp"

namespace isic {

    /**
     * @brief Health status information for a component.
     */
    struct HealthStatus {
        HealthState state{HealthState::Unknown};
        std::string_view componentName{};
        std::string message{};
        std::uint64_t lastUpdatedMs{0};

        // Additional metrics for detailed health reporting
        std::uint32_t errorCount{0};
        std::uint32_t warningCount{0};
        std::uint64_t uptimeMs{0};

        [[nodiscard]] bool isHealthy() const noexcept {
            return state == HealthState::Healthy;
        }

        [[nodiscard]] bool isDegraded() const noexcept {
            return state == HealthState::Degraded;
        }

        [[nodiscard]] bool isUnhealthy() const noexcept {
            return state == HealthState::Unhealthy;
        }
    };

    /**
     * @brief Interface for components that provide health status.
     *
     * Any service or module that should be monitored by HealthMonitorService
     * should implement this interface.
     */
    class IHealthCheck {
    public:
        virtual ~IHealthCheck() = default;

        /**
         * @brief Get the current health status of this component.
         *
         * This method should be lightweight and non-blocking.
         * It should return cached/computed state, not perform expensive checks.
         *
         * @return Current health status
         */
        [[nodiscard]] virtual HealthStatus getHealth() const = 0;

        /**
         * @brief Get the component name for identification.
         * @return Component name (should be a string literal or static string)
         */
        [[nodiscard]] virtual std::string_view getComponentName() const = 0;

        /**
         * @brief Perform a quick self-test/health check.
         *
         * This may be called periodically by HealthMonitorService.
         * Should be quick (< 100ms typically) and update internal state.
         *
         * @return true if check passed, false otherwise
         */
        virtual bool performHealthCheck() { return true; }
    };

    /**
     * @brief Aggregate health status for the entire device.
     */
    struct DeviceHealthStatus {
        HealthState overallState{HealthState::Unknown};
        std::uint8_t healthyCount{0};
        std::uint8_t degradedCount{0};
        std::uint8_t unhealthyCount{0};
        std::uint8_t unknownCount{0};
        std::uint64_t lastUpdatedMs{0};

        // System-wide metrics
        std::uint32_t freeHeapBytes{0};
        std::uint32_t minFreeHeapBytes{0};
        std::uint8_t cpuUsagePercent{0};
        std::uint32_t uptimeSeconds{0};

        [[nodiscard]] std::uint8_t totalComponents() const noexcept {
            return healthyCount + degradedCount + unhealthyCount + unknownCount;
        }

        [[nodiscard]] const char* overallStateString() const noexcept {
            return toString(overallState);
        }
    };

}

#endif  // HARDWARE_IHEALTHCHECK_HPP

