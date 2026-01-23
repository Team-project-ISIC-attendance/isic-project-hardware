#include "services/AttendanceService.hpp"

#include "common/Logger.hpp"
#include "platform/PlatformESP.hpp"

#include <ArduinoJson.h>

namespace isic
{
namespace
{
bool hasTimeElapsed(const std::uint32_t startMs, const std::uint32_t nowMs, const std::uint32_t thresholdMs) noexcept
{
    return (nowMs - startMs) >= thresholdMs;
}

void serializeRecord(const JsonObject &obj, const AttendanceRecord &record)
{
    const auto unixMs{platform::getUnixTimeMs()};
    obj["uid"] = cardUidToString(record.cardUid);
    obj["ts"] = unixMs.value_or(0); // TODO: handle missing unix time, for now set to 0 backend must handle it
    obj["seq"] = record.sequence;
}

std::string serializeBatch(const std::vector<AttendanceRecord> &records)
{
    JsonDocument doc;
    const auto arr{doc.to<JsonArray>()};

    for (const auto &record: records)
    {
        serializeRecord(arr.add<JsonObject>(), record);
    }

    std::string json;
    serializeJson(doc, json);

    return json; // NRVO must apply
}
} // namespace

AttendanceService::AttendanceService(EventBus &bus, const AttendanceConfig &config)
    : ServiceBase("AttendanceService")
    , m_bus(bus)
    , m_config(config)
{
    m_batch.reserve(m_config.batchMaxSize);
    m_offlineBatch.reserve(m_config.offlineBufferSize);

    m_eventConnections.reserve(4);
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::CardScanned, [this](const Event &e) {
        if (const auto *card = e.get<CardEvent>())
        {
            processCard(*card);
        }
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttConnected, [this](const Event & /*e*/) {
        m_useOfflineMode = false;
        flushOfflineBatch();
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttDisconnected, [this](const Event & /*e*/) {
        m_useOfflineMode = true;
    }));

    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::ConfigChanged, [this](const Event /*e*/) {
        // Reload config on changes
        LOG_INFO(m_name, "Config changed, reloading...");
        // In this simplified example, we assume m_config is updated externally
        // TODO: handle dynamic config changes if needed
    }));
}

Status AttendanceService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Init: batch=%u, offline=%u, debounce=%ums", m_config.batchMaxSize, m_config.offlineBufferSize, m_config.debounceIntervalMs);
    setState(ServiceState::Running);
    return Status::Ok();
}

void AttendanceService::loop()
{
    // Only loop if service is in Running state
    if (m_state != ServiceState::Running)
    {
        return;
    }

    const auto now{millis()};

    // Check batch flush conditions (only if batch has data)
    if (!m_batch.empty())
    {
        const bool batchFull{m_batch.size() >= m_config.batchMaxSize};
        const bool batchTimeout{hasTimeElapsed(m_batchStartMs, now, m_config.batchFlushIntervalMs)};

        if (batchFull || batchTimeout)
        {
            flushBatch();
        }
    }

    if (!m_offlineBatch.empty() && !m_useOfflineMode)
    {
        if (hasTimeElapsed(m_lastOfflineRetryMs, now, m_config.offlineBufferFlushIntervalMs))
        {
            flushOfflineBatch();
            m_lastOfflineRetryMs = now;
        }
    }
}

void AttendanceService::end()
{
    setState(ServiceState::Stopping);
    LOG_INFO(m_name, "Shutting down...");

    flush();
    m_eventConnections.clear();

    setState(ServiceState::Stopped);
    LOG_INFO(m_name, "Stopped");
}

void AttendanceService::flush()
{
    flushBatch();
    flushOfflineBatch();
}

void AttendanceService::processCard(const CardEvent &card)
{
    // Early exit if debounced - most common case for rapid scans
    if (!shouldProcessCard(card.uid, card.timestampMs))
    {
        LOG_INFO(m_name, "Card debounced: %s", cardUidToString(card.uid).c_str());
        ++m_metrics.cardsDebounced;
        return;
    }

    const AttendanceRecord record{
            .timestampMs = card.timestampMs,
            .sequence = ++m_sequenceNumber,
            .cardUid = card.uid,
    };

    LOG_INFO(m_name, "Card: %s seq=%u", cardUidToString(card.uid).c_str(), record.sequence);
    ++m_metrics.cardsProcessed;

    addToBatch(record);
    if (!m_config.batchingEnabled)
    {
        flushBatch();
    }

    m_bus.publish(EventType::AttendanceRecorded);
}

