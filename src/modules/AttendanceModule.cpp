#include "modules/AttendanceModule.hpp"
#include "services/UserFeedbackService.hpp"
#include "services/AttendanceBatcher.hpp"
#include "core/Logger.hpp"

#include <memory>

namespace isic {
    namespace {
        constexpr auto *ATT_TAG{"Attendance"};
        constexpr auto *ATT_TASK_NAME{"attendance"};
        constexpr std::uint32_t ATT_TASK_LOOP_MS{50};
    }

    AttendanceModule::AttendanceModule(EventBus &bus, Pn532Driver &pn532, PowerService &powerService) : m_bus(bus) , m_pn532(pn532), m_powerService(powerService) {
        m_offlineMutex = xSemaphoreCreateMutex();
        m_metricsMutex = xSemaphoreCreateMutex();
    }

    AttendanceModule::~AttendanceModule() {
        AttendanceModule::stop();

        if (m_cardQueue) {
            vQueueDelete(m_cardQueue);
            m_cardQueue = nullptr;
        }

        if (m_offlineMutex) {
            vSemaphoreDelete(m_offlineMutex);
            m_offlineMutex = nullptr;
        }

        if (m_metricsMutex) {
            vSemaphoreDelete(m_metricsMutex);
            m_metricsMutex = nullptr;
        }

        if (m_subscriptionId != 0) {
            m_bus.unsubscribe(m_subscriptionId);
            m_subscriptionId = 0;
        }
    }

    Status AttendanceModule::begin(const AppConfig &cfg) {
        m_cfg = &cfg;
        m_attCfg = &cfg.attendance;
        m_startTimeMs = millis();

        // Create card event queue with configured size
        const auto queueSize{m_attCfg->eventQueueSize > 0 ? m_attCfg->eventQueueSize : CARD_QUEUE_SIZE};

        m_cardQueue = xQueueCreate(queueSize, sizeof(CardScannedEvent));
        if (!m_cardQueue) {
            LOG_ERROR(ATT_TAG, "Failed to create card queue");
            return Status::Error(ErrorCode::ResourceExhausted, "Queue creation failed");
        }

        // Initialize offline buffer
        m_offlineBuffer.resize(m_attCfg->offlineBufferSize);
        m_offlineHead = 0;
        m_offlineTail = 0;

        // Subscribe to relevant events (after object is fully constructed)
        m_subscriptionId = m_bus.subscribe(
            this,
            EventFilter::only(EventType::CardScanned)
            .include(EventType::CardReadError)
            .include(EventType::MqttConnected)
            .include(EventType::MqttDisconnected)
            .include(EventType::ConfigUpdated)
        );

        setState(ModuleState::Initialized);

        LOG_INFO(ATT_TAG, "AttendanceModule initialized: debounce=%ums, buffer=%u, batching=%s", unsigned{m_attCfg->debounceMs}, unsigned{m_attCfg->offlineBufferSize}, m_attCfg->batchingEnabled ? "on" : "off");
        return Status::OK();
    }

    void AttendanceModule::start() {
        if (m_running.load()) {
            return;
        }

        setState(ModuleState::Starting);
        m_running.store(true);

        xTaskCreatePinnedToCore(
            &AttendanceModule::processingTaskThunk,
            ATT_TASK_NAME,
            m_attCfg ? m_attCfg->taskStackSize : 4096,
            this,
            m_attCfg ? m_attCfg->taskPriority : 3,
            &m_taskHandle,
            1
        );

        setState(ModuleState::Running);
        LOG_INFO(ATT_TAG, "AttendanceModule started");
    }

    void AttendanceModule::stop() {
        if (!m_running.load()) {
            return;
        }

        setState(ModuleState::Stopping);
        m_running.store(false);

        if (m_processingWakeLock.isValid()) {
            m_powerService.releaseWakeLock(m_processingWakeLock);
        }

        if (m_taskHandle) {
            vTaskDelay(pdMS_TO_TICKS(200));
            m_taskHandle = nullptr;
        }

        setState(ModuleState::Stopped);
        LOG_INFO(ATT_TAG, "AttendanceModule stopped");
    }

