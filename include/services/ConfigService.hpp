#ifndef HARDWARE_CONFIGSERVICE_HPP
#define HARDWARE_CONFIGSERVICE_HPP

/**
 * @file ConfigService.hpp
 * @brief Configuration management service using NVS storage.
 *
 * @note Provides persistent storage and runtime updates for application
 * configuration via ESP32 NVS (Non-Volatile Storage).
 */

#include <Preferences.h>

#include <string>

#include "AppConfig.hpp"
#include "core/Result.hpp"
#include "core/EventBus.hpp"

namespace isic {
    /**
     * @brief Configuration management service.
     *
     * Responsibilities:
     * - Load configuration from NVS at boot
     * - Save configuration changes to NVS
     * - Parse and apply JSON configuration updates
     * - Notify other components of configuration changes
     *
     * @note Uses Preferences library for NVS access.
     */
    class ConfigService : public IEventListener {
    public:
        explicit ConfigService(EventBus& bus);
        ~ConfigService() override = default;

        /**
         * @brief Initialize the service and load configuration.
         * @return Status indicating success or failure
         */
        [[nodiscard]] Status begin();

        /**
         * @brief Get the current configuration (read-only).
         * @return Reference to the current configuration
         */
        [[nodiscard]] const AppConfig& get() const noexcept {
            return m_config;
        }

        /**
         * @brief Update configuration from a JSON string.
         * @param json JSON configuration string
         * @return Status indicating success or failure
         */
        [[nodiscard]] Status updateFromJson(const std::string& json);

        /**
         * @brief Save current configuration to persistent storage.
         * @return Status indicating success or failure
         */
        [[nodiscard]] Status save();

        void onEvent(const Event& event) override;

    private:
        Status load();
        void notifyUpdated() const;

        EventBus& m_bus;
        Preferences m_prefs{};
        AppConfig m_config{};
    };
}

#endif  // HARDWARE_CONFIGSERVICE_HPP
