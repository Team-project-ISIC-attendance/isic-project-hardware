#ifndef ISIC_PROJECT_HARDWARE_OTAMODULE_HPP
#define ISIC_PROJECT_HARDWARE_OTAMODULE_HPP

/**
 * @file OtaModule.hpp
 * @brief OTA update module providing high-level OTA management interface.
 *
 * This module wraps OtaService and provides:
 * - MQTT command handling for OTA operations
 * - Event-driven OTA coordination
 * - Progress and status broadcasting
 * - Integration with ModuleManager lifecycle
 *
 * MQTT OTA Commands (on topic: device/<id>/ota/set):
 *
 * 1. Trigger Update:
 *    {
 *      "action": "update",
 *      "url": "https://server.com/firmware.bin",
 *      "version": "1.2.3",
 *      "sha256": "abc123...",  (optional)
 *      "force": false          (optional)
 *    }
 *
 * 2. Check for Updates:
 *    { "action": "check" }
 *
 * 3. Cancel In-Progress Update:
 *    { "action": "cancel" }
 *
 * 4. Rollback to Previous Firmware:
 *    { "action": "rollback" }
 *
 * 5. Mark Current Partition Valid:
 *    { "action": "mark_valid" }
 *
 * 6. Get Current Status:
 *    { "action": "get_status" }
 *
 * Status is published to: device/<id>/ota/status (retained)
 * Progress is published to: device/<id>/ota/progress
 */

#include "core/IModule.hpp"
#include "services/OtaService.hpp"

#include <ArduinoJson.h>

namespace isic {

    /**
     * @brief OTA update module for managing firmware updates.
     *
     * Handles:
     * - OTA commands from MQTT (parsed and dispatched)
     * - Config updates
     * - Progress reporting with UI feedback
     * - Graceful handling during updates
     */
    class OtaModule : public IModule, public IEventListener {
    public:
        OtaModule(OtaService& otaService, EventBus& bus);
        ~OtaModule() override = default;

        // Non-copyable, non-movable
        OtaModule(const OtaModule&) = delete;
        OtaModule& operator=(const OtaModule&) = delete;
        OtaModule(OtaModule&&) = delete;
        OtaModule& operator=(OtaModule&&) = delete;

        // ==================== IModule Interface ====================

        void start() override;
        void stop() override;

        void handleEvent(const Event& event) override;
        void handleConfigUpdate(const AppConfig& config) override;

        // ==================== IEventListener Interface ====================

        void onEvent(const Event& event) override { handleEvent(event); }

        [[nodiscard]] ModuleInfo getInfo() const override {
            return ModuleInfo{
                .name = "OTA",
                .version = "1.0.0",
                .description = "Over-the-air firmware updates with rollback support",
                .enabledByDefault = true,
                .priority = 5  // Medium priority
            };
        }

        // ==================== OTA Command Handling ====================

        /**
         * @brief Parse and execute an OTA command from JSON payload.
         * @param payload JSON command string
         * @return Status indicating success/failure
         */
        [[nodiscard]] Status handleOtaCommand(const std::string& payload);

        /**
         * @brief Parse OTA command JSON into OtaCommand struct.
         * @param payload JSON string
         * @param cmd Output command struct
         * @return true if parsing succeeded
         */
        [[nodiscard]] static bool parseOtaCommand(const std::string& payload, OtaCommand& cmd);

    private:
        // Event handlers
        void onOtaStateChanged(const OtaStateChangedEvent& event);
        void onOtaProgress(const OtaProgressEvent& event);
        void onMqttMessage(const MqttMessageEvent& event);

        OtaService& m_ota;
        EventBus& m_bus;
        EventBus::ListenerId m_subscriptionId{0};
    };

}  // namespace isic

#endif  // ISIC_PROJECT_HARDWARE_OTAMODULE_HPP
