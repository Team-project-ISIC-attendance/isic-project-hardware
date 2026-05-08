#ifndef ISIC_CORE_SIGNAL_HPP
#define ISIC_CORE_SIGNAL_HPP

/**
 * @file Signal.hpp
 * @brief Asynchronous publish/subscribe signal system for embedded systems
 *
 * Provides type-safe, thread-safe signal/slot mechanism with deferred
 * event dispatch. Optimized for resource-constrained microcontrollers
 * with ISR-safe publishing and deterministic memory usage.
 */

#include "platform/PlatformMutex.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <vector>

namespace isic
{

/**
 * @class Signal
 * @brief Thread-safe asynchronous signal with typed arguments
 *
 * Signal implements the observer pattern with deferred callback invocation.
 * Events are queued during publish() and delivered to subscribers during
 * dispatch(), enabling safe event emission from ISR contexts.
 *
 * @tparam Args Event argument types (must be copyable for queue storage)
 *
 * @par Thread Safety
 * - publish(): Safe from any context including ISR
 * - connect()/disconnect(): Safe from any context
 * - dispatch(): Must be called from main loop only
 *
 * @par Memory Model
 * - Fixed-size ring buffer for pending events (no dynamic allocation on publish)
 * - Subscriber list grows dynamically but pre-reserves capacity
 * - Arguments stored by value (use lightweight types or std::ref)
 *
 * @par Overflow Policy
 * When the ring buffer is full, oldest events are dropped (FIFO eviction).
 * Monitor with pendingCount() to detect saturation.
 *
 * @par Usage Example
 * @code
 * Signal<int, std::string> onData;
 *
 * // Subscribe with RAII cleanup
 * auto conn = onData.connectScoped([](int code, const std::string& msg) {
 *     Serial.printf("Received: %d - %s\n", code, msg.c_str());
 * });
 *
 * // Publish from anywhere (ISR-safe)
 * onData.publish(42, "hello");
 *
 * // Dispatch from main loop
 * void loop() {
 *     onData.dispatch();  // Invokes callbacks here
 * }
 * @endcode
 *
 * @see EventBus for typed event routing across multiple signal types
 */
template<typename... Args>
class Signal
{
public:
    using Callback = std::function<void(Args...)>;
    using Connection = std::size_t;

    /**
     * @class ScopedConnection
     * @brief RAII wrapper for automatic signal disconnection
     *
     * Automatically disconnects from the signal when destroyed.
     * Move-only to ensure single ownership of the connection.
     *
     * @par Usage
     * Store as a class member to tie subscription lifetime to object lifetime:
     * @code
     * class MyHandler {
     *     Signal<int>::ScopedConnection m_conn;
     * public:
     *     void subscribe(Signal<int>& sig) {
     *         m_conn = sig.connectScoped([this](int x) { handle(x); });
     *     }
     * };  // Auto-disconnects when MyHandler is destroyed
     * @endcode
     */
    class ScopedConnection
    {
    public:
        ScopedConnection() = default;

        ScopedConnection(Signal *signal, Connection id)
            : m_signal(signal)
            , m_id(id)
        {
        }

        ~ScopedConnection()
        {
            disconnect();
        }
        // Non-copyable
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
                disconnect();
                m_signal = other.m_signal;
                m_id = other.m_id;
                other.m_signal = nullptr;
                other.m_id = 0;
            }
            return *this;
        }

