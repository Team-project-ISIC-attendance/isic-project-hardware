#ifndef ISIC_CORE_SIGNAL_HPP
#define ISIC_CORE_SIGNAL_HPP

#include "platform/PlatformMutex.hpp"

#include <algorithm>
#include <functional>
#include <vector>

namespace isic
{
/**
 * @brief Enterprise-grade async publish/subscribe signal system
 *
 * @tparam Args Event argument types (must be copyable/movable)
 *
 * @warning dispatch() MUST be called from main loop regularly
 * @note Maximum 16 pending events per signal (configurable via MAX_PENDING_EVENTS)
 * @note Event args stored by value (use std::ref for large objects)
 *
 * @example
 * @code
 * Signal<int, std::string> signal;
 *
 * // Subscribe
 * auto conn = signal.connect([](int x, const std::string& msg) {
 *     Serial.printf("Received: %d, %s\n", x, msg.c_str());
 * });
 *
 * // Publish (from anywhere, including ISR on ESP32)
 * signal.publish(42, "hello");
 *
 * // Dispatch (from main loop)
 * void loop() {
 *     signal.dispatch();  // Invokes all pending callbacks
 * }
 * @endcode
 */
template<typename... Args>
class Signal
{
public:
    using Callback = std::function<void(Args...)>;
    using Connection = std::size_t;

    /**
     * @brief RAII wrapper for automatic disconnection
     */
    class ScopedConnection
    {
    public:
        ScopedConnection() = default;
        ScopedConnection(Signal *signal, const Connection id)
            : m_signal(signal)
            , m_id(id)
        {
        }

        ScopedConnection(const ScopedConnection &) = delete;
        ScopedConnection &operator=(const ScopedConnection &) = delete;

        ScopedConnection(ScopedConnection &&other) noexcept
            : m_signal(other.m_signal)
            , m_id(other.m_id)
        {
            other.m_signal = nullptr;
            other.m_id = 0;
        }

        ScopedConnection &operator=(ScopedConnection &&other) noexcept
        {
            if (this != &other)
            {
                disconnect(); // Disconnect current if exists
                m_signal = other.m_signal;
                m_id = other.m_id;
                other.m_signal = nullptr;
                other.m_id = 0;
            }
            return *this;
        }

        ~ScopedConnection()
        {
            disconnect();
        }

        void disconnect()
        {
            if (m_signal && m_id != 0)
            {
                m_signal->disconnect(m_id);
                m_signal = nullptr;
                m_id = 0;
            }
        }

    private:
        Signal *m_signal{nullptr};
        Connection m_id{0};
    };

    Signal() = default;
    ~Signal() = default;

    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;

    Signal(Signal &&other) noexcept
    {
        LockGuard<Mutex> lock(other.m_mutex);
        m_slots = std::move(other.m_slots);
        m_nextId = other.m_nextId;
        other.m_nextId = 0;
    }

    Signal &operator=(Signal &&other) noexcept
    {
        if (this != &other)
        {
            UniqueLock<Mutex> lockThis(m_mutex);
            UniqueLock<Mutex> lockOther(other.m_mutex);
            m_slots = std::move(other.m_slots);
            m_nextId = other.m_nextId;
            other.m_nextId = 0;
        }
        return *this;
    }

    /**
     * @brief Connect a callback to this signal
     *
     * @param callback Function to call when signal is emitted
     * @return Connection ID for later disconnection (0 if callback is null)
     * @note Thread-safe, can be called from any task
     */
    [[nodiscard]] Connection connect(Callback callback)
    {
        if (!callback)
            return 0;

        LockGuard<Mutex> lock(m_mutex);

        // Pre-allocate capacity on first subscription to avoid reallocations
        // Most signals have 1-3 subscribers, so reserving 4 slots prevents
        // the 0→1→2→4 reallocation cascade that fragments ESP8266 heap
        if (m_slots.empty())
        {
            m_slots.reserve(4);
        }

        Connection id = ++m_nextId;
        m_slots.push_back({id, std::move(callback)});
        return id;
    }

    /**
     * @brief Connect and return a scoped connection
     */
    [[nodiscard]] ScopedConnection connectScoped(Callback callback)
    {
        return ScopedConnection(this, connect(std::move(callback)));
    }

    /**
     * @brief Disconnect a callback
     *
     * @param id Connection ID returned from connect()
     * @note Thread-safe, safe to call from within a callback
     * @note Safe to call multiple times with same ID (no-op after first)
     */
    void disconnect(Connection id)
    {
        if (id == 0)
        {
            return;
        }

        LockGuard<Mutex> lock(m_mutex);
        const auto findByIndex{[id](const Slot &slot) { return slot.id == id; }};
        m_slots.erase(std::remove_if(m_slots.begin(), m_slots.end(), findByIndex), m_slots.end());
    }

