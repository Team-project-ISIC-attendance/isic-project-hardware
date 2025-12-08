#include "services/HealthMonitorService.hpp"
#include "core/Logger.hpp"

#include <memory>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

namespace isic {
    namespace {
        constexpr auto *HEALTH_TAG{"HealthMonitor"};
        constexpr auto *HEALTH_TASK_NAME{"health_monitor"};
        constexpr std::uint32_t HEALTH_TASK_STACK_SIZE{4096};
        constexpr std::uint8_t HEALTH_TASK_PRIORITY{1};
        constexpr std::uint8_t HEALTH_TASK_CORE{0};
        constexpr std::size_t MAX_COMPONENTS{16};
    }

    HealthMonitorService::HealthMonitorService(EventBus &bus) : m_bus(bus) {
        m_componentsMutex = xSemaphoreCreateMutex();
        m_components.reserve(MAX_COMPONENTS);
        m_subscriptionId = m_bus.subscribe(this);
    }

    HealthMonitorService::~HealthMonitorService() {
        stop();
        if (m_componentsMutex) {
            vSemaphoreDelete(m_componentsMutex);
            m_componentsMutex = nullptr;
        }
        m_bus.unsubscribe(m_subscriptionId);
    }

    Status HealthMonitorService::begin(const AppConfig &cfg) {
        m_cfg = &cfg.health;
        m_appCfg = &cfg;
        m_startTimeMs = millis();
        m_lastReportMs = 0;

        // Initialize device health
        m_deviceHealth.overallState = HealthState::Unknown;
        m_deviceHealth.lastUpdatedMs = millis();

        // Start background task
        m_running.store(true);
        xTaskCreatePinnedToCore(
            &HealthMonitorService::healthTaskThunk,
            HEALTH_TASK_NAME,
            HEALTH_TASK_STACK_SIZE,
            this,
            HEALTH_TASK_PRIORITY,
            &m_taskHandle,
            HEALTH_TASK_CORE
        );

        LOG_INFO(HEALTH_TAG, "HealthMonitorService started, check interval=%ums", unsigned{m_cfg->checkIntervalMs});
        return Status::OK();
    }

    void HealthMonitorService::stop() {
        m_running.store(false);
        if (m_taskHandle) {
            vTaskDelay(pdMS_TO_TICKS(200));
            m_taskHandle = nullptr;
        }
    }

    void HealthMonitorService::registerComponent(IHealthCheck *component) {
        if (!component) return;

        if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(HEALTH_TAG, "Failed to acquire mutex for registration");
            return;
        }

        // Check if already registered
        for (const auto &entry: m_components) {
            if (entry.component == component) {
                xSemaphoreGive(m_componentsMutex);
                LOG_WARNING(HEALTH_TAG, "Component already registered: %.*s", unsigned{component->getComponentName().size()}, component->getComponentName().data());
                return;
            }
        }

        if (m_components.size() >= MAX_COMPONENTS) {
            xSemaphoreGive(m_componentsMutex);
            LOG_ERROR(HEALTH_TAG, "Max components reached (%u)", unsigned{MAX_COMPONENTS});
            return;
        }

        ComponentEntry entry {
            .component = component,
            .lastStatus = HealthStatus{
                .state = HealthState::Unknown,
                .componentName = component->getComponentName(),
            },
            .previousState = HealthState::Unknown,
            .lastCheckMs = 0,
        };

        m_components.push_back(entry);
        xSemaphoreGive(m_componentsMutex);

