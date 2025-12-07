#include "services/MqttService.hpp"
#include "services/PowerService.hpp"
#include "core/Logger.hpp"

#include <ArduinoJson.h>
#include <new>

namespace isic {
    namespace {
        constexpr auto *MQTT_TAG{"MqttService"};
        constexpr auto *MQTT_TASK_NAME{"mqtt_loop"};
        constexpr std::uint32_t MQTT_TASK_LOOP_MS{50};

        constexpr std::uint8_t WIFI_CONNECT_RETRIES{20};
        constexpr std::uint32_t WIFI_CONNECT_DELAY_MS{500};

        constexpr std::size_t DEFAULT_QUEUE_SIZE{64};
        constexpr std::uint32_t MAX_BACKOFF_MS{60000};
    }

    MqttService::MqttService(EventBus &bus) : m_bus(bus), m_client(m_wifiClient) {
        m_metricsMutex = xSemaphoreCreateMutex();
        m_subscriptionId = m_bus.subscribe(this,
        EventFilter::only(EventType::ConfigUpdated)
                .include(EventType::AttendanceRecorded)
                .include(EventType::OtaStateChanged)
                .include(EventType::OtaProgress)
                .include(EventType::HealthStatusChanged)
                .include(EventType::MqttMessageReceived)
        );
    }

    MqttService::~MqttService() {
        stop();

        if (m_outboundQueue) {
            // Drain and delete any remaining messages in the queue
            MqttOutboundMessage *msg{nullptr};
            while (xQueueReceive(m_outboundQueue, &msg, 0) == pdTRUE) {
                delete msg;
            }

            vQueueDelete(m_outboundQueue);
            m_outboundQueue = nullptr;
        }

        if (m_metricsMutex) {
            vSemaphoreDelete(m_metricsMutex);
            m_metricsMutex = nullptr;
        }

        m_bus.unsubscribe(m_subscriptionId);
    }

    Status MqttService::begin(const AppConfig &cfg, PowerService &powerService) {
        m_cfg = &cfg;
        m_mqttCfg = &cfg.mqtt;
        m_powerService = &powerService;

        // Create outbound queue
        const auto queueSize{m_mqttCfg->outboundQueueSize > 0 ? m_mqttCfg->outboundQueueSize : DEFAULT_QUEUE_SIZE};

        m_outboundQueue = xQueueCreate(queueSize, sizeof(MqttOutboundMessage*));
        if (!m_outboundQueue) {
            LOG_ERROR(MQTT_TAG, "Failed to create outbound queue");
            return Status::Error(ErrorCode::ResourceExhausted, "Queue creation failed");
        }

        // Set MQTT callback
        m_client.setCallback([this](char *topic, std::uint8_t *payload, unsigned int length) {
            this->onMqttMessage(topic, payload, length);
        });

        // Set keep alive
        m_client.setKeepAlive(m_mqttCfg->keepAliveSeconds);

        // Start MQTT task
        m_running.store(true);
        xTaskCreatePinnedToCore(
            &MqttService::mqttTaskThunk,
            MQTT_TASK_NAME,
            m_mqttCfg->taskStackSize,
            this,
            m_mqttCfg->taskPriority,
            &m_taskHandle,
            m_mqttCfg->taskCore
        );

        LOG_INFO(MQTT_TAG, "MqttService started, broker=%s:%u, queue=%u", m_mqttCfg->broker.c_str(), unsigned{m_mqttCfg->port}, unsigned{queueSize});
        return Status::OK();
    }

    void MqttService::stop() {
        m_running.store(false);

        if (m_wakeLock.isValid() && m_powerService) {
            m_powerService->releaseWakeLock(m_wakeLock);
        }

        if (m_client.connected()) {
            publishStatus("offline");
            m_client.disconnect();
        }

        if (m_taskHandle) {
            vTaskDelay(pdMS_TO_TICKS(200));
            m_taskHandle = nullptr;
        }

        LOG_INFO(MQTT_TAG, "MqttService stopped");
    }