    /**
     * @brief Publish event for async dispatch
     *
     * @param args Event arguments to publish
     * @return true if published successfully, false if buffer full
     *
     * @note Does NOT execute callbacks immediately - call dispatch() to process
     * @note If buffer is full, oldest pending event is dropped (FIFO behavior)
     * @warning ESP8266 ISR: Keep Args copyable and lightweight (no heap allocation)
     */
    bool publish(Args... args)
    {
        LockGuard<Mutex> lock(m_mutex);

        // Check if buffer full - drop oldest if needed (ring buffer overflow policy)
        if (m_pendingCount >= MAX_PENDING_EVENTS)
        {
            // Ring buffer full - advance read position (drop oldest)
            m_pendingRead = (m_pendingRead + 1) % MAX_PENDING_EVENTS;
            --m_pendingCount;
        }

        // Store event in ring buffer
        m_pendingEvents[m_pendingWrite] = PendingEvent{std::forward<Args>(args)...};
        m_pendingWrite = (m_pendingWrite + 1) % MAX_PENDING_EVENTS;
        ++m_pendingCount;

        return true;
    }

    /**
     * @brief Process all pending publications and invoke subscribers
     *
     * @return Number of events dispatched
     *
     * @note Thread-safe: Can be called while publish() happens from other contexts
     * @note Callbacks execute in order of publication (FIFO)
     * @note Safe to call publish() from within callbacks (events queued for next dispatch)
     */
    std::size_t dispatch()
    {
        std::size_t dispatched{0};

        // Process all pending events
        while (true)
        {
            PendingEvent event;
            bool hasEvent{false};

            // Critical section: Extract one event
            {
                LockGuard<Mutex> lock(m_mutex);
                if (m_pendingCount > 0)
                {
                    event = std::move(m_pendingEvents[m_pendingRead]);
                    m_pendingRead = (m_pendingRead + 1) % MAX_PENDING_EVENTS;
                    --m_pendingCount;
                    hasEvent = true;
                }
            }

            if (!hasEvent)
            {
                break; // No more pending events
            }

            // Invoke callbacks with mutex unlocked (allows re-entrant publish)
            invokeCallbacks(event);
            ++dispatched;
        }

        return dispatched;
    }

    /**
     * @brief Get number of pending events awaiting dispatch
     *
     * @return Pending event count
     */
    [[nodiscard]] std::size_t pendingCount() const
    {
        LockGuard<Mutex> lock(m_mutex);
        return m_pendingCount;
    }

    /**
     * @brief Operator overload for publish
     */
    bool operator()(Args... args)
    {
        return publish(std::forward<Args>(args)...);
    }

    /**
     * @brief Disconnect all callbacks
     */
    void clear()
    {
        LockGuard<Mutex> lock(m_mutex);
        m_slots.clear();
    }

    /**
     * @brief Get number of connected slots
     *
     * @return Number of active connections
     */
    [[nodiscard]] std::size_t size() const
    {
        LockGuard<Mutex> lock(m_mutex);
        return m_slots.size();
    }

    /**
     * @brief Check if signal has any connected slots
     *
     * @return true if no slots are connected
     */
    [[nodiscard]] bool empty() const
    {
        LockGuard<Mutex> lock(m_mutex);
        return m_slots.empty();
    }

private:
    struct Slot
    {
        Connection id;
        Callback callback;
    };

    /**
     * @brief Storage for pending event arguments
     *
     * Stores a tuple of event arguments for deferred dispatch.
     * Uses decay to store values instead of references (no dangling references).
     */
    struct PendingEvent
    {
        std::tuple<std::decay_t<Args>...> args;

        PendingEvent() = default;

        template<typename... TArgs>
        explicit PendingEvent(TArgs&&... a)
            : args(std::forward<TArgs>(a)...)
        {
        }
    };

    /**
     * @brief Invoke all subscribers with event arguments
     *
     * @param event Pending event with stored arguments
     */
    void invokeCallbacks(PendingEvent &event)
    {
        std::vector<Slot> slotsCopy;

        // Copy subscriber list under lock
        {
            LockGuard<Mutex> lock(m_mutex);
            if (m_slots.empty())
            {
                return;
            }
            slotsCopy = m_slots;
        }

        // Invoke callbacks with mutex unlocked (allows re-entrant operations)
        for (const auto &slot: slotsCopy)
        {
            if (slot.callback)
            {
                std::apply(slot.callback, event.args);
            }
        }
    }

    /// Ring buffer size - balance between memory and event burst capacity
    /// ESP8266: Reduced to 4 to save memory (27 KB across 36 signals)
    /// With 100 Hz dispatch rate, 4 slots provide adequate buffering
    static constexpr std::size_t MAX_PENDING_EVENTS{4};

    mutable Mutex m_mutex;
    std::vector<Slot> m_slots;
    Connection m_nextId{0};

    // Ring buffer for async event dispatch
    PendingEvent m_pendingEvents[MAX_PENDING_EVENTS];
    std::size_t m_pendingRead{0};
    std::size_t m_pendingWrite{0};
    std::size_t m_pendingCount{0};
};
} // namespace isic

#endif // ISIC_CORE_SIGNAL_HPP