        LOG_INFO(HEALTH_TAG, "Registered component: %.*s", unsigned{component->getComponentName().size()}, component->getComponentName().data());
    }

    void HealthMonitorService::unregisterComponent(IHealthCheck *component) {
        if (!component) {
            return;
        }

        if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(HEALTH_TAG, "Failed to acquire mutex for unregistration");
            return;
        }

        for (auto it = m_components.begin(); it != m_components.end(); ++it) {
            if (it->component == component) {
                const auto name = it->component->getComponentName();
                m_components.erase(it);
                xSemaphoreGive(m_componentsMutex);
                LOG_INFO(HEALTH_TAG, "Unregistered component: %.*s", unsigned{name.size()}, name.data());
                return;
            }
        }

        xSemaphoreGive(m_componentsMutex);
    }

    DeviceHealthStatus HealthMonitorService::getDeviceHealth() const {
        return m_deviceHealth;
    }

    HealthStatus HealthMonitorService::getComponentHealth(std::string_view name) const {
        if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return HealthStatus{.state = HealthState::Unknown, .componentName = name};
        }

        for (const auto &entry: m_components) {
            if (entry.component && entry.component->getComponentName() == name) {
                const auto status{entry.lastStatus};
                xSemaphoreGive(m_componentsMutex);
                return status;
            }
        }

        xSemaphoreGive(m_componentsMutex);
        return HealthStatus{
            .state = HealthState::Unknown,
            .componentName = name
        };
    }

    void HealthMonitorService::checkNow() {
        performHealthChecks();
        updateAggregateHealth();
        checkForStateChanges();

        if (m_cfg && m_cfg->logToSerial) {
            logHealthStatus();
        }
    }

    std::size_t HealthMonitorService::getComponentCount() const {
        if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return 0;
        }
        const auto count{m_components.size()};
        xSemaphoreGive(m_componentsMutex);
        return count;
    }

    void HealthMonitorService::updateConfig(const HealthConfig &cfg) {
        m_cfg = &cfg;
        LOG_INFO(HEALTH_TAG, "Config updated: checkInterval=%ums, reportInterval=%ums", unsigned{m_cfg->checkIntervalMs}, unsigned{m_cfg->reportIntervalMs});
    }

    void HealthMonitorService::onEvent(const Event &event) {
        if (event.type == EventType::ConfigUpdated) {
            if (const auto *ce = std::get_if<ConfigUpdatedEvent>(&event.payload)) {
                if (ce->config) {
                    updateConfig(ce->config->health);
                }
            }
        }
    }

    void HealthMonitorService::healthTaskThunk(void *arg) {
        static_cast<HealthMonitorService *>(arg)->healthTask();
    }

    void HealthMonitorService::healthTask() {
        LOG_DEBUG(HEALTH_TAG, "Health monitoring task started");

        while (m_running.load()) {
            const auto now{millis()};
            const auto checkInterval{m_cfg ? m_cfg->checkIntervalMs : 10000};
            const auto reportInterval{m_cfg ? m_cfg->reportIntervalMs : 60000};

            // Perform health checks
            performHealthChecks();
            updateSystemMetrics();
            updateAggregateHealth();
            checkForStateChanges();

            // Log to serial if enabled
            if (m_cfg && m_cfg->logToSerial) {
                static std::uint64_t lastLogMs{0}; // static becouse we want to retain value between iterations
                if (now - lastLogMs >= checkInterval) {
                    logHealthStatus();
                    lastLogMs = now;
                }
            }

            // Publish to MQTT periodically
            if (m_cfg && m_cfg->publishToMqtt) {
                if (now - m_lastReportMs >= reportInterval) {
                    publishHealthToMqtt();
                    m_lastReportMs = now;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(checkInterval));
        }

        LOG_DEBUG(HEALTH_TAG, "Health monitoring task exiting");
        vTaskDelete(nullptr);
    }

    void HealthMonitorService::performHealthChecks() {
        if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return;
        }

        const auto now = millis();

        for (auto &entry: m_components) {
            if (!entry.component) continue;

            // Perform health check
            entry.component->performHealthCheck();

            // Get current status
            entry.lastStatus = entry.component->getHealth();
            entry.lastCheckMs = now;
        }

        xSemaphoreGive(m_componentsMutex);
    }

    void HealthMonitorService::updateAggregateHealth() {
        if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return;
        }

        std::uint8_t healthy{0};
        std::uint8_t degraded{0};
        std::uint8_t unhealthy{0};
        std::uint8_t unknown{0};

        for (const auto &entry: m_components) {
            switch (entry.lastStatus.state) {
                case HealthState::Healthy: {
                    ++healthy;
                    break;
                }
                case HealthState::Degraded: {
                    ++degraded;
                    break;
                }
                case HealthState::Unhealthy: {
                    ++unhealthy;
                    break;
                }
                default: {
                    ++unknown;
                    break;
                }
            }
        }

        xSemaphoreGive(m_componentsMutex);

        // Determine overall state
        auto overall{HealthState::Unknown};
        if (unhealthy > 0) {
            overall = HealthState::Unhealthy;
        } else if (degraded > 0 || unknown > 0) {
            overall = HealthState::Degraded;
        } else if (healthy > 0) {
            overall = HealthState::Healthy;
        }

        m_deviceHealth.overallState = overall;
        m_deviceHealth.healthyCount = healthy;
        m_deviceHealth.degradedCount = degraded;
        m_deviceHealth.unhealthyCount = unhealthy;
        m_deviceHealth.unknownCount = unknown;
        m_deviceHealth.lastUpdatedMs = millis();
        m_deviceHealth.uptimeSeconds = (millis() - m_startTimeMs) / 1000;
    }

    void HealthMonitorService::checkForStateChanges() {
        if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return;
        }

        for (auto &entry: m_components) {
            if (entry.lastStatus.state != entry.previousState) {
                // State changed - emit event (heap-allocated for RTOS safety)
                auto evt = std::make_unique<Event>(Event{
                    .type = EventType::HealthStatusChanged,
                    .payload = HealthStatusChangedEvent{
                        .componentName = entry.lastStatus.componentName,
                        .oldState = entry.previousState,
                        .newState = entry.lastStatus.state,
                        .message = entry.lastStatus.message,
                        .timestampMs = static_cast<std::uint64_t>(millis())
                    },
                    .timestampMs = static_cast<std::uint64_t>(millis())
                });
                entry.previousState = entry.lastStatus.state;

                // Release mutex before publishing to avoid deadlock
                xSemaphoreGive(m_componentsMutex);

                LOG_INFO(HEALTH_TAG, "Health state changed: %.*s: %s -> %s", unsigned{entry.lastStatus.componentName.size()}, entry.lastStatus.componentName.data(), toString(entry.previousState), toString(entry.lastStatus.state));
                (void) m_bus.publish(std::move(evt));  // TODO: check publish result

                // Re-acquire mutex to continue iteration
                if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                    return;
                }
            }
        }

        xSemaphoreGive(m_componentsMutex);
    }

    void HealthMonitorService::logHealthStatus() const {
        LOG_INFO(HEALTH_TAG, "Device health: %s (H:%u D:%u U:%u ?:%u) heap=%uKB uptime=%us", toString(m_deviceHealth.overallState), unsigned{m_deviceHealth.healthyCount}, unsigned{m_deviceHealth.degradedCount}, unsigned{m_deviceHealth.unhealthyCount}, unsigned{m_deviceHealth.unknownCount}, unsigned{m_deviceHealth.freeHeapBytes / 1024}, unsigned{m_deviceHealth.uptimeSeconds});
    }

    void HealthMonitorService::publishHealthToMqtt() const {
        const auto json{buildHealthJson()};

        // Publish via dedicated HealthReportReady event - MqttService will handle actual publishing
        auto evt = std::make_unique<Event>(Event{
            .type = EventType::HealthReportReady,
            .payload = HealthReportReadyEvent{
                .jsonPayload = json,
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        });

        (void) m_bus.publish(std::move(evt));  // TODO: check publish result
        LOG_INFO(HEALTH_TAG, "Published health report to MQTT");
    }

    std::string HealthMonitorService::buildHealthJson() const {
        JsonDocument doc;

        // Overall status
        doc["overall"] = toString(m_deviceHealth.overallState);
        doc["uptime_s"] = m_deviceHealth.uptimeSeconds;
        doc["free_heap_kb"] = m_deviceHealth.freeHeapBytes / 1024;
        doc["min_heap_kb"] = m_deviceHealth.minFreeHeapBytes / 1024;

        // Component counts
        const auto counts{doc["counts"].to<JsonObject>()};
        counts["healthy"] = m_deviceHealth.healthyCount;
        counts["degraded"] = m_deviceHealth.degradedCount;
        counts["unhealthy"] = m_deviceHealth.unhealthyCount;
        counts["unknown"] = m_deviceHealth.unknownCount;

        // Device info
        if (m_appCfg) {
            doc["device_id"] = m_appCfg->device.deviceId;
            doc["firmware"] = m_appCfg->device.firmwareVersion;
        }

        // Component details
        const auto components{doc["components"].to<JsonArray>()};

        if (xSemaphoreTake(m_componentsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (const auto &entry: m_components) {
                if (!entry.component) {
                    continue;
                }

                auto comp{components.add<JsonObject>()};
                const std::string nameStr(entry.lastStatus.componentName);
                comp["name"] = nameStr;
                comp["state"] = toString(entry.lastStatus.state);

                if (!entry.lastStatus.message.empty()) {
                    comp["message"] = entry.lastStatus.message;
                }
                comp["errors"] = entry.lastStatus.errorCount;
            }
            xSemaphoreGive(m_componentsMutex);
        }

        std::string result;
        serializeJson(doc, result);
        return result;
    }

    void HealthMonitorService::updateSystemMetrics() {
        m_deviceHealth.freeHeapBytes = esp_get_free_heap_size();
        m_deviceHealth.minFreeHeapBytes = esp_get_minimum_free_heap_size();
        m_deviceHealth.uptimeSeconds = (millis() - m_startTimeMs) / 1000;

        // CPU usage would require more complex tracking
        // For now, leave at 0 or implement simple idle time tracking
        // TODO: need to implement CPU usage tracking here
        m_deviceHealth.cpuUsagePercent = 0;
    }
}
