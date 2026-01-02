#include "services/OtaService.hpp"

#include "common/Logger.hpp"
#include "services/ConfigService.hpp"

namespace isic {

OtaService::OtaService(EventBus& bus, const OtaConfig& config)
    : ServiceBase("OtaService")
    , m_bus(bus)
    , m_config(config)
{
    // TODO: i chnage ota to new elegant ota async version, so mqtt ota part need re-implemnt if needed

    // // Request subscription to OTA topic when MQTT connects
    // m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttConnected, [this](const Event&) {
    //     m_bus.publish(Event{
    //             EventType::MqttSubscribeRequest,
    //             MqttEvent{.topic = "ota/set"}
    //     });
    // }));
    //
    // // Handle OTA commands via MQTT
    // m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttMessage, [this](const Event& event) {
    //     const auto* mqtt = event.get<MqttEvent>();
    //     if (!mqtt) return;
    //
    //     if (mqtt->topic.find("/ota/set") != std::string::npos) {
    //         LOG_INFO(m_name, "OTA command received via MQTT");
    //         // Parse command and initiate OTA if needed
    //     }
    // }));
}

/**
 * OTA Update Process Overview, if no know about OTA please read this carefully:
 * =============================
 *
 * STEP 1: Service Initialization (begin())
 *   - Load OTA configuration (enabled flag, credentials)
 *   - Register callback handlers with ElegantOTA library
 *   - Start async web server endpoint at /update
 *   - Subscribe to MQTT ota/set topic for remote update triggers
 *
 * STEP 2: User Accesses Web Interface
 *   - User navigates to http://<device-ip>/update
 *   - If authentication enabled: prompts for username/password
 *   - ElegantOTA serves web UI for firmware upload
 *
 * STEP 3: Firmware Upload Begins (onOtaStart)
 *   - User selects .bin file and clicks upload
 *   - onOtaStart() callback fires
 *   - State changes to OtaState::Downloading
 *   - Publishes EventType::OtaStarted to event bus
 *   - Other services can pause/prepare for update
 *
 * STEP 4: Progress Updates (onOtaProgress)
 *   - Called repeatedly as firmware chunks upload
 *   - Calculates percentage: (current/total) * 100
 *   - Logs every 10% milestone
 *   - Publishes EventType::OtaProgress events with byte counts
 *
 * STEP 5: Update Completion (onOtaEnd)
 *   - Success path:
 *     * New firmware written to OTA partition
 *     * State changes to OtaState::Completed
 *     * Publishes EventType::OtaCompleted
 *     * Device will reboot automatically
 *     * Bootloader switches to new partition
 *   - Failure path:
 *     * State changes to OtaState::Error
 *     * Publishes EventType::OtaError
 *     * Device remains on current firmware
 *
 * STEP 6: Post-Update (after reboot)
 *   - Device boots with new firmware
 *   - Bootloader validates new partition
 *   - If valid: runs new firmware
 *   - If invalid: rollback to previous partition
 */
Status OtaService::begin() {
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing OtaService...");

    if (!m_config.enabled) {
        LOG_INFO(m_name, "OTA disabled");
        setState(ServiceState::Running);
        return Status::Ok();
    }

    ElegantOTA.onStart([this]() { onOtaStart(); });
    ElegantOTA.onEnd([this](const bool success) { onOtaEnd(success); });
    ElegantOTA.onProgress([this](const std::size_t current, const std::size_t total) {
        onOtaProgress(current, total);
    });

    if (!m_config.username.empty() && !m_config.password.empty()) {
        ElegantOTA.begin(&m_webServer, m_config.username.c_str(), m_config.password.c_str());
        LOG_INFO(m_name, "OtaService ready with authentication at /update");
    } else {
        ElegantOTA.begin(&m_webServer);
        LOG_INFO(m_name, "OtaService ready (no auth) at /update");
    }

    setState(ServiceState::Running);
    return Status::Ok();
}

void OtaService::loop() {
    ElegantOTA.loop(); // Handle OTA web server tasks
}

void OtaService::end() {
    setState(ServiceState::Stopped);
}

void OtaService::onOtaStart() {
    LOG_INFO(m_name, "OTA update starting...");

    m_otaState = OtaState::Downloading;
    m_progress = 0;

    m_bus.publish(EventType::OtaStarted);
}

void OtaService::onOtaEnd(const bool success) {
    if (success) {
        LOG_INFO(m_name, "OTA update completed successfully");
        m_otaState = OtaState::Completed;
        m_progress = 100;
        m_bus.publish(EventType::OtaCompleted);
    } else {
        LOG_ERROR(m_name, "OTA update failed");
        m_otaState = OtaState::Error;
        m_bus.publish(EventType::OtaError);
    }
}

void OtaService::onOtaProgress(const std::size_t current, const std::size_t total) {
    m_progress = (current * 100) / total;

    // Throttle progress events - only log/publish every 10%
    // Prevents flooding the event bus and serial output
    static uint8_t lastReported = 0;
    if (m_progress != lastReported && m_progress % 10 == 0) {
        LOG_DEBUG(m_name, "OTA progress: %d%%", progress_);
        lastReported = m_progress;

        const Event event(EventType::OtaProgress, OtaProgressEvent {
            .percent = m_progress,
            .bytesReceived = static_cast<std::uint32_t>(current),
            .totalBytes = static_cast<std::uint32_t>(total)
        });
        m_bus.publish(event);
    }
}
} // namespace isic


