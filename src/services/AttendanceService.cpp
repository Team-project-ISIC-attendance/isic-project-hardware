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
    obj["uid"] = cardUidToString(record.cardUid);
    obj["ts"] = platform::getUnixTimeMs().value_or(0);
    obj["seq"] = record.sequence;
}

} // namespace

AttendanceService::AttendanceService(EventBus &bus, const AttendanceConfig &config)
    : ServiceBase("AttendanceService")
    , m_bus(bus)
    , m_config(config)
{
    m_batch.reserve(m_config.batchMaxSize);
    if (m_config.offlineBufferSize > 0)
    {
        m_offlineRing.resize(m_config.offlineBufferSize);
    }

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

    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::ConfigChanged, [this](const Event & /*e*/) {
        // m_config is a live reference — values are already updated by ConfigService
        LOG_INFO(m_name, "Config updated: batch=%u, offline=%u, debounce=%ums",
                 m_config.batchMaxSize, m_config.offlineBufferSize, m_config.debounceIntervalMs);

        // Trim live batch if new max is smaller
        while (m_batch.size() > m_config.batchMaxSize)
        {
            m_batch.pop_back();
        }

        // Trim offline ring buffer to new capacity, applying current overflow policy
        const std::size_t newCap = m_config.offlineBufferSize;
        if (newCap == 0)
        {
            m_offlineHead = m_offlineTail = m_offlineCount = 0;
        }
        else
        {
            while (m_offlineCount > newCap)
            {
                switch (m_config.offlineQueuePolicy)
                {
                    case AttendanceConfig::OfflineQueuePolicy::DropOldest:
                        m_offlineHead = (m_offlineHead + 1) % m_offlineRing.size();
                        break;
                    case AttendanceConfig::OfflineQueuePolicy::DropNewest:
                    case AttendanceConfig::OfflineQueuePolicy::DropAll:
                        if (m_offlineTail == 0)
                            m_offlineTail = m_offlineRing.size() - 1;
                        else
                            --m_offlineTail;
                        break;
                }
                --m_offlineCount;
            }
            m_offlineRing.resize(newCap);
        }
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

    if (m_offlineCount > 0 && !m_useOfflineMode)
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
        if ((lastSeenMs != 0) && (memcmp(uid.data(), cardUid.data(), kCardUidMaxSize) == 0))
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

    // Online mode: serialize into stack buffer and publish
    const auto recordCount{m_batch.size()};

    JsonDocument doc;
    const auto arr{doc.to<JsonArray>()};
    for (const auto &record: m_batch)
    {
        serializeRecord(arr.add<JsonObject>(), record);
    }

    char buf[512];
    const auto len{serializeJson(doc, buf, sizeof(buf))};

    LOG_INFO(m_name, "Flush: %u records, %u bytes", recordCount, len);
    m_bus.publish(Event{EventType::MqttPublishRequest, MqttEvent{"attendance", std::string(buf, len), false}});

    ++m_metrics.batchesSent;
    m_batch.clear();
}

void AttendanceService::addToOfflineBatch(const AttendanceRecord &record)
{
    const auto cap{static_cast<std::uint16_t>(m_offlineRing.size())};
    if (cap == 0)
    {
        return;
    }

    if (m_offlineCount < cap)
    {
        // Fast path: ring has room — O(1)
        m_offlineRing[m_offlineTail] = record;
        m_offlineTail = static_cast<std::uint16_t>((m_offlineTail + 1) % cap);
        ++m_offlineCount;
        return;
    }

    // Slow path: buffer full — apply policy
    ++m_metrics.errorCount;

    switch (m_config.offlineQueuePolicy)
    {
        case AttendanceConfig::OfflineQueuePolicy::DropOldest: {
            // Overwrite oldest slot — O(1)
            m_offlineRing[m_offlineTail] = record;
            m_offlineTail = static_cast<std::uint16_t>((m_offlineTail + 1) % cap);
            m_offlineHead = static_cast<std::uint16_t>((m_offlineHead + 1) % cap);
            LOG_WARN(m_name, "Buffer full: dropped oldest");
            break;
        }
        case AttendanceConfig::OfflineQueuePolicy::DropNewest: {
            LOG_WARN(m_name, "Buffer full: dropped newest");
            break;
        }
        case AttendanceConfig::OfflineQueuePolicy::DropAll: {
            m_offlineHead = 0;
            m_offlineTail = 1 % cap;
            m_offlineCount = 1;
            m_offlineRing[0] = record;
            LOG_WARN(m_name, "Buffer full: cleared all");
            break;
        }
        default: {
            LOG_WARN(m_name, "Buffer full: unknown policy, dropped newest");
            break;
        }
    }
}

void AttendanceService::flushOfflineBatch()
{
    if (m_offlineCount == 0 || m_useOfflineMode)
    {
        return;
    }

    const auto cap{static_cast<std::uint16_t>(m_offlineRing.size())};
    const auto recordCount{m_offlineCount};

    JsonDocument doc;
    const auto arr{doc.to<JsonArray>()};
    for (std::uint16_t i = 0; i < recordCount; ++i)
    {
        const auto idx{static_cast<std::uint16_t>((m_offlineHead + i) % cap)};
        serializeRecord(arr.add<JsonObject>(), m_offlineRing[idx]);
    }

    char buf[512];
    const auto len{serializeJson(doc, buf, sizeof(buf))};

    LOG_INFO(m_name, "Offline flush: %u records, %u bytes", recordCount, len);
    m_bus.publish(Event{EventType::MqttPublishRequest, MqttEvent{"attendance", std::string(buf, len), false}});

    m_offlineHead = 0;
    m_offlineTail = 0;
    m_offlineCount = 0;
    ++m_metrics.batchesSent;
}
} // namespace isic
