#ifndef ISIC_SERVICES_ATTENDANCESERVICE_HPP
#define ISIC_SERVICES_ATTENDANCESERVICE_HPP

/**
 * @file AttendanceService.hpp
 * @brief Attendance recording and batching service
 *
 * Handles card scan processing, batching, and offline buffering before
 * publishing to the backend.
 */

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <array>
#include <vector>

namespace isic
{
class AttendanceService : public ServiceBase
{
public:
    AttendanceService(EventBus &bus, const AttendanceConfig &config);
    ~AttendanceService() override = default;

    AttendanceService(const AttendanceService &) = delete;
    AttendanceService &operator=(const AttendanceService &) = delete;
    AttendanceService(AttendanceService &&) = delete;
    AttendanceService &operator=(AttendanceService &&) = delete;

    // IService interface
    Status begin() override;
    void loop() override;
    void end() override;

    [[nodiscard]] const AttendanceMetrics &getMetrics() const
    {
        return m_metrics;
    }

    [[nodiscard]] std::size_t getCurrentBatchSize() const noexcept
    {
        return m_batch.size();
    }

    [[nodiscard]] std::size_t getOfflineBufferSize() const noexcept
    {
        return m_offlineBatch.size();
    }

    [[nodiscard]] bool isOfflineMode() const noexcept
    {
        return m_useOfflineMode;
    }

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
        obj["cards_processed"] = m_metrics.cardsProcessed;
        obj["cards_debounced"] = m_metrics.cardsDebounced;
        obj["batches_sent"] = m_metrics.batchesSent;
        obj["errors"] = m_metrics.errorCount;
    }

private:
    [[nodiscard]] bool shouldProcessCard(const CardUid &cardUid, std::uint32_t timestampMs) noexcept;
    void processCard(const CardEvent &card);

    void addToBatch(const AttendanceRecord &record);
    void flushBatch();

    void addToOfflineBatch(const AttendanceRecord &record);
    void flushOfflineBatch();

    void flush();

    EventBus &m_bus;
    const AttendanceConfig &m_config;

    AttendanceMetrics m_metrics{};

    // Offline mode flag
    bool m_useOfflineMode{true};

    // Current batch
    std::vector<AttendanceRecord> m_batch{};
    std::uint32_t m_batchStartMs{0};
    std::uint32_t m_sequenceNumber{0};

    // Offline buffer
    std::vector<AttendanceRecord> m_offlineBatch{};
    std::uint32_t m_lastOfflineRetryMs{0};

    // Debounce cache
    struct DebounceEntry
    {
        CardUid uid{};
        std::uint32_t lastSeenMs{0};
    };
    std::array<DebounceEntry, AttendanceConfig::Constants::kDebounceCacheSize> m_debounceCache{};
    std::uint8_t m_debounceCacheIndex{0};

    // Event subscriptions
    std::vector<EventBus::Subscription> m_eventConnections{};
};
} // namespace isic

#endif // ISIC_SERVICES_ATTENDANCESERVICE_HPP