    bool MqttService::publishAsync(const std::string &topic, const std::string &payload, const bool retained, const std::uint8_t qos) {
        if (!m_outboundQueue) {
            return false;
        }

        // Heap-allocate the message to avoid std::string corruption through FreeRTOS queue memcpy
        auto *msg = new (std::nothrow) MqttOutboundMessage{
            .topic = topic,
            .payload = payload,
            .retained = retained,
            .qos = qos,
            .enqueuedMs = millis()
        };

        if (!msg) {
            LOG_ERROR(MQTT_TAG, "Failed to allocate outbound message");
            return false;
        }

        // Non-blocking enqueue (queue stores pointer)
        if (xQueueSend(m_outboundQueue, &msg, 0) != pdTRUE) {
            if (m_mqttCfg->queueFullPolicy == MqttConfig::QueueFullPolicy::DropNewest) {
                delete msg;  // Clean up the message we just allocated

                if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    m_metrics.messagesDropped++;
                    xSemaphoreGive(m_metricsMutex);
                }
                LOG_WARNING(MQTT_TAG, "Queue full, dropping new message");

                const Event evt{
                    .type = EventType::MqttQueueOverflow,
                    .payload = MqttQueueOverflowEvent{
                        .droppedCount = 1,
                        .currentQueueSize = uxQueueMessagesWaiting(m_outboundQueue)
                    },
                    .timestampMs = static_cast<std::uint64_t>(millis())
                };
                (void) m_bus.publish(evt); // TODO: handle publish failure?

                return false;
            } else if (m_mqttCfg->queueFullPolicy == MqttConfig::QueueFullPolicy::DropOldest) {
                MqttOutboundMessage *discarded{nullptr};
                if (xQueueReceive(m_outboundQueue, &discarded, 0) == pdTRUE) {
                    delete discarded;  // Free the discarded message

                    if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        m_metrics.messagesDropped++;
                        xSemaphoreGive(m_metricsMutex);
                    }
                    LOG_WARNING(MQTT_TAG, "Queue full, dropping oldest message");
                }
                if (xQueueSend(m_outboundQueue, &msg, 0) != pdTRUE) {
                    delete msg;  // Clean up on failure
                    return false;
                }
            } else {
                delete msg;  // Clean up on failure
                return false;
            }
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.currentQueueSize = uxQueueMessagesWaiting(m_outboundQueue);
            if (m_metrics.currentQueueSize > m_metrics.peakQueueSize) {
                m_metrics.peakQueueSize = m_metrics.currentQueueSize;
            }
            xSemaphoreGive(m_metricsMutex);
        }

        return true;
    }

    bool MqttService::publishSync(const std::string &topic, const std::string &payload) {
        if (!m_client.connected()) {
            return false;
        }
        return m_client.publish(topic.c_str(), payload.c_str());
    }

    MqttMetrics MqttService::getMetrics() const {
        MqttMetrics result{};
        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            result = m_metrics;
            xSemaphoreGive(m_metricsMutex);
        }
        return result;
    }

    std::size_t MqttService::getQueueSize() const {
        if (!m_outboundQueue) {
            return 0;
        }

        return uxQueueMessagesWaiting(m_outboundQueue);
    }

    HealthStatus MqttService::getHealth() const {
        HealthStatus status{
            .componentName = getComponentName(),
            .lastUpdatedMs = millis(),
        };

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            status.errorCount = m_metrics.messagesFailed + m_metrics.connectFailures;

            if (m_metrics.isConnected) {
                status.state = HealthState::Healthy;
                status.message = "Connected";
            } else if (m_wifiConnected.load()) {
                status.state = HealthState::Degraded;
                status.message = "WiFi OK, MQTT disconnected";
            } else {
                status.state = HealthState::Unhealthy;
                status.message = "Disconnected";
            }

            status.uptimeMs = m_metrics.totalConnectedMs;
            xSemaphoreGive(m_metricsMutex);
        } else {
            status.state = HealthState::Unknown;
            status.message = "Unable to read state";
        }

        return status;
    }

    bool MqttService::performHealthCheck() {
        return m_connected.load();
    }

    void MqttService::onEvent(const Event &event) {
        switch (event.type) {
            case EventType::ConfigUpdated: {
                if (const auto *ce = std::get_if<ConfigUpdatedEvent>(&event.payload)) {
                    if (ce->config) {
                        m_cfg = ce->config;
                        m_mqttCfg = &ce->config->mqtt;
                        LOG_INFO(MQTT_TAG, "Config updated");
                    }
                }
                break;
            }
            case EventType::AttendanceRecorded: {
                if (const auto *rec = std::get_if<AttendanceRecord>(&event.payload)) {
                    handleAttendanceEvent(*rec);
                }
                break;
            }
            case EventType::OtaStateChanged: {
                if (const auto *ota = std::get_if<OtaStateChangedEvent>(&event.payload)) {
                    handleOtaStateChanged(*ota);
                }
                break;
            }
            case EventType::OtaProgress: {
                if (const auto *prog = std::get_if<OtaProgressEvent>(&event.payload)) {
                    handleOtaProgress(*prog);
                }
                break;
            }
            case EventType::MqttMessageReceived: {
                if (const auto *msg = std::get_if<MqttMessageEvent>(&event.payload)) {
                    if (msg->topic == "health/report") {
                        handleHealthReport(msg->payload);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    void MqttService::mqttTaskThunk(void *arg) {
        static_cast<MqttService *>(arg)->mqttTask();
    }

    void MqttService::mqttTask() {
        LOG_DEBUG(MQTT_TAG, "MQTT task started");

        while (m_running.load()) {
            ensureWifiConnected();

            if (!m_client.connected()) {
                handleDisconnect();

                const auto now{millis()};
                if (const auto backoff{calculateBackoff()}; now - m_lastReconnectAttempt >= backoff) {
                    m_lastReconnectAttempt = now;
                    connectMqtt();
                }
            } else {
                m_client.loop();
                processOutboundQueue();
                m_consecutiveFailures = 0;

                if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    m_metrics.totalConnectedMs += MQTT_TASK_LOOP_MS;
                    xSemaphoreGive(m_metricsMutex);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(MQTT_TASK_LOOP_MS));
        }

        LOG_DEBUG(MQTT_TAG, "MQTT task exiting");
        vTaskDelete(nullptr);
    }

    void MqttService::ensureWifiConnected() {
        if (WiFi.status() == WL_CONNECTED) {
            m_wifiConnected.store(true);
            return;
        }

        m_wifiConnected.store(false);

        if (!m_cfg) {
            return;
        }

        LOG_INFO(MQTT_TAG, "Connecting WiFi to SSID: %s", m_cfg->wifi.ssid.c_str());

        if (m_powerService && !m_wakeLock.isValid()) {
            m_wakeLock = m_powerService->requestWakeLock("mqtt_connect");
        }

        WiFiClass::mode(WIFI_STA); // Set WiFi to station mode
        WiFi.begin(m_cfg->wifi.ssid.c_str(), m_cfg->wifi.password.c_str());

        for (std::uint8_t i = 0; i < WIFI_CONNECT_RETRIES; ++i) {
            if (WiFi.status() == WL_CONNECTED) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_DELAY_MS));
        }

        if (WiFi.status() == WL_CONNECTED) {
            m_wifiConnected.store(true);
            LOG_INFO(MQTT_TAG, "WiFi connected, IP: %s", WiFi.localIP().toString().c_str());
        } else {
            LOG_WARNING(MQTT_TAG, "WiFi connection timeout");
        }
    }

    void MqttService::connectMqtt() {
        if (!m_cfg || !m_mqttCfg) {
            return;
        }

        if (m_powerService && !m_wakeLock.isValid()) {
            m_wakeLock = m_powerService->requestWakeLock("mqtt_connect");
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.connectAttempts++;
            xSemaphoreGive(m_metricsMutex);
        }

        LOG_INFO(MQTT_TAG, "Connecting to MQTT %s:%u", m_mqttCfg->broker.c_str(), unsigned{m_mqttCfg->port});
        m_client.setServer(m_mqttCfg->broker.c_str(), m_mqttCfg->port);

        const auto clientId{String(m_cfg->device.deviceId.c_str())};
        auto connected{false};

        if (!m_mqttCfg->username.empty()) {
            connected = m_client.connect(clientId.c_str(), m_mqttCfg->username.c_str(), m_mqttCfg->password.c_str()
            );
        } else {
            connected = m_client.connect(clientId.c_str());
        }

        if (!connected) {
            m_consecutiveFailures++;

            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.connectFailures++;
                xSemaphoreGive(m_metricsMutex);
            }

            LOG_WARNING(MQTT_TAG, "MQTT connect failed, rc=%d, failures=%u", m_client.state(), unsigned{m_consecutiveFailures});
            return;
        }

        m_connected.store(true);
        m_connectionStartMs = millis();

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.isConnected = true;
            m_metrics.lastConnectedMs = millis();
            xSemaphoreGive(m_metricsMutex);
        }

        LOG_INFO(MQTT_TAG, "MQTT connected");

        // Subscribe to topics
        m_client.subscribe(topicConfigSet().c_str());
        m_client.subscribe(topicOtaSet().c_str());
        m_client.subscribe((topicModules() + "/+/set").c_str());

        // Publish online status
        publishStatus("online");

        // Emit connected event
        const Event evt{
            .type = EventType::MqttConnected,
            .payload = std::monostate{},
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void) m_bus.publish(evt, pdMS_TO_TICKS(100)); // TODO: handle publish failure?

        if (m_wakeLock.isValid() && m_powerService) {
            m_powerService->releaseWakeLock(m_wakeLock);
        }
    }

    void MqttService::handleDisconnect() {
        if (const bool wasConnected{m_connected.exchange(false)}; wasConnected) {
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.isConnected = false;
                m_metrics.lastDisconnectedMs = millis();
                m_metrics.disconnects++;
                xSemaphoreGive(m_metricsMutex);
            }

            LOG_WARNING(MQTT_TAG, "MQTT disconnected");

            const Event evt{
                .type = EventType::MqttDisconnected,
                .payload = std::monostate{},
                .timestampMs = static_cast<std::uint64_t>(millis())
            };
            (void) m_bus.publish(evt, pdMS_TO_TICKS(100)); // TODO: handle publish failure?
        }
    }

    std::uint32_t MqttService::calculateBackoff() const {
        if (!m_mqttCfg) {
            return 5000;
        }

        const auto minBackoff{m_mqttCfg->reconnectBackoffMinMs};
        const auto maxBackoff{m_mqttCfg->reconnectBackoffMaxMs};
        const auto multiplier{ m_mqttCfg->reconnectBackoffMultiplier};

        auto backoff{static_cast<std::uint32_t>(minBackoff * std::pow(multiplier, m_consecutiveFailures))};

        if (backoff > maxBackoff) {
            backoff = maxBackoff;
        }

        const auto jitter{(random(0, 20) - 10) * backoff / 100};
        return backoff + jitter;
    }

    void MqttService::onMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
        const std::string t{topic};
        const std::string p{reinterpret_cast<char *>(payload), length};

        LOG_INFO(MQTT_TAG, "Message on %s: %s", t.c_str(), p.c_str());

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.messagesReceived++;
            xSemaphoreGive(m_metricsMutex);
        }

        if (t == topicConfigSet()) {
            const Event evt{
                .type = EventType::MqttMessageReceived,
                .payload = MqttMessageEvent{t, p},
                .timestampMs = static_cast<std::uint64_t>(millis())
            };
            (void) m_bus.publish(evt, pdMS_TO_TICKS(100)); // TODO: handle publish failure?
        } else if (t == topicOtaSet()) {
            // Forward OTA command to OtaModule via MqttMessageReceived event
            // OtaModule handles full JSON parsing with action, url, version, sha256, etc.
            LOG_INFO(MQTT_TAG, "OTA command received, forwarding to OtaModule");

            const Event evt{
                .type = EventType::MqttMessageReceived,
                .payload = MqttMessageEvent{t, p},
                .timestampMs = static_cast<std::uint64_t>(millis()),
                .priority = EventPriority::E_HIGH
            };
            (void) m_bus.publish(evt, pdMS_TO_TICKS(100));  // TODO: handle publish failure?
        }
    }

    void MqttService::processOutboundQueue() {
        if (!m_outboundQueue || !m_client.connected()) {
            return;
        }

        for (std::uint8_t i = 0; i < 10; ++i) {
            MqttOutboundMessage *msg{nullptr};
            if (xQueueReceive(m_outboundQueue, &msg, 0) != pdTRUE || !msg) {
                break;
            }

            if (!sendMessage(*msg)) {
                if (m_client.connected()) {
                    if (const auto age = millis() - msg->enqueuedMs; age < 30000) {
                        // Re-queue for retry
                        if (xQueueSendToFront(m_outboundQueue, &msg, 0) != pdTRUE) {
                            delete msg;  // Failed to re-queue, clean up
                        }
                        // Don't delete msg here - it's back in the queue
                    } else {
                        delete msg;  // Message too old, discard

                        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            m_metrics.messagesDropped++;
                            xSemaphoreGive(m_metricsMutex);
                        }
                    }
                } else {
                    delete msg;  // Not connected, discard
                }
            } else {
                delete msg;  // Successfully sent, clean up
            }
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.currentQueueSize = uxQueueMessagesWaiting(m_outboundQueue);
            xSemaphoreGive(m_metricsMutex);
        }
    }

    bool MqttService::sendMessage(const MqttOutboundMessage &msg) {
        const bool success{m_client.publish(msg.topic.c_str(), msg.payload.c_str(), msg.retained)};

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (success) {
                m_metrics.messagesPublished++;
                m_metrics.lastPublishMs = millis();
            } else {
                m_metrics.messagesFailed++;
            }
            xSemaphoreGive(m_metricsMutex);
        }

        return success;
    }

    std::string MqttService::makeBaseTopic() const {
        if (!m_cfg) {
            return "device/unknown";
        }
        return m_mqttCfg->baseTopic + "/" + m_cfg->device.deviceId;
    }

    std::string MqttService::topicConfigSet() const {
        return makeBaseTopic() + "/config/set";
    }
    std::string MqttService::topicConfigStatus() const {
        return makeBaseTopic() + "/config/status";
    }
    std::string MqttService::topicOtaSet() const {
        return makeBaseTopic() + "/ota/set";
    }
    std::string MqttService::topicOtaStatus() const {
        return makeBaseTopic() + "/ota/status";
    }
    std::string MqttService::topicOtaProgress() const {
        return makeBaseTopic() + "/ota/progress";
    }
    std::string MqttService::topicOtaError() const {
        return makeBaseTopic() + "/ota/error";
    }
    std::string MqttService::topicAttendance() const {
        return makeBaseTopic() + "/attendance";
    }
    std::string MqttService::topicAttendanceBatch() const {
        return makeBaseTopic() + "/attendance/batch";
    }
    std::string MqttService::topicStatus() const {
        return makeBaseTopic() + "/status";
    }
    std::string MqttService::topicHealth() const {
        return makeBaseTopic() + "/status/health";
    }
    std::string MqttService::topicPn532Status() const {
        return makeBaseTopic() + "/status/pn532";
    }
    std::string MqttService::topicMetrics() const {
        return makeBaseTopic() + "/metrics";
    }
    std::string MqttService::topicModules() const {
        return makeBaseTopic() + "/modules";
    }

    void MqttService::publishStatus(const char *status) {
        JsonDocument doc{};
        doc["status"] = status;
        doc["ip"] = WiFi.localIP().toString();
        doc["device_id"] = m_cfg ? m_cfg->device.deviceId : "unknown";
        doc["firmware"] = m_cfg ? m_cfg->device.firmwareVersion : "unknown";
        doc["uptime_s"] = millis() / 1000;

        std::string payload{};
        serializeJson(doc, payload);

        (void) publishAsync(topicStatus(), payload, true); // TODO: handle failure?
    }

    void MqttService::publishHealth(const std::string &healthJson) {
        (void) publishAsync(topicHealth(), healthJson); // TODO: handle failure?
    }

    void MqttService::handleAttendanceEvent(const AttendanceRecord &record) {
        char cardStr[CARD_ID_SIZE * 2 + 1];
        std::uint32_t pos = 0;
        for (const auto byte: record.cardId) {
            pos += snprintf(cardStr + pos, sizeof(cardStr) - pos, "%02X", byte);
        }

        JsonDocument doc{};
        doc["card_id"] = cardStr;
        doc["timestamp_ms"] = record.timestampMs;
        doc["device_id"] = record.deviceId;
        doc["location_id"] = record.locationId;
        doc["sequence"] = record.sequenceNumber;

        std::string payload{};
        serializeJson(doc, payload);

        (void) publishAsync(topicAttendance(), payload); // TODO: handle failure?
    }

    void MqttService::handleOtaStateChanged(const OtaStateChangedEvent &event) {
        JsonDocument doc{};

        // State info
        doc["old_state"] = toString(static_cast<OtaState>(event.oldState));
        doc["new_state"] = toString(static_cast<OtaState>(event.newState));
        doc["message"] = event.message;
        doc["timestamp_ms"] = event.timestampMs;

        // Include device info for context
        if (m_cfg) {
            doc["device_id"] = m_cfg->device.deviceId;
            doc["firmware_version"] = m_cfg->device.firmwareVersion;
        }

        std::string payload{};
        serializeJson(doc, payload);

        // Publish to status topic (retained so new subscribers get current state)
        (void) publishAsync(topicOtaStatus(), payload, true); // TODO: handle failure?

        // Also publish to error topic if state is Failed
        if (const auto newState = static_cast<OtaState>(event.newState); newState == OtaState::Failed) {
            JsonDocument errorDoc{};
            errorDoc["error"] = event.message;
            errorDoc["timestamp_ms"] = event.timestampMs;
            if (m_cfg) {
                errorDoc["device_id"] = m_cfg->device.deviceId;
            }

            std::string errorPayload{};
            serializeJson(errorDoc, errorPayload);
            (void) publishAsync(topicOtaError(), errorPayload); // TODO: handle failure?
        }
    }

    void MqttService::handleOtaProgress(const OtaProgressEvent &event) {
        JsonDocument doc{};
        doc["percent"] = event.percent;
        doc["bytes_downloaded"] = event.bytesDownloaded;
        doc["total_bytes"] = event.totalBytes;
        doc["success"] = event.success;
        doc["message"] = event.message;

        // Add ETA and speed if we have enough data
        if (event.totalBytes > 0 && event.bytesDownloaded > 0) {
            // Could add estimated time remaining calculation here
            doc["remaining_bytes"] = event.totalBytes - event.bytesDownloaded;
        }

        if (m_cfg) {
            doc["device_id"] = m_cfg->device.deviceId;
        }

        std::string payload{};
        serializeJson(doc, payload);

        (void) publishAsync(topicOtaProgress(), payload); // TODO: handle failure?
    }

    void MqttService::handleHealthReport(const std::string &payload) {
        (void) publishAsync(topicHealth(), payload); // TODO: handle failure?
    }
}
