#ifndef HARDWARE_IMODULE_HPP
#define HARDWARE_IMODULE_HPP

/**
 * @file IModule.hpp
 * @brief Module interface for pluggable firmware functionality.
 *
 * Modules are the primary extension point for the firmware. Each module
 * has a defined lifecycle (init, start, stop) and receives events from
 * the EventBus.
 */

#include <string_view>
#include <cstdint>

#include "AppConfig.hpp"
#include "core/Events.hpp"
#include "core/EventBus.hpp"

namespace isic {

    /**
     * @brief Module metadata for registration and discovery.
     */
    struct ModuleInfo {
        std::string_view name{};
        std::string_view version{"1.0.0"};
        std::string_view description{};
        bool enabledByDefault{true};
        std::uint8_t priority{0};  // Higher = started first
    };

    /**
     * @brief Module state for lifecycle management.
     */
    enum class ModuleState : std::uint8_t {
        Uninitialized,
        Initialized,
        Starting,
        Running,
        Stopping,
        Stopped,
        Error
    };

    [[nodiscard]] inline constexpr const char* toString(ModuleState state) noexcept {
        switch (state) {
            case ModuleState::Uninitialized: return "uninitialized";
            case ModuleState::Initialized:   return "initialized";
            case ModuleState::Starting:      return "starting";
            case ModuleState::Running:       return "running";
            case ModuleState::Stopping:      return "stopping";
            case ModuleState::Stopped:       return "stopped";
            case ModuleState::Error:         return "error";
            default:                         return "unknown";
        }
    }

    /**
     * @brief Module interface for pluggable functionality.
     *
     * Modules are the primary extension point for the firmware.
     * Each module:
     * - Has a clear lifecycle (init, start, stop)
     * - Receives events via handleEvent()
     * - Receives config updates via handleConfigUpdate()
     * - Can be enabled/disabled at runtime
     * - Provides metadata for discovery
     *
     * To add a new module:
     * 1. Implement IModule interface
     * 2. Return ModuleInfo from getInfo()
     * 3. Register with ModuleManager in main.cpp
     * 4. (Optional) Add enable flag to ModulesConfig
     */
    class IModule {
    public:
        virtual ~IModule() = default;

        // ==================== Lifecycle ====================

        /**
         * @brief Start the module.
         * Called by ModuleManager when system starts or module is enabled.
         */
        virtual void start() = 0;

        /**
         * @brief Stop the module.
         * Called by ModuleManager when system stops or module is disabled.
         */
        virtual void stop() = 0;

        // ==================== Event Handling ====================

        /**
         * @brief Handle an event from the EventBus.
         * @param event Event to handle
         */
        virtual void handleEvent(const Event& event) = 0;

        /**
         * @brief Handle configuration update.
         * @param config New configuration
         */
        virtual void handleConfigUpdate(const AppConfig& config) = 0;

        // ==================== Metadata ====================

        /**
         * @brief Get module metadata.
         * @return ModuleInfo with name, version, etc.
         */
        [[nodiscard]] virtual ModuleInfo getInfo() const = 0;

        /**
         * @brief Get current module state.
         */
        [[nodiscard]] virtual ModuleState getState() const noexcept { return m_state; }

        /**
         * @brief Check if module is currently running.
         */
        [[nodiscard]] bool isRunning() const noexcept {
            return m_state == ModuleState::Running;
        }

        /**
         * @brief Check if module is enabled.
         */
        [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }

        /**
         * @brief Set module enabled state.
         * When disabled, module will be stopped.
         * @param enabled New enabled state
         */
        void setEnabled(bool enabled) noexcept { m_enabled = enabled; }

    protected:
        /**
         * @brief Set module state (for derived classes).
         */
        void setState(ModuleState state) noexcept { m_state = state; }

        ModuleState m_state{ModuleState::Uninitialized};
        bool m_enabled{true};
    };

    /**
     * @brief Base class for modules with common functionality.
     *
     * Provides:
     * - State management
     * - EventBus integration
     * - Default implementations
     *
     * Usage:
     *   class MyModule : public ModuleBase {
     *   public:
     *       MyModule(EventBus& bus) : ModuleBase(bus) {}
     *
     *       ModuleInfo getInfo() const override {
     *           return {"MyModule", "1.0.0", "Description", true, 0};
     *       }
     *
     *   protected:
     *       void onStart() override { ... }
     *       void onStop() override { ... }
     *       void onEvent(const Event& event) override { ... }
     *   };
     */
    class ModuleBase : public IModule, public IEventListener {
    public:
        explicit ModuleBase(EventBus& bus) : m_bus(bus) {
            m_subscriptionId = m_bus.subscribe(this);
        }

        ~ModuleBase() override {
            m_bus.unsubscribe(m_subscriptionId);
        }

        // Non-copyable, non-movable
        ModuleBase(const ModuleBase&) = delete;
        ModuleBase& operator=(const ModuleBase&) = delete;
        ModuleBase(ModuleBase&&) = delete;
        ModuleBase& operator=(ModuleBase&&) = delete;

        void start() override final {
            if (m_state == ModuleState::Running) {
                return;
            }

            setState(ModuleState::Starting);
            onStart();
            setState(ModuleState::Running);
        }

        void stop() override final {
            if (m_state != ModuleState::Running) {
                return;
            }

            setState(ModuleState::Stopping);
            onStop();
            setState(ModuleState::Stopped);
        }

        void handleEvent(const Event& event) override {
            if (m_state == ModuleState::Running) {
                onEvent(event);
            }
        }

        void handleConfigUpdate(const AppConfig& config) override {
            if (m_state == ModuleState::Running || m_state == ModuleState::Initialized) {
                onConfigUpdate(config);
            }
        }

    protected:
        /**
         * @brief Called when module is starting.
         * Override to perform initialization.
         */
        virtual void onStart() {}

        /**
         * @brief Called when module is stopping.
         * Override to perform cleanup.
         */
        virtual void onStop() {}

        /**
         * @brief Called for each event when module is running.
         * Override to handle specific events.
         */
        virtual void onEvent(const Event& event) { (void)event; }

        /**
         * @brief Called when configuration changes.
         * Override to apply new configuration.
         */
        virtual void onConfigUpdate(const AppConfig& config) { (void)config; }

        /**
         * @brief Get reference to EventBus.
         */
        [[nodiscard]] EventBus& bus() const {
            return m_bus;
        }

        /**
         * @brief Publish an event.
         */
        [[nodiscard]] bool publish(const Event& event) const {
            return m_bus.publish(event);
        }

    private:
        EventBus& m_bus;
        EventBus::ListenerId m_subscriptionId{0};
    };

}  // namespace isic

#endif  // HARDWARE_IMODULE_HPP