    void AttendanceModule::handleEvent(const Event &event) {
        switch (event.type) {
            case EventType::CardScanned: {
                if (const auto *cs = std::get_if<CardScannedEvent>(&event.payload)) {
                    onCardScanned(*cs);
                }
                break;
            }
            case EventType::CardReadError: {
                if (const auto *err = std::get_if<CardReadErrorEvent>(&event.payload)) {
                    onCardReadError(*err);
                }
                break;
            }
            case EventType::MqttConnected:
                onMqttConnected();
                break;
            case EventType::MqttDisconnected:
                onMqttDisconnected();
                break;
            case EventType::ConfigUpdated: {
                if (const auto *ce = std::get_if<ConfigUpdatedEvent>(&event.payload)) {
                    if (ce->config) {
                        handleConfigUpdate(*ce->config);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    void AttendanceModule::handleConfigUpdate(const AppConfig &config) {
        m_cfg = &config;
        m_attCfg = &config.attendance;

        // Resize offline buffer if needed
        if (xSemaphoreTake(m_offlineMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (m_offlineBuffer.size() != m_attCfg->offlineBufferSize) {
                m_offlineBuffer.resize(m_attCfg->offlineBufferSize);
                m_offlineHead = 0;
                m_offlineTail = 0;
            }
            xSemaphoreGive(m_offlineMutex);
        }

        LOG_INFO(ATT_TAG, "Config updated: debounce=%ums, buffer=%u", unsigned{m_attCfg->debounceMs}, unsigned{m_attCfg->offlineBufferSize});
    }

    HealthStatus AttendanceModule::getHealth() const {
        HealthStatus status{};
        status.componentName = getComponentName();
        status.lastUpdatedMs = millis();

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            status.errorCount = m_metrics.cardsDroppedDebounce + m_metrics.cardsDroppedQueueFull;

            if (m_metrics.highLoadDetected) {
                status.state = HealthState::Degraded;
                status.message = "High load detected";
            } else if (!m_running.load()) {
                status.state = HealthState::Unhealthy;
                status.message = "Not running";
            } else {
                status.state = HealthState::Healthy;
                status.message = "Processing normally";
            }

            status.uptimeMs = millis() - m_startTimeMs;
            xSemaphoreGive(m_metricsMutex);
        } else {
            status.state = HealthState::Unknown;
            status.message = "Unable to read state";
        }

        return status;
    }

    bool AttendanceModule::performHealthCheck() {
        checkHighLoad();
        return m_running.load();
    }

    AttendanceMetrics AttendanceModule::getMetrics() const {
        AttendanceMetrics result{};

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            result = m_metrics;
            xSemaphoreGive(m_metricsMutex);
        }

        return result;
    }

    std::uint32_t AttendanceModule::getOfflineBufferCount() const {
        if (xSemaphoreTake(m_offlineMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return 0;
        }

        std::uint32_t count = 0;
        for (const auto &entry: m_offlineBuffer) {
            if (entry.pending) {
                ++count;
            }
        }

        xSemaphoreGive(m_offlineMutex);
        return count;
    }

    void AttendanceModule::flushOfflineBuffer() {
        LOG_INFO(ATT_TAG, "Flushing offline buffer");
        tryFlushOfflineBuffer();
    }

    bool AttendanceModule::isHighLoad() const {
        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            const auto result{m_metrics.highLoadDetected};
            xSemaphoreGive(m_metricsMutex);
            return result;
        }

        return false;
    }

    void AttendanceModule::onCardScanned(const CardScannedEvent &event) {
        if (!m_cardQueue) {
            return;
        }

        // Non-blocking enqueue
        if (xQueueSend(m_cardQueue, &event, 0) != pdTRUE) {
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.cardsDroppedQueueFull++;
                xSemaphoreGive(m_metricsMutex);
            }

            LOG_WARNING(ATT_TAG, "Card queue full, event dropped");
            signalError();

            auto evt = std::make_unique<Event>(Event{
                .type = EventType::QueueOverflow,
                .payload = QueueOverflowEvent{
                    .queueName = "card_queue",
                    .droppedItems = 1,
                    .queueCapacity = CARD_QUEUE_SIZE
                },
                .timestampMs = static_cast<std::uint64_t>(millis())
            });
            (void) m_bus.publish(std::move(evt)); // TODO: check publish result
        } else {
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.eventsQueued++;
                m_metrics.currentQueueSize = uxQueueMessagesWaiting(m_cardQueue);
                if (m_metrics.currentQueueSize > m_metrics.peakQueueSize) {
                    m_metrics.peakQueueSize = m_metrics.currentQueueSize;
                }
                xSemaphoreGive(m_metricsMutex);
            }
        }
    }

    void AttendanceModule::onCardReadError(const CardReadErrorEvent &event) {
        LOG_WARNING(ATT_TAG, "Card read error: %s", toString(event.error));
        signalError();
    }

    void AttendanceModule::onMqttConnected() {
        m_mqttConnected.store(true);
        LOG_INFO(ATT_TAG, "MQTT connected, flushing offline buffer");
        tryFlushOfflineBuffer();
    }

    void AttendanceModule::onMqttDisconnected() {
        m_mqttConnected.store(false);
        LOG_INFO(ATT_TAG, "MQTT disconnected, buffering locally");
    }

    void AttendanceModule::processingTaskThunk(void *arg) {
        static_cast<AttendanceModule *>(arg)->processingTask();
    }

    void AttendanceModule::processingTask() {
        LOG_DEBUG(ATT_TAG, "Processing task started");

        CardScannedEvent event{};

        while (m_running.load()) {
            if (xQueueReceive(m_cardQueue, &event, pdMS_TO_TICKS(ATT_TASK_LOOP_MS)) == pdTRUE) {
                // Acquire wake lock for processing
                if (!m_processingWakeLock.isValid()) {
                    m_processingWakeLock = m_powerService.requestWakeLock("attendance_process");
                }

                processCard(event);

                if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    m_metrics.currentQueueSize = uxQueueMessagesWaiting(m_cardQueue);
                    xSemaphoreGive(m_metricsMutex);
                }
            } else {
                // No events - release wake lock if queue is empty
                if (m_processingWakeLock.isValid() &&
                    uxQueueMessagesWaiting(m_cardQueue) == 0) {
                    m_powerService.releaseWakeLock(m_processingWakeLock);
                }
            }

            checkHighLoad();

            // Try to flush offline buffer if MQTT is connected
            if (m_mqttConnected.load() && hasOfflineRecords()) {
                tryFlushOfflineBuffer();
            }
        }

        if (m_processingWakeLock.isValid()) {
            m_powerService.releaseWakeLock(m_processingWakeLock);
        }

        LOG_DEBUG(ATT_TAG, "Processing task exiting");
        vTaskDelete(nullptr);
    }

    bool AttendanceModule::shouldProcessCard(const CardId &cardId, std::uint64_t timestamp) {
        if (!m_attCfg) {
            return true;
        }

        for (auto &[id, lastSeenMs, valid]: m_recentCards) {
            if (valid && id == cardId) {
                if (const auto elapsed = timestamp - lastSeenMs; elapsed < m_attCfg->debounceMs) {
                    return false;
                }
                lastSeenMs = timestamp;
                return true;
            }
        }

        auto &[id, lastSeenMs, valid]{m_recentCards[m_recentCardsIndex]};
        id = cardId;
        lastSeenMs = timestamp;
        valid = true;

        m_recentCardsIndex = (m_recentCardsIndex + 1) % DEBOUNCE_CACHE_SIZE;

        return true;
    }

    void AttendanceModule::processCard(const CardScannedEvent &event) {
        const auto now{millis()};

        if (!shouldProcessCard(event.cardId, event.timestampMs)) {
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.cardsDroppedDebounce++;
                xSemaphoreGive(m_metricsMutex);
            }
            LOG_DEBUG(ATT_TAG, "Card debounced");
            return;
        }

        // Build attendance record
        AttendanceRecord record{};
        record.cardId = event.cardId;
        record.timestampMs = event.timestampMs;
        record.sequenceNumber = m_sequenceNumber.fetch_add(1);

        if (m_cfg) {
            record.deviceId = m_cfg->device.deviceId;
            record.locationId = m_cfg->device.locationId;
        }

        // Update metrics
        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.totalCardsProcessed++;
            m_metrics.lastCardProcessedMs = now;
            xSemaphoreGive(m_metricsMutex);
        }

