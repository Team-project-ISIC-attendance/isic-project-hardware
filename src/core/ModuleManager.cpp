#include "core/ModuleManager.hpp"
#include "core/Logger.hpp"

#include <algorithm>

namespace isic {

    namespace {
        constexpr auto* MGR_TAG = "ModuleMgr";
    }

    ModuleManager::ModuleManager(EventBus& bus) : m_bus(bus) {
        m_modules.reserve(MAX_MODULES);
        m_subscriptionId = m_bus.subscribe(this,
            EventFilter::only(EventType::ConfigUpdated)
                .include(EventType::MqttMessageReceived));
    }

    ModuleManager::~ModuleManager() {
        stopAll();
        m_bus.unsubscribe(m_subscriptionId);
    }

    bool ModuleManager::addModule(IModule& module) {
        if (m_modules.size() >= MAX_MODULES) {
            LOG_ERROR(MGR_TAG, "Max modules reached (%zu)", MAX_MODULES);
            return false;
        }

        const auto info = module.getInfo();

        // Check for duplicate
        for (const auto& entry : m_modules) {
            if (entry.info.name == info.name) {
                LOG_WARNING(MGR_TAG, "Module already registered: %.*s",
                           static_cast<int>(info.name.size()), info.name.data());
                return false;
            }
        }

        ModuleEntry entry{};
        entry.module = &module;
        entry.info = info;
        entry.registered = true;

        // Set initial enabled state from module info
        module.setEnabled(info.enabledByDefault);

        m_modules.push_back(entry);

        LOG_INFO(MGR_TAG, "Registered module: %.*s v%.*s (priority=%u, enabled=%s)",
                 static_cast<int>(info.name.size()), info.name.data(),
                 static_cast<int>(info.version.size()), info.version.data(),
                 info.priority,
                 info.enabledByDefault ? "yes" : "no");

        return true;
    }

    bool ModuleManager::removeModule(std::string_view name) {
        for (auto it = m_modules.begin(); it != m_modules.end(); ++it) {
            if (it->info.name == name) {
                if (it->module && it->module->isRunning()) {
                    it->module->stop();
                }
                m_modules.erase(it);
                LOG_INFO(MGR_TAG, "Removed module: %.*s",
                        static_cast<int>(name.size()), name.data());
                return true;
            }
        }
        return false;
    }

    IModule* ModuleManager::getModule(std::string_view name) {
        for (auto& entry : m_modules) {
            if (entry.info.name == name) {
                return entry.module;
            }
        }
        return nullptr;
    }

    void ModuleManager::startAll() {
        LOG_INFO(MGR_TAG, "Starting %zu modules...", m_modules.size());

        sortByPriority();

        std::size_t started = 0;
        for (auto& entry : m_modules) {
            if (!entry.module) continue;

            if (!entry.module->isEnabled()) {
                LOG_DEBUG(MGR_TAG, "Skipping disabled module: %.*s",
                         static_cast<int>(entry.info.name.size()),
                         entry.info.name.data());
                continue;
            }

            LOG_DEBUG(MGR_TAG, "Starting module: %.*s",
                     static_cast<int>(entry.info.name.size()),
                     entry.info.name.data());

            entry.module->start();

            if (entry.module->isRunning()) {
                ++started;
            } else {
                LOG_WARNING(MGR_TAG, "Module failed to start: %.*s",
                           static_cast<int>(entry.info.name.size()),
                           entry.info.name.data());
            }
        }

        m_allStarted = true;
        LOG_INFO(MGR_TAG, "Started %zu/%zu modules", started, m_modules.size());
    }

    void ModuleManager::stopAll() {
        LOG_INFO(MGR_TAG, "Stopping all modules...");

        // Stop in reverse priority order
        sortByPriority();

        for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
            if (it->module && it->module->isRunning()) {
                LOG_DEBUG(MGR_TAG, "Stopping module: %.*s",
                         static_cast<int>(it->info.name.size()),
                         it->info.name.data());
                it->module->stop();
            }
        }