        /// Manually disconnect before destruction
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
        m_snapshot = std::move(other.m_snapshot);
        m_pendingEvents = std::move(other.m_pendingEvents);
        m_pendingRead = other.m_pendingRead;
        m_pendingWrite = other.m_pendingWrite;
        m_pendingCount = other.m_pendingCount;
        m_dropCount = other.m_dropCount;
        m_nextId = other.m_nextId;
        other.m_nextId = 0;
        other.m_pendingRead = 0;
        other.m_pendingWrite = 0;
        other.m_pendingCount = 0;
        other.m_dropCount = 0;
    }

    Signal &operator=(Signal &&other) noexcept
    {
        if (this != &other)
        {
            UniqueLock<Mutex> lockThis(m_mutex);
            UniqueLock<Mutex> lockOther(other.m_mutex);
            m_slots = std::move(other.m_slots);
            m_snapshot = std::move(other.m_snapshot);
            m_pendingEvents = std::move(other.m_pendingEvents);
            m_pendingRead = other.m_pendingRead;
            m_pendingWrite = other.m_pendingWrite;
            m_pendingCount = other.m_pendingCount;
            m_dropCount = other.m_dropCount;
            m_nextId = other.m_nextId;
            other.m_nextId = 0;
            other.m_pendingRead = 0;
            other.m_pendingWrite = 0;
            other.m_pendingCount = 0;
            other.m_dropCount = 0;
        }
        return *this;
    }

    /**
     * @brief Register a callback to receive signal emissions
     *
     * @param callback Function to invoke when signal is dispatched
     * @return Connection handle for manual disconnection, 0 if callback is null
     *
     * @note Prefer connectScoped() for automatic lifetime management
     * @note Thread-safe: can be called from any context
     *
     * @par Complexity
     * O(1) amortized (vector push_back)
     */
    [[nodiscard]] Connection connect(Callback callback)
    {
        if (!callback)
        {
            return 0;
        }

        LockGuard<Mutex> lock(m_mutex);

        // Allocate lazily so unused event types do not consume heap at boot.
        if (m_slots.empty())
        {
            m_slots.reserve(kInitialSlotCapacity);
        }

        Connection id = ++m_nextId;
        m_slots.push_back({id, std::move(callback)});
        return id;
    }

    /**
     * @brief Register a callback with RAII-based automatic cleanup
     *
     * @param callback Function to invoke when signal is dispatched
     * @return ScopedConnection that disconnects on destruction
     *
     * @note Store the returned ScopedConnection as a class member
     */
    [[nodiscard]] ScopedConnection connectScoped(Callback callback)
    {
        return ScopedConnection(this, connect(std::move(callback)));
    }

    /**
     * @brief Remove a subscription using its connection handle
     *
     * @param id Connection handle from connect()
     *
     * @note Thread-safe: can be called from any context
     * @note Safe to call from within a callback
     * @note Idempotent: safe to call multiple times with same ID
     */
    void disconnect(Connection id)
    {
        if (id == 0)
        {
            return;
        }

        LockGuard<Mutex> lock(m_mutex);
        auto it = std::remove_if(m_slots.begin(), m_slots.end(),
                                 [id](const Slot &slot) { return slot.id == id; });
        m_slots.erase(it, m_slots.end());
    }

    /**
     * @brief Disconnect all subscribers
     */
    void clear()
    {
        LockGuard<Mutex> lock(m_mutex);
        m_slots.clear();
    }

    /**
     * @brief Queue event arguments for asynchronous delivery
     *
     * Copies arguments into the internal ring buffer. Subscribers are
     * NOT invoked immediately - events are delivered during dispatch().
     *
     * @param args Event arguments to publish
     * @return true always (oldest event dropped if buffer full)
     *
     * @note ISR-safe: can be called from interrupt handlers
     * @note Events delivered in FIFO order
     * @warning Keep Args lightweight on ESP8266 ISR (no heap allocation)
     *
     * @par Overflow Behavior
     * When buffer is full, oldest pending event is silently dropped.
     *
     * @par Complexity
     * O(1) for queue insertion
     */
    template<typename... TArgs>
    bool publish(TArgs &&...args)
    {
        static_assert(sizeof...(TArgs) == sizeof...(Args), "Signal::publish argument count mismatch");
        static_assert((std::is_constructible_v<std::decay_t<Args>, TArgs &&> && ...), "Signal::publish argument type mismatch");

        LockGuard<Mutex> lock(m_mutex);

        if (!m_pendingEvents)
        {
            m_pendingEvents.reset(new (std::nothrow) PendingEvent[kMaxPendingEvents]);
            if (!m_pendingEvents)
            {
                return false;
            }
        }

        // Ring buffer overflow: drop oldest event and count the loss
        if (m_pendingCount >= kMaxPendingEvents)
        {
            m_pendingRead = (m_pendingRead + 1) % kMaxPendingEvents;
            --m_pendingCount;
            ++m_dropCount;
        }

        // Store in ring buffer
        m_pendingEvents[m_pendingWrite] = PendingEvent{std::forward<Args>(args)...};
        m_pendingWrite = (m_pendingWrite + 1) % kMaxPendingEvents;
        ++m_pendingCount;

        return true;
    }

    /// Convenience: operator() as alias for publish()
    template<typename... TArgs>
    bool operator()(TArgs &&...args)
    {
        return publish(std::forward<TArgs>(args)...);
    }

    /**
     * @brief Deliver all queued events to subscribers
     *
     * Processes all pending events, invoking registered callbacks for each.
     * Events published during dispatch are queued for the next cycle.
     *
     * @return Number of events dispatched
     *
     * @warning Must be called from main loop (not ISR-safe)
     *
     * @par Re-entrancy
     * Safe to call publish() from within callbacks - events are queued.
     *
     * @par Complexity
     * O(E * S) where E = pending events, S = subscriber count
     */
    std::size_t dispatch()
    {
        std::size_t dispatched{0};

        while (true)
        {
            PendingEvent event;
            bool hasEvent{false};

            // Extract one event under lock
            {
                LockGuard<Mutex> lock(m_mutex);
                if (m_pendingEvents && m_pendingCount > 0)
                {
                    event = std::move(m_pendingEvents[m_pendingRead]);
                    m_pendingRead = (m_pendingRead + 1) % kMaxPendingEvents;
                    --m_pendingCount;
                    hasEvent = true;
                }
            }

            if (!hasEvent)
            {
                break;
            }

            // Invoke callbacks with mutex unlocked (allows re-entrant publish)
            invokeCallbacks(event);
            ++dispatched;
        }

        return dispatched;
    }

    /// Number of events awaiting dispatch
    [[nodiscard]] std::size_t pendingCount() const
    {
        LockGuard<Mutex> lock(m_mutex);
        return m_pendingCount;
    }

    /// Cumulative number of events dropped due to ring buffer overflow
    [[nodiscard]] std::size_t dropCount() const
    {
        LockGuard<Mutex> lock(m_mutex);
        return m_dropCount;
    }

    /// Number of connected subscribers
    [[nodiscard]] std::size_t size() const
    {
        LockGuard<Mutex> lock(m_mutex);
        return m_slots.size();
    }

    /// True if no subscribers connected
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

    /// Stores event arguments for deferred dispatch (values, not references)
    struct PendingEvent
    {
        std::tuple<std::decay_t<Args>...> args;

        PendingEvent() = default;

        template<typename... TArgs>
        explicit PendingEvent(TArgs &&...a)
            : args(std::forward<TArgs>(a)...)
        {
        }
    };

    void invokeCallbacks(PendingEvent &event)
    {
        // Copy subscriber list under lock into a reusable snapshot buffer.
        // This avoids heap allocations on every event dispatch.
        {
            UniqueLock<Mutex> lock(m_mutex);
            if (m_slots.empty())
            {
                return;
            }
            if (m_snapshot.capacity() < m_slots.size())
            {
                m_snapshot.reserve(std::max(kInitialSlotCapacity, m_slots.size()));
            }
            m_snapshot = m_slots;
        }

        // Invoke with mutex unlocked (allows re-entrant operations)
        for (const auto &slot: m_snapshot)
        {
            if (slot.callback)
            {
                std::apply(slot.callback, event.args);
            }
        }
    }

    /// Ring buffer capacity - small enough for RAM pressure, but large enough
    /// to handle short bursts such as split config responses on ESP8266.
#if defined(ISIC_PLATFORM_ESP8266)
    static constexpr std::size_t kMaxPendingEvents{12};
#else
    static constexpr std::size_t kMaxPendingEvents{8};
#endif

    /// Initial slot vector capacity - most event types have few subscribers.
    static constexpr std::size_t kInitialSlotCapacity{2};

    mutable Mutex m_mutex;
    std::vector<Slot> m_slots;
    std::vector<Slot> m_snapshot;
    Connection m_nextId{0};

    // Ring buffer for async event dispatch
    std::unique_ptr<PendingEvent[]> m_pendingEvents{};
    std::size_t m_pendingRead{0};
    std::size_t m_pendingWrite{0};
    std::size_t m_pendingCount{0};
    std::size_t m_dropCount{0};
};
} // namespace isic

#endif // ISIC_CORE_SIGNAL_HPP
