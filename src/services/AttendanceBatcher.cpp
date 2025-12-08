#include "services/AttendanceBatcher.hpp"
#include "services/MqttService.hpp"
#include "services/PowerService.hpp"
#include "core/Logger.hpp"

#include <ArduinoJson.h>
#include <memory>

namespace isic {
    namespace {
        constexpr auto* BATCHER_TAG = "Batcher";
        constexpr std::uint32_t TASK_LOOP_MS = 100;
    }

    AttendanceBatcher::AttendanceBatcher(EventBus& bus) : m_bus(bus) {
        m_batchMutex = xSemaphoreCreateMutex();
        m_pendingMutex = xSemaphoreCreateMutex();
        m_metricsMutex = xSemaphoreCreateMutex();

        m_subscriptionId = m_bus.subscribe(this,
            EventFilter::only(EventType::ConfigUpdated)
                .include(EventType::AttendanceRecorded)
                .include(EventType::MqttConnected)
                .include(EventType::MqttDisconnected)
                .include(EventType::SleepEntering));
    }

    AttendanceBatcher::~AttendanceBatcher() {
        stop();
        m_bus.unsubscribe(m_subscriptionId);

        if (m_batchMutex) {
            vSemaphoreDelete(m_batchMutex);
            m_batchMutex = nullptr;
        }
        if (m_pendingMutex) {
            vSemaphoreDelete(m_pendingMutex);
            m_pendingMutex = nullptr;
        }
        if (m_metricsMutex) {
            vSemaphoreDelete(m_metricsMutex);
            m_metricsMutex = nullptr;
        }
        if (m_recordQueue) {
            // Drain and delete any remaining records in the queue
            AttendanceRecord* rec{nullptr};
            while (xQueueReceive(m_recordQueue, &rec, 0) == pdTRUE) {
                delete rec;
            }
            vQueueDelete(m_recordQueue);
            m_recordQueue = nullptr;
        }
    }

    Status AttendanceBatcher::begin(const AppConfig& cfg,
                                     MqttService& mqtt,
                                     PowerService& power) {
        m_cfg = &cfg.attendance;
        m_appCfg = &cfg;
        m_mqtt = &mqtt;
        m_power = &power;
        m_enabled.store(m_cfg->batchingEnabled);

        // Create record queue (stores AttendanceRecord* pointers to avoid std::string corruption)
        m_recordQueue = xQueueCreate(RECORD_QUEUE_SIZE, sizeof(AttendanceRecord*));
        if (!m_recordQueue) {
            return Status::Error(ErrorCode::ResourceExhausted, "Queue creation failed");
        }

        // Start batcher task
        m_running.store(true);
        xTaskCreatePinnedToCore(
            &AttendanceBatcher::batcherTaskThunk,
            "batcher",
            4096,
            this,
            2,
            &m_taskHandle,
            1
        );

        LOG_INFO(BATCHER_TAG, "AttendanceBatcher started: maxBatch=%u, flushInterval=%us",
                 static_cast<unsigned>(m_cfg->batchMaxSize),
                 m_cfg->batchFlushIntervalMs / 1000);

        return Status::OK();
    }

    void AttendanceBatcher::stop() {
        m_running.store(false);

        // Flush remaining records
        flush();

        if (m_wakeLock.isValid() && m_power) {
            m_power->releaseWakeLock(m_wakeLock);
        }

        if (m_taskHandle) {
            vTaskDelay(pdMS_TO_TICKS(200));
            m_taskHandle = nullptr;
        }
    }

    bool AttendanceBatcher::addRecord(const AttendanceRecord& record) {
        if (!m_enabled.load()) {
            LOG_WARNING(BATCHER_TAG, "Batcher disabled, rejecting record");
            return false;
        }
        
        if (!m_recordQueue) {
            LOG_WARNING(BATCHER_TAG, "Record queue not initialized");
            return false;
        }

        // Heap-allocate the record to avoid std::string corruption through FreeRTOS queue memcpy
        auto* rec = new (std::nothrow) AttendanceRecord{record};
        if (!rec) {
            LOG_ERROR(BATCHER_TAG, "Failed to allocate record");
            return false;
        }

        // Non-blocking enqueue (queue stores pointer)
        if (xQueueSend(m_recordQueue, &rec, 0) != pdTRUE) {
            delete rec;  // Clean up since we couldn't queue it
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.recordsDropped++;
                xSemaphoreGive(m_metricsMutex);
            }
            LOG_WARNING(BATCHER_TAG, "Record queue full, dropping record");
            return false;
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.recordsReceived++;
            m_metrics.lastRecordReceivedMs = millis();
            xSemaphoreGive(m_metricsMutex);
        }
        
