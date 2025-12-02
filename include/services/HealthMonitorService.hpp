#ifndef HARDWARE_HEALTHMONITORSERVICE_HPP
#define HARDWARE_HEALTHMONITORSERVICE_HPP

/**
 * @file HealthMonitorService.hpp
 * @brief Central health monitoring for all system components.
 *
 * Aggregates health statuses from registered components and provides
 * system-wide health reporting via MQTT and serial.
 */

#include <Arduino.h>

#include <vector>
#include <atomic>
#include <cstdint>

#include "AppConfig.hpp"
#include "core/EventBus.hpp"
#include "core/IHealthCheck.hpp"
#include "core/Result.hpp"

namespace isic {
    /**
     * @brief Central health monitoring service for all system components.
     *
     * Responsibilities:
     * - Register and track health of multiple components
     * - Periodically collect health statuses
     * - Aggregate into global device health state
     * - Publish health to MQTT and log to serial
     * - Emit HealthStatusChanged events when components change state
     */
    class HealthMonitorService : public IEventListener {
    public:
        explicit HealthMonitorService(EventBus& bus);
        ~HealthMonitorService() override;

        // Non-copyable, non-movable
        HealthMonitorService(const HealthMonitorService&) = delete;
        HealthMonitorService& operator=(const HealthMonitorService&) = delete;
        HealthMonitorService(HealthMonitorService&&) = delete;
        HealthMonitorService& operator=(HealthMonitorService&&) = delete;

        /**
         * @brief Initialize the health monitor service.
         * @param cfg Application configuration
         * @return Status indicating success or failure
         */
        [[nodiscard]] Status begin(const AppConfig& cfg);

        /**
         * @brief Stop the health monitor service.
         */
        void stop();

        /**
         * @brief Register a component for health monitoring.
         * @param component Pointer to component implementing IHealthCheck
         */
        void registerComponent(IHealthCheck* component);

        /**
         * @brief Unregister a component from health monitoring.
         * @param component Pointer to component to unregister
         */
        void unregisterComponent(IHealthCheck* component);

        /**
         * @brief Get the current aggregate device health status.
         */
        [[nodiscard]] DeviceHealthStatus getDeviceHealth() const;

        /**
         * @brief Get health status of a specific component by name.
         * @param name Component name
         * @return HealthStatus or Unknown if not found
         */
        [[nodiscard]] HealthStatus getComponentHealth(std::string_view name) const;

        /**
         * @brief Force an immediate health check of all components.
         */
        void checkNow();

        /**
         * @brief Get the number of registered components.
         */
        [[nodiscard]] std::size_t getComponentCount() const;

        /**
         * @brief Update configuration at runtime.
         */
        void updateConfig(const HealthConfig& cfg);

        // EventBus interface
        void onEvent(const Event& event) override;

    private:
        // Background task for periodic health checks
        static void healthTaskThunk(void* arg);
        void healthTask();

        // Health check logic
        void performHealthChecks();
        void updateAggregateHealth();
        void checkForStateChanges();

        // Reporting
        void logHealthStatus() const;
        void publishHealthToMqtt() const;
        std::string buildHealthJson() const;

        // System metrics
        void updateSystemMetrics();

        // Component tracking
        struct ComponentEntry {
            IHealthCheck* component{nullptr};
            HealthStatus lastStatus{};
            HealthState previousState{HealthState::Unknown};
            std::uint64_t lastCheckMs{0};
        };

        EventBus& m_bus;
        const HealthConfig* m_cfg{nullptr};
        const AppConfig* m_appCfg{nullptr};

        // Task management
        TaskHandle_t m_taskHandle{nullptr};
        std::atomic<bool> m_running{false};

        // Component tracking
        mutable SemaphoreHandle_t m_componentsMutex{nullptr};
        std::vector<ComponentEntry> m_components{};

        // Aggregate status
        DeviceHealthStatus m_deviceHealth{};
        std::uint64_t m_startTimeMs{0};
        std::uint64_t m_lastReportMs{0};

        // EventBus subscription
        EventBus::ListenerId m_subscriptionId{0};
    };

}  // namespace isic

#endif  // HARDWARE_HEALTHMONITORSERVICE_HPP