bool AttendanceService::shouldProcessCard(const CardUid &cardUid, const std::uint32_t timestampMs) noexcept
{
    // Search for existing entry and update in-place if found
    for (auto &[uid, lastSeenMs]: m_debounceCache)
    {
        if ((lastSeenMs != 0) && (uid == cardUid))
        {
            if (!hasTimeElapsed(lastSeenMs, timestampMs, m_config.debounceIntervalMs))
            {
                return false; // Still in debounce window
            }
            // Expired - update timestamp in-place and allow
            lastSeenMs = timestampMs;
            return true;
        }
    }

    // Not found - add new entry using ring buffer eviction
    auto &entry{m_debounceCache[m_debounceCacheIndex]};
    entry.uid = cardUid;
    entry.lastSeenMs = timestampMs;

    m_debounceCacheIndex = static_cast<std::uint8_t>((m_debounceCacheIndex + 1) % AttendanceConfig::Constants::kDebounceCacheSize);
    return true;
}

void AttendanceService::addToBatch(const AttendanceRecord &record)
{
    // Fast path: batch has room
    if (m_batch.size() < m_config.batchMaxSize)
    {
        if (m_batch.empty())
        {
            m_batchStartMs = millis();
        }
        m_batch.push_back(record);
        return;
    }

    // Slow path: batch is full
    // This can happen if batchingEnabled is true and cards come faster than flush interval
    flushBatch();

    if (m_batch.size() < m_config.batchMaxSize)
    {
        m_batchStartMs = millis();
        m_batch.push_back(record);
    }
    else
    {
        addToOfflineBatch(record);
    }
}

void AttendanceService::flushBatch()
{
    if (m_batch.empty())
    {
        return; // Nothing to flush
    }

    // Offline mode: move batch to offline buffer
    if (m_useOfflineMode)
    {
        LOG_DEBUG(m_name, "Offline: buffering %u records", m_batch.size());
        for (const auto &record: m_batch)
        {
            addToOfflineBatch(record);
        }
        m_batch.clear();
        return;
    }

    // Online mode: serialize and publish
    const auto recordCount{m_batch.size()};
    auto json{serializeBatch(m_batch)};

    LOG_INFO(m_name, "Flush: %u records, %u bytes", recordCount, json.length());
    m_bus.publish(Event{EventType::MqttPublishRequest, MqttEvent{"attendance", std::move(json), false}});

    ++m_metrics.batchesSent;
    m_batch.clear();
}

void AttendanceService::addToOfflineBatch(const AttendanceRecord &record)
{
    // Fast path: buffer has room
    if (m_offlineBatch.size() < m_config.offlineBufferSize)
    {
        m_offlineBatch.push_back(record);
        return;
    }

    ++m_metrics.errorCount; // Count as error because we couldn't send it, add data loss

    // Slow path: buffer is full - apply policy
    switch (m_config.offlineQueuePolicy)
    {
        case AttendanceConfig::OfflineQueuePolicy::DropOldest: {
            // Remove first element (oldest) - O(n) but rare operation
            m_offlineBatch.erase(m_offlineBatch.begin());
            m_offlineBatch.push_back(record);
            LOG_WARN(m_name, "Buffer full: dropped oldest");
            break;
        }
        case AttendanceConfig::OfflineQueuePolicy::DropNewest: {
            // Simply don't add the new record
            LOG_WARN(m_name, "Buffer full: dropped newest");
            break;
        }
        case AttendanceConfig::OfflineQueuePolicy::DropAll: {
            // Nuclear option - clear everything, start fresh
            m_offlineBatch.clear();
            m_offlineBatch.push_back(record);
            LOG_WARN(m_name, "Buffer full: cleared all");
            break;
        }
        default: {
            // Defensive: unknown policy, drop newest (safest)
            LOG_WARN(m_name, "Buffer full: unknown policy");
            break;
        }
    }
}

void AttendanceService::flushOfflineBatch()
{
    if (m_offlineBatch.empty() || m_useOfflineMode)
    {
        return;
    }

    const auto recordCount{m_offlineBatch.size()};
    auto json{serializeBatch(m_offlineBatch)};

    LOG_INFO(m_name, "Offline flush: %u records, %u bytes", recordCount, json.length());
    m_bus.publish(Event{EventType::MqttPublishRequest, MqttEvent{"attendance", std::move(json), false}});

    m_offlineBatch.clear();
    ++m_metrics.batchesSent;
}
} // namespace isic