        LOG_DEBUG(BATCHER_TAG, "Record queued: seq=%u, queue=%u",
                  static_cast<unsigned>(record.sequenceNumber),
                  static_cast<unsigned>(uxQueueMessagesWaiting(m_recordQueue)));

        return true;
    }

    void AttendanceBatcher::flush() {
        if (xSemaphoreTake(m_batchMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!m_currentBatch.isEmpty()) {
                flushCurrentBatch();
            }
            xSemaphoreGive(m_batchMutex);
        }

        // Also try to flush pending buffers
        tryFlushPendingBuffers();
    }

    void AttendanceBatcher::flushForSleep() {
        LOG_INFO(BATCHER_TAG, "Flushing for sleep");

        // Acquire wake lock to complete flush
        if (m_power && !m_wakeLock.isValid()) {
            m_wakeLock = m_power->requestWakeLock("batcher_flush");
        }

        flush();

        // Release wake lock
        if (m_wakeLock.isValid() && m_power) {
            m_power->releaseWakeLock(m_wakeLock);
        }
    }

    std::size_t AttendanceBatcher::getCurrentBatchSize() const {
        if (xSemaphoreTake(m_batchMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return 0;
        }
        const auto size = m_currentBatch.count;
        xSemaphoreGive(m_batchMutex);
        return size;
    }

    std::size_t AttendanceBatcher::getPendingBufferCount() const {
        if (xSemaphoreTake(m_pendingMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return 0;
        }
        const auto count = m_pendingCount;
        xSemaphoreGive(m_pendingMutex);
        return count;
    }

    BatcherMetrics AttendanceBatcher::getMetrics() const {
        BatcherMetrics result{};
        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            result = m_metrics;
            xSemaphoreGive(m_metricsMutex);
        }
        result.currentBatchSize = getCurrentBatchSize();
        result.pendingBufferCount = getPendingBufferCount();
        return result;
    }

    void AttendanceBatcher::updateConfig(const AttendanceConfig& cfg) {
        m_cfg = &cfg;
        m_enabled.store(cfg.batchingEnabled);
        LOG_INFO(BATCHER_TAG, "Config updated: enabled=%s, maxBatch=%u",
                 cfg.batchingEnabled ? "yes" : "no",
                 static_cast<unsigned>(cfg.batchMaxSize));
    }

    void AttendanceBatcher::setEnabled(bool enabled) {
        m_enabled.store(enabled);
        if (!enabled) {
            flush();  // Flush remaining when disabling
        }
    }

    void AttendanceBatcher::onEvent(const Event& event) {
        switch (event.type) {
            case EventType::ConfigUpdated:
                if (const auto* ce = std::get_if<ConfigUpdatedEvent>(&event.payload)) {
                    if (ce->config) {
                        updateConfig(ce->config->attendance);
                    }
                }
                break;

            case EventType::AttendanceRecorded:
                if (const auto* rec = std::get_if<AttendanceRecord>(&event.payload)) {
                    // Don't call addRecord here - the record comes from AttendanceModule
                    // which will call addRecord directly if batching is enabled
                }
                break;

            case EventType::MqttConnected:
                m_mqttConnected.store(true);
                tryFlushPendingBuffers();
                break;

            case EventType::MqttDisconnected:
                m_mqttConnected.store(false);
                break;

            case EventType::SleepEntering:
                flushForSleep();
                break;

            default:
                break;
        }
    }

    void AttendanceBatcher::batcherTaskThunk(void* arg) {
        static_cast<AttendanceBatcher*>(arg)->batcherTask();
    }

    void AttendanceBatcher::batcherTask() {
        LOG_DEBUG(BATCHER_TAG, "Batcher task started");

        AttendanceRecord* rec{nullptr};

        while (m_running.load()) {
            // Process incoming records (queue stores pointers)
            while (xQueueReceive(m_recordQueue, &rec, pdMS_TO_TICKS(TASK_LOOP_MS)) == pdTRUE) {
                if (rec) {
                    processIncomingRecord(*rec);
                    delete rec;  // Clean up after processing
                    rec = nullptr;
                }
            }

            // Check if we should flush based on time
            if (xSemaphoreTake(m_batchMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (shouldFlushBatch()) {
                    flushCurrentBatch();
                }
                xSemaphoreGive(m_batchMutex);
            }

            // Try to flush pending buffers if MQTT is connected
            if (m_mqttConnected.load()) {
                tryFlushPendingBuffers();
            }
        }

        LOG_DEBUG(BATCHER_TAG, "Batcher task exiting");
        vTaskDelete(nullptr);
    }

    void AttendanceBatcher::processIncomingRecord(const AttendanceRecord& record) {
        if (xSemaphoreTake(m_batchMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_WARNING(BATCHER_TAG, "Could not acquire batch mutex");
            return;
        }

        const auto now = millis();

        // Start new batch if empty
        if (m_currentBatch.isEmpty()) {
            m_batchStartMs = now;
            LOG_DEBUG(BATCHER_TAG, "Starting new batch");
        }

        m_lastRecordMs = now;
        
        const auto prevCount = m_currentBatch.count;
        m_currentBatch.add(record);
        
        LOG_INFO(BATCHER_TAG, "Added record to batch: count=%u->%u, seq=%u, maxSize=%u",
                 static_cast<unsigned>(prevCount),
                 static_cast<unsigned>(m_currentBatch.count),
                 static_cast<unsigned>(record.sequenceNumber),
                 m_cfg ? static_cast<unsigned>(m_cfg->batchMaxSize) : 0);

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.recordsBatched++;
            m_metrics.currentBatchSize = m_currentBatch.count;
            if (m_currentBatch.count > m_metrics.peakBatchSize) {
                m_metrics.peakBatchSize = m_currentBatch.count;
            }
            xSemaphoreGive(m_metricsMutex);
        }

        // Check if batch is full
        if (m_currentBatch.isFull() ||
            (m_cfg && m_currentBatch.count >= m_cfg->batchMaxSize)) {
            LOG_INFO(BATCHER_TAG, "Batch full (%u records), flushing now",
                     static_cast<unsigned>(m_currentBatch.count));
            flushCurrentBatch();
        }

        xSemaphoreGive(m_batchMutex);
    }

    bool AttendanceBatcher::shouldFlushBatch() const {
        if (m_currentBatch.isEmpty()) {
            return false;
        }

        const auto now = millis();

        // Flush if batch has been open too long
        if (m_cfg && now - m_batchStartMs >= m_cfg->batchFlushIntervalMs) {
            LOG_DEBUG(BATCHER_TAG, "Time flush triggered: %ums elapsed (interval=%ums)",
                      static_cast<unsigned>(now - m_batchStartMs),
                      m_cfg->batchFlushIntervalMs);
            return true;
        }

        // Flush if no new records for a while (idle flush)
        if (m_cfg && now - m_lastRecordMs >= m_cfg->batchFlushOnIdleMs) {
            LOG_DEBUG(BATCHER_TAG, "Idle flush triggered: %ums since last record (idle=%ums)",
                      static_cast<unsigned>(now - m_lastRecordMs),
                      m_cfg->batchFlushOnIdleMs);
            return true;
        }

        return false;
    }

    void AttendanceBatcher::flushCurrentBatch() {
        // Must be called with m_batchMutex held

        if (m_currentBatch.isEmpty()) {
            return;
        }

        LOG_INFO(BATCHER_TAG, "Flushing batch with %u records",
                 static_cast<unsigned>(m_currentBatch.count));
        
        // Log each record in the batch for debugging
        for (std::size_t i = 0; i < m_currentBatch.count; ++i) {
            const auto& rec = m_currentBatch.records[i];
            LOG_DEBUG(BATCHER_TAG, "  [%u] card=%02X%02X%02X%02X seq=%u",
                      static_cast<unsigned>(i),
                      rec.cardId[0], rec.cardId[1], rec.cardId[2], rec.cardId[3],
                      static_cast<unsigned>(rec.sequenceNumber));
        }

        // Try to send directly if MQTT is connected
        if (m_mqttConnected.load() && sendBatch(m_currentBatch)) {
            // Success - clear batch
            m_currentBatch.clear();
        } else {
            // Buffer for later
            bufferBatch(m_currentBatch);
            m_currentBatch.clear();
        }
    }

    bool AttendanceBatcher::sendBatch(const AttendanceBatch& batch) {
        if (!m_mqtt) {
            return false;
        }

        const auto json = serializeBatch(batch);

        // Build topic
        std::string topic;
        if (m_appCfg) {
            topic = m_appCfg->mqtt.baseTopic + "/" +
                    m_appCfg->device.deviceId + "/attendance/batch";
        } else {
            topic = "device/unknown/attendance/batch";
        }

        const bool success = m_mqtt->publishAsync(topic, json, false, 1);

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (success) {
                m_metrics.batchesSent++;
                m_metrics.lastBatchSentMs = millis();
            } else {
                m_metrics.batchesFailed++;
            }
            xSemaphoreGive(m_metricsMutex);
        }

        if (success) {
            LOG_INFO(BATCHER_TAG, "Batch sent: %u records",
                     static_cast<unsigned>(batch.count));
        } else {
            LOG_WARNING(BATCHER_TAG, "Failed to send batch");
        }

        return success;
    }

    void AttendanceBatcher::bufferBatch(const AttendanceBatch& batch) {
        if (xSemaphoreTake(m_pendingMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(BATCHER_TAG, "Could not acquire pending mutex");
            return;
        }

        if (m_pendingCount >= MAX_PENDING_BATCHES) {
            // Buffer full - drop oldest
            m_pendingHead = (m_pendingHead + 1) % MAX_PENDING_BATCHES;
            m_pendingCount--;

            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.recordsDropped += batch.count;
                xSemaphoreGive(m_metricsMutex);
            }

            LOG_WARNING(BATCHER_TAG, "Pending buffer full, dropping oldest batch");
        }

        m_pendingBuffer[m_pendingTail] = batch;
        m_pendingTail = (m_pendingTail + 1) % MAX_PENDING_BATCHES;
        m_pendingCount++;

        xSemaphoreGive(m_pendingMutex);

        LOG_DEBUG(BATCHER_TAG, "Buffered batch, pending=%u",
                  static_cast<unsigned>(m_pendingCount));
    }

    void AttendanceBatcher::tryFlushPendingBuffers() {
        if (!m_mqttConnected.load() || !m_mqtt) {
            return;
        }

        if (xSemaphoreTake(m_pendingMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return;
        }

        std::size_t flushed = 0;

        while (m_pendingCount > 0) {
            const auto& batch = m_pendingBuffer[m_pendingHead];

            // Release mutex during send to avoid blocking
            xSemaphoreGive(m_pendingMutex);

            if (!sendBatch(batch)) {
                // Send failed, stop trying
                return;
            }

            // Re-acquire mutex
            if (xSemaphoreTake(m_pendingMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
                return;
            }

            m_pendingHead = (m_pendingHead + 1) % MAX_PENDING_BATCHES;
            m_pendingCount--;
            ++flushed;

            // Limit how many we send in one go
            if (flushed >= 3) {
                break;
            }
        }

        xSemaphoreGive(m_pendingMutex);

        if (flushed > 0) {
            LOG_INFO(BATCHER_TAG, "Flushed %u pending batches",
                     static_cast<unsigned>(flushed));
        }
    }

    std::string AttendanceBatcher::serializeBatch(const AttendanceBatch& batch) const {
        JsonDocument doc;

        doc["count"] = batch.count;
        doc["first_ts"] = batch.firstTimestampMs;
        doc["last_ts"] = batch.lastTimestampMs;

        if (m_appCfg) {
            doc["device_id"] = m_appCfg->device.deviceId;
            doc["location_id"] = m_appCfg->device.locationId;
        }

        auto records = doc["records"].to<JsonArray>();

        for (std::size_t i = 0; i < batch.count; ++i) {
            const auto& rec = batch.records[i];

            // Format card ID as hex string
            char cardStr[CARD_ID_SIZE * 2 + 1] = {0};
            for (std::size_t j = 0; j < CARD_ID_SIZE; ++j) {
                snprintf(cardStr + j * 2, 3, "%02X", rec.cardId[j]);
            }

            auto entry = records.add<JsonObject>();
            entry["card"] = cardStr;
            entry["ts"] = rec.timestampMs;
            entry["seq"] = rec.sequenceNumber;
        }

        std::string result;
        serializeJson(doc, result);
        return result;
    }

}  // namespace isic

