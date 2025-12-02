#ifndef HARDWARE_MODULEMANAGER_HPP
#define HARDWARE_MODULEMANAGER_HPP

/**
 * @file ModuleManager.hpp
 * @brief Central module lifecycle and event routing manager.
 *
 * Manages registration, startup, shutdown, and event routing for
 * all firmware modules with priority-based ordering.
 */

#include <vector>
#include <string_view>
#include <cstdint>

#include "core/IModule.hpp"
#include "core/EventBus.hpp"

namespace isic {
    /**
     * @brief Module registration entry.
     */
    struct ModuleEntry {
        IModule* module{nullptr};
        ModuleInfo info{};
        bool registered{false};
    };

    /**
     * @brief Module manager metrics.
     */
    struct ModuleManagerMetrics {
        std::size_t totalModules{0};
        std::size_t runningModules{0};
        std::size_t enabledModules{0};
        std::size_t errorModules{0};
    };

    /**
     * @brief Central module manager for lifecycle and event routing.
     *
     * Features:
     * - Manages module lifecycle (init, start, stop)
     * - Routes events to modules
     * - Handles config updates
     * - Supports enable/disable via config and MQTT
     * - Priority-based startup order
     * - Module discovery and status reporting
     *
     * Usage:
     *   ModuleManager mgr(bus);
     *   mgr.addModule(attendanceModule);
     *   mgr.addModule(otaModule);
     *   mgr.startAll();
     *   // ... modules receive events automatically ...
     *   mgr.stopAll();
     */
    class ModuleManager : public IEventListener {
    public:
        explicit ModuleManager(EventBus& bus);
        ModuleManager(const ModuleManager&) = delete;
        ModuleManager& operator=(const ModuleManager&) = delete;
        ModuleManager(ModuleManager&&) = delete;
        ModuleManager& operator=(ModuleManager&&) = delete;
        ~ModuleManager() override;

        // ==================== Module Registration ====================

        /**
         * @brief Add a module to the manager.
         * @param module Module to add
         * @return true if added successfully
         */
        bool addModule(IModule& module);

        /**
         * @brief Remove a module from the manager.
         * @param name Module name
         * @return true if removed
         */
        bool removeModule(std::string_view name);

        /**
         * @brief Get a module by name.
         * @param name Module name
         * @return Pointer to module or nullptr
         */
        [[nodiscard]] IModule* getModule(std::string_view name) const;

        /**
         * @brief Get all registered modules.
         */
        [[nodiscard]] const std::vector<ModuleEntry>& getModules() const noexcept { return m_modules; }

        // ==================== Lifecycle Management ====================

        /**
         * @brief Start all enabled modules.
         * Modules are started in priority order (highest first).
         */
        void startAll();

        /**
         * @brief Stop all running modules.
         * Modules are stopped in reverse priority order.
         */
        void stopAll();

        /**
         * @brief Start a specific module by name.
         */
        bool startModule(std::string_view name);

        /**
         * @brief Stop a specific module by name.
         */
        bool stopModule(std::string_view name);

        /**
         * @brief Enable a module (and start if manager is running).
         */
        bool enableModule(std::string_view name);

        /**
         * @brief Disable a module (stops it first).
         */
        bool disableModule(std::string_view name);

        // ==================== Status & Metrics ====================

        /**
         * @brief Get manager metrics.
         */
        [[nodiscard]] ModuleManagerMetrics getMetrics() const;

        /**
         * @brief Get number of registered modules.
         */
        [[nodiscard]] std::size_t getModuleCount() const noexcept { return m_modules.size(); }

        /**
         * @brief Check if all modules are running.
         */
        [[nodiscard]] bool allRunning() const;

        // ==================== IEventListener ====================

        void onEvent(const Event& event) override;

    private:
        void sortByPriority();

        void applyModuleConfig(const ModulesConfig& cfg);

        void publishModuleStateChange(std::string_view name, bool enabled);

        EventBus& m_bus;
        std::vector<ModuleEntry> m_modules{};
        EventBus::ListenerId m_subscriptionId{0};
        bool m_allStarted{false};

        static constexpr std::size_t MAX_MODULES = 16;
    };

}  // namespace isic

#endif  // HARDWARE_MODULEMANAGER_HPP