        m_allStarted = false;
        LOG_INFO(MGR_TAG, "All modules stopped");
    }

    bool ModuleManager::startModule(std::string_view name) {
        auto* module = getModule(name);
        if (!module) {
            LOG_WARNING(MGR_TAG, "Module not found: %.*s",
                       static_cast<int>(name.size()), name.data());
            return false;
        }

        if (module->isRunning()) {
            return true;  // Already running
        }

        if (!module->isEnabled()) {
            module->setEnabled(true);
        }

        module->start();
        return module->isRunning();
    }

    bool ModuleManager::stopModule(std::string_view name) {
        auto* module = getModule(name);
        if (!module) {
            return false;
        }

        if (!module->isRunning()) {
            return true;  // Already stopped
        }

        module->stop();
        return !module->isRunning();
    }

    bool ModuleManager::enableModule(std::string_view name) {
        auto* module = getModule(name);
        if (!module) {
            return false;
        }

        if (module->isEnabled()) {
            return true;  // Already enabled
        }

        module->setEnabled(true);
        publishModuleStateChange(name, true);

        // Start if manager is running
        if (m_allStarted) {
            module->start();
        }

        LOG_INFO(MGR_TAG, "Enabled module: %.*s",
                static_cast<int>(name.size()), name.data());

        return true;
    }

    bool ModuleManager::disableModule(std::string_view name) {
        auto* module = getModule(name);
        if (!module) {
            return false;
        }

        if (!module->isEnabled()) {
            return true;  // Already disabled
        }

        // Stop first if running
        if (module->isRunning()) {
            module->stop();
        }

        module->setEnabled(false);
        publishModuleStateChange(name, false);

        LOG_INFO(MGR_TAG, "Disabled module: %.*s",
                static_cast<int>(name.size()), name.data());

        return true;
    }

    ModuleManagerMetrics ModuleManager::getMetrics() const {
        ModuleManagerMetrics metrics{};
        metrics.totalModules = m_modules.size();

        for (const auto& entry : m_modules) {
            if (!entry.module) continue;

            if (entry.module->isRunning()) {
                ++metrics.runningModules;
            }

            if (entry.module->isEnabled()) {
                ++metrics.enabledModules;
            }

            if (entry.module->getState() == ModuleState::Error) {
                ++metrics.errorModules;
            }
        }

        return metrics;
    }

    bool ModuleManager::allRunning() const {
        for (const auto& entry : m_modules) {
            if (entry.module && entry.module->isEnabled() && !entry.module->isRunning()) {
                return false;
            }
        }
        return true;
    }

    void ModuleManager::onEvent(const Event& event) {
        // Handle config updates
        if (event.type == EventType::ConfigUpdated) {
            const auto* cfgEvt = std::get_if<ConfigUpdatedEvent>(&event.payload);
            if (cfgEvt && cfgEvt->config) {
                applyModuleConfig(cfgEvt->config->modules);

                // Forward to all running modules
                for (auto& entry : m_modules) {
                    if (entry.module && entry.module->isRunning()) {
                        entry.module->handleConfigUpdate(*cfgEvt->config);
                    }
                }
            }
            return;
        }

        // Handle MQTT commands for module control
        if (event.type == EventType::MqttMessageReceived) {
            const auto* msg = std::get_if<MqttMessageEvent>(&event.payload);
            if (msg && msg->topic.find("/modules/") != std::string::npos) {
                // Parse module control command
                // Topic format: device/<id>/modules/<module_name>/set
                // Payload: {"enabled": true/false}
                // TODO: Implement MQTT module control
            }
            return;
        }

        // Forward other events to running modules
        for (auto& entry : m_modules) {
            if (entry.module && entry.module->isRunning()) {
                entry.module->handleEvent(event);
            }
        }
    }

    void ModuleManager::sortByPriority() {
        std::sort(m_modules.begin(), m_modules.end(),
            [](const ModuleEntry& a, const ModuleEntry& b) {
                return a.info.priority > b.info.priority;
            });
    }

    void ModuleManager::applyModuleConfig(const ModulesConfig& cfg) {
        // Map config fields to module names
        // This could be made more dynamic with a registry

        for (auto& entry : m_modules) {
            if (!entry.module) continue;

            bool shouldBeEnabled = entry.info.enabledByDefault;

            if (entry.info.name == "Attendance") {
                shouldBeEnabled = cfg.attendanceEnabled;
            } else if (entry.info.name == "OTA") {
                shouldBeEnabled = cfg.otaEnabled;
            }

            const bool currentlyEnabled = entry.module->isEnabled();

            if (shouldBeEnabled != currentlyEnabled) {
                if (shouldBeEnabled) {
                    enableModule(entry.info.name);
                } else {
                    disableModule(entry.info.name);
                }
            }
        }
    }

    void ModuleManager::publishModuleStateChange(std::string_view name, bool enabled) {
        const Event evt{
            .type = EventType::ModuleStateChanged,
            .payload = ModuleStateChangedEvent{
                .moduleName = name,
                .enabled = enabled,
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(evt);
    }

}  // namespace isic