        // Signal success feedback
        signalSuccess();

        LOG_INFO(ATT_TAG, "Card processed: %02X%02X%02X%02X%02X%02X%02X seq=%u", record.cardId[0], record.cardId[1], record.cardId[2], record.cardId[3], record.cardId[4], record.cardId[5], record.cardId[6], unsigned{record.sequenceNumber});
        enqueueAttendance(record);
    }

    void AttendanceModule::enqueueAttendance(const AttendanceRecord &record) {
        // If batching is enabled and batcher is available, use it
        if (m_attCfg && m_attCfg->batchingEnabled && m_batcher) {
            LOG_DEBUG(ATT_TAG, "Sending to batcher: seq=%u", unsigned{record.sequenceNumber});
            if (m_batcher->addRecord(record)) {
                if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    m_metrics.eventsBatched++;
                    xSemaphoreGive(m_metricsMutex);
                }
                LOG_DEBUG(ATT_TAG, "Record batched successfully");
                return;
            }
            LOG_WARNING(ATT_TAG, "Batcher rejected record, falling back to direct publish");
            // Fall through to direct publish if batching fails
        } else {
            LOG_DEBUG(ATT_TAG, "Batching disabled or no batcher, using direct publish");
        }

        // Direct publish mode
        if (m_mqttConnected.load()) {
            publishAttendance(record);
        } else {
            bufferForOffline(record);
        }
    }

    void AttendanceModule::publishAttendance(const AttendanceRecord &record) {
        auto evt = std::make_unique<Event>(Event{
            .type = EventType::AttendanceRecorded,
            .payload = record,
            .timestampMs = static_cast<std::uint64_t>(millis())
        });

        if (m_bus.publish(std::move(evt))) {
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.eventsPublished++;
                m_metrics.lastEventPublishedMs = millis();
                xSemaphoreGive(m_metricsMutex);
            }
        } else {
            LOG_WARNING(ATT_TAG, "Failed to publish attendance event");
            bufferForOffline(record);
        }
    }

    void AttendanceModule::bufferForOffline(const AttendanceRecord &record) {
        if (xSemaphoreTake(m_offlineMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            LOG_ERROR(ATT_TAG, "Failed to acquire offline buffer mutex");
            return;
        }

        auto &[attendanceRecord, pending, sendAttempts] = m_offlineBuffer[m_offlineTail];

        if (pending) {
            LOG_WARNING(ATT_TAG, "Offline buffer full, overwriting oldest record");
        }

        attendanceRecord = record;
        pending = true;
        sendAttempts = 0;

        m_offlineTail = (m_offlineTail + 1) % m_offlineBuffer.size();

        xSemaphoreGive(m_offlineMutex);
        LOG_DEBUG(ATT_TAG, "Buffered for offline: seq=%u", unsigned{record.sequenceNumber});
    }

    bool AttendanceModule::hasOfflineRecords() const {
        if (xSemaphoreTake(m_offlineMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return false;
        }

        auto hasPending{false};
        for (const auto &entry: m_offlineBuffer) {
            if (entry.pending) {
                hasPending = true;
                break;
            }
        }

        xSemaphoreGive(m_offlineMutex);
        return hasPending;
    }

    void AttendanceModule::tryFlushOfflineBuffer() {
        if (!m_mqttConnected.load()) {
            return;
        }

        if (xSemaphoreTake(m_offlineMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return;
        }

        std::uint32_t flushed = 0;

        for (auto &[record, pending, sendAttempts]: m_offlineBuffer) {
            if (!pending) {
                continue;
            }

            if (sendAttempts >= MAX_SEND_ATTEMPTS) {
                pending = false;
                LOG_WARNING(ATT_TAG, "Dropping offline record after %u attempts", unsigned{MAX_SEND_ATTEMPTS});
                continue;
            }

            auto evt = std::make_unique<Event>(Event{
                .type = EventType::AttendanceRecorded,
                .payload = record,
                .timestampMs = static_cast<std::uint64_t>(millis())
            });

            xSemaphoreGive(m_offlineMutex);

            const bool published{m_bus.publish(std::move(evt))};

            if (xSemaphoreTake(m_offlineMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                return;
            }

            if (published) {
                pending = false;
                ++flushed;

                if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    m_metrics.eventsPublished++;
                    xSemaphoreGive(m_metricsMutex);
                }
            } else {
                sendAttempts++;
            }

            if (flushed >= 10) {
                break;
            }
        }

        xSemaphoreGive(m_offlineMutex);

        if (flushed > 0) {
            LOG_INFO(ATT_TAG, "Flushed %u offline records", unsigned{flushed});
        }
    }

    void AttendanceModule::checkHighLoad() {
        if (!m_attCfg || !m_cardQueue) {
            return;
        }

        const auto queueSize{uxQueueMessagesWaiting(m_cardQueue)};
        const auto highLoad{queueSize >= m_attCfg->queueHighWatermark};

        auto wasHighLoad{false};
        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            wasHighLoad = m_metrics.highLoadDetected;
            m_metrics.highLoadDetected = highLoad;
            m_metrics.currentQueueSize = queueSize;
            xSemaphoreGive(m_metricsMutex);
        }

        if (highLoad && !wasHighLoad) {
            emitHighLoadEvent();
        }
    }

    void AttendanceModule::emitHighLoadEvent() {
        LOG_WARNING(ATT_TAG, "High load detected, queue size: %u/%u", unsigned{m_metrics.currentQueueSize}, unsigned{CARD_QUEUE_SIZE});

        auto evt = std::make_unique<Event>(Event{
            .type = EventType::HighLoadDetected,
            .payload = HighLoadEvent{
                .source = "attendance_queue",
                .currentLoad = m_metrics.currentQueueSize,
                .threshold = m_attCfg ? m_attCfg->queueHighWatermark : 0,
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis()),
            .priority = EventPriority::E_HIGH
        });
        (void) m_bus.publish(std::move(evt)); // TODO: check publish result
    }

    void AttendanceModule::signalSuccess() {
        if (m_feedback) {
            m_feedback->signalSuccess();
        } else {
            // Publish feedback request event as fallback
            auto evt = std::make_unique<Event>(Event{
                .type = EventType::FeedbackRequested,
                .payload = FeedbackRequestEvent{
                    .signal = FeedbackSignal::Success,
                    .repeatCount = 1
                },
                .timestampMs = static_cast<std::uint64_t>(millis())
            });
            (void) m_bus.publish(std::move(evt)); // TODO: check publish result
        }
    }

    void AttendanceModule::signalError() {
        if (m_feedback) {
            m_feedback->signalError();
        } else {
            auto evt = std::make_unique<Event>(Event{
                .type = EventType::FeedbackRequested,
                .payload = FeedbackRequestEvent{
                    .signal = FeedbackSignal::Error,
                    .repeatCount = 1
                },
                .timestampMs = static_cast<std::uint64_t>(millis())
            });
            (void) m_bus.publish(std::move(evt)); // TODO: check publish result
        }
    }
}
