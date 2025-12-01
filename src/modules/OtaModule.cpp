/**
 * @file OtaModule.cpp
 * @brief OTA module implementation with MQTT command handling.
 */

#include "modules/OtaModule.hpp"
#include "core/Logger.hpp"

namespace isic {

    namespace {
        constexpr auto* OTA_MODULE_TAG = "OtaModule";

        // Known action strings for JSON parsing
        constexpr const char* ACTION_UPDATE = "update";
        constexpr const char* ACTION_ROLLBACK = "rollback";
        constexpr const char* ACTION_CHECK = "check";
        constexpr const char* ACTION_CANCEL = "cancel";
        constexpr const char* ACTION_MARK_VALID = "mark_valid";
        constexpr const char* ACTION_GET_STATUS = "get_status";
    }

    OtaModule::OtaModule(OtaService& otaService, EventBus& bus)
        : m_ota(otaService)
        , m_bus(bus) {

        setState(ModuleState::Initialized);
    }

    void OtaModule::start() {
        setState(ModuleState::Starting);

        // Subscribe to relevant events
        m_subscriptionId = m_bus.subscribe(
            this,
            EventFilter::only(EventType::OtaStateChanged)
                .include(EventType::OtaProgress)
                .include(EventType::MqttMessageReceived)
        );

        LOG_INFO(OTA_MODULE_TAG, "OtaModule started");
        LOG_INFO(OTA_MODULE_TAG, "  Listening for OTA commands on MQTT");

        // Log current partition info
        const auto running = m_ota.getRunningPartition();
        if (!running.label.empty()) {
            LOG_INFO(OTA_MODULE_TAG, "  Running: %s (v%s)",
                     running.label.c_str(),
                     running.appVersion.empty() ? "unknown" : running.appVersion.c_str());
        }

        const auto next = m_ota.getNextUpdatePartition();
        if (!next.label.empty()) {
            LOG_INFO(OTA_MODULE_TAG, "  Next OTA slot: %s", next.label.c_str());
        }

        if (m_ota.isPendingValidation()) {
            LOG_WARNING(OTA_MODULE_TAG, "  Boot is pending validation!");
        }

        setState(ModuleState::Running);
    }

    void OtaModule::stop() {
        setState(ModuleState::Stopping);

        // Unsubscribe from EventBus
        if (m_subscriptionId != 0) {
            m_bus.unsubscribe(m_subscriptionId);
            m_subscriptionId = 0;
        }

        LOG_INFO(OTA_MODULE_TAG, "OtaModule stopped");

        setState(ModuleState::Stopped);
    }

    void OtaModule::handleEvent(const Event& event) {
        switch (event.type) {
            case EventType::OtaStateChanged:
                if (const auto* ota = std::get_if<OtaStateChangedEvent>(&event.payload)) {
                    onOtaStateChanged(*ota);
                }
                break;

            case EventType::OtaProgress:
                if (const auto* prog = std::get_if<OtaProgressEvent>(&event.payload)) {
                    onOtaProgress(*prog);
                }
                break;

            case EventType::MqttMessageReceived:
                if (const auto* msg = std::get_if<MqttMessageEvent>(&event.payload)) {
                    onMqttMessage(*msg);
            }
                break;

            default:
                break;
        }
    }

    void OtaModule::handleConfigUpdate(const AppConfig& config) {
        m_ota.updateConfig(config.ota);

        // Update enabled state based on module config
        const bool enabled = config.modules.otaEnabled && config.ota.enabled;
        setEnabled(enabled);
        m_ota.setEnabled(enabled);

        LOG_DEBUG(OTA_MODULE_TAG, "Config updated: enabled=%s, autoCheck=%s, autoUpdate=%s",
                 enabled ? "yes" : "no",
                 config.ota.autoCheck ? "yes" : "no",
                 config.ota.autoUpdate ? "yes" : "no");
    }

    Status OtaModule::handleOtaCommand(const std::string& payload) {
        OtaCommand cmd{};

        if (!parseOtaCommand(payload, cmd)) {
            LOG_WARNING(OTA_MODULE_TAG, "Failed to parse OTA command: %s", payload.c_str());
            return Status::Error(ErrorCode::JsonError, "Invalid JSON command");
        }

        LOG_INFO(OTA_MODULE_TAG, "Executing OTA command: %s", toString(cmd.action));

        return m_ota.executeCommand(cmd);
    }

    bool OtaModule::parseOtaCommand(const std::string& payload, OtaCommand& cmd) {
        JsonDocument doc;

        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            LOG_WARNING(OTA_MODULE_TAG, "JSON parse error: %s", err.c_str());
            return false;
        }

        const JsonObject root = doc.as<JsonObject>();

        // Parse action (required)
        const char* actionStr = root["action"] | "";
        if (strlen(actionStr) == 0) {
            LOG_WARNING(OTA_MODULE_TAG, "Missing 'action' field");
            return false;
        }

        // Map action string to enum
        if (strcmp(actionStr, ACTION_UPDATE) == 0) {
            cmd.action = OtaAction::Update;
        } else if (strcmp(actionStr, ACTION_ROLLBACK) == 0) {
            cmd.action = OtaAction::Rollback;
        } else if (strcmp(actionStr, ACTION_CHECK) == 0) {
            cmd.action = OtaAction::Check;
        } else if (strcmp(actionStr, ACTION_CANCEL) == 0) {
            cmd.action = OtaAction::Cancel;
        } else if (strcmp(actionStr, ACTION_MARK_VALID) == 0) {
            cmd.action = OtaAction::MarkValid;
        } else if (strcmp(actionStr, ACTION_GET_STATUS) == 0) {
            cmd.action = OtaAction::GetStatus;
        } else {
            LOG_WARNING(OTA_MODULE_TAG, "Unknown action: %s", actionStr);
            return false;
        }

        // Parse optional fields
        if (root["url"].is<const char*>()) {
            cmd.url = root["url"].as<const char*>();
        }

        if (root["version"].is<const char*>()) {
            cmd.version = root["version"].as<const char*>();
        }

        if (root["sha256"].is<const char*>()) {
            cmd.sha256 = root["sha256"].as<const char*>();
        }

        cmd.force = root["force"] | false;
        cmd.timeout = root["timeout"] | 0;

        // Validate required fields for update action
        if (cmd.action == OtaAction::Update && cmd.url.empty()) {
            LOG_WARNING(OTA_MODULE_TAG, "Update action requires 'url' field");
            return false;
        }

        return true;
    }

    void OtaModule::onOtaStateChanged(const OtaStateChangedEvent& event) {
        const auto oldState = static_cast<OtaState>(event.oldState);
        const auto newState = static_cast<OtaState>(event.newState);

        LOG_INFO(OTA_MODULE_TAG, "OTA state: %s -> %s",
                 toString(oldState), toString(newState));

        if (!event.message.empty()) {
            LOG_INFO(OTA_MODULE_TAG, "  Message: %s", event.message.c_str());
        }

        // Could trigger UI feedback here
        switch (newState) {
            case OtaState::Downloading:
                // Signal OTA started (LED pattern, etc.)
                {
                    Event feedbackEvt{
                        .type = EventType::FeedbackRequested,
                        .payload = FeedbackRequestEvent{
                            .signal = FeedbackSignal::OtaStarted,
                            .repeatCount = 1
                        },
                        .timestampMs = static_cast<std::uint64_t>(millis())
                    };
                    (void)m_bus.publish(feedbackEvt);
                }
                break;

            case OtaState::Completed:
                // Signal OTA complete (success pattern)
                {
                    Event feedbackEvt{
                        .type = EventType::FeedbackRequested,
                        .payload = FeedbackRequestEvent{
                            .signal = FeedbackSignal::OtaComplete,
                            .repeatCount = 1
                        },
                        .timestampMs = static_cast<std::uint64_t>(millis())
                    };
                    (void)m_bus.publish(feedbackEvt);
                }
                break;

            case OtaState::Failed:
                // Signal OTA failed (error pattern)
                {
                    Event feedbackEvt{
                        .type = EventType::FeedbackRequested,
                        .payload = FeedbackRequestEvent{
                            .signal = FeedbackSignal::Error,
                            .repeatCount = 3
                        },
                        .timestampMs = static_cast<std::uint64_t>(millis())
                    };
                    (void)m_bus.publish(feedbackEvt);
                }
                break;

            default:
                break;
        }
    }

    void OtaModule::onOtaProgress(const OtaProgressEvent& event) {
        // Log progress at 10% intervals to avoid spam
        static std::uint8_t lastLoggedPercent = 0;

        if (event.percent >= lastLoggedPercent + 10 || event.percent == 100) {
            LOG_DEBUG(OTA_MODULE_TAG, "OTA progress: %u%% (%u/%u bytes)",
                     event.percent, event.bytesDownloaded, event.totalBytes);
            lastLoggedPercent = (event.percent / 10) * 10;
        }

        // Reset for next update
        if (event.percent == 100 || event.percent == 0) {
            lastLoggedPercent = 0;
        }
    }

    void OtaModule::onMqttMessage(const MqttMessageEvent& event) {
        // Check if this is an OTA command topic
        // Expected format: device/<id>/ota/set or ends with /ota/set
        if (event.topic.find("/ota/set") == std::string::npos) {
            return;
        }

        LOG_INFO(OTA_MODULE_TAG, "Received OTA command on %s", event.topic.c_str());

        Status result = handleOtaCommand(event.payload);

        if (!result.ok()) {
            LOG_WARNING(OTA_MODULE_TAG, "OTA command failed: %s", result.message);
        }
    }

}  // namespace isic
