#ifndef ISIC_CORE_EVENTBUS_HPP
#define ISIC_CORE_EVENTBUS_HPP

/**
 * @file EventBus.hpp
 * @brief Asynchronous publish/subscribe event bus for embedded systems
 *
 * Provides decoupled inter-component communication via typed events.
 * Designed for resource-constrained embedded environments with
 * ISR-safe publishing and deterministic dispatch.
 */

#include "common/Types.hpp"
#include "core/Signal.hpp"

#include <array>
#include <utility>
#include <variant>

namespace isic
{
/**
 * @class EventBus
 * @brief Thread-safe asynchronous event bus with typed subscriptions
 *
 * EventBus implements the publish/subscribe pattern optimized for embedded
 * systems. Events are queued during publish() and delivered to subscribers
 * during dispatch(), enabling safe event emission from ISR contexts.
 *
 * @par Thread Safety
 * - publish(): Safe from any context including ISR
 * - subscribe()/unsubscribe(): Must be called from main context only
 * - dispatch(): Must be called from main loop only
 *
 * @par Memory Model
 * - Fixed-size signal array indexed by EventType (no dynamic allocation)
 * - Each signal maintains its own subscriber list and event queue
 *
 * @par Usage Example
 * @code
 * class MyService {
 *     EventBus& m_bus;
 *     EventBus::ScopedConnection m_cardConn;  // Auto-unsubscribes on destroy
 *
 * public:
 *     void begin() {
 *         m_cardConn = m_bus.subscribeScoped(EventType::CardScanned,
 *             [this](const Event& e) { onCardScanned(e); });
 *     }
 *
 *     void onCardScanned(const Event& e) {
 *         if (auto* card = e.get<CardEvent>()) {
 *             processCard(card->uid);
 *         }
 *     }
 * };
 *
 * // Main loop
 * void loop() {
 *     bus.dispatch();  // Delivers all queued events
 * }
 * @endcode
 *
 * @see Signal for underlying queue implementation
 * @see Event for event payload structure
 */
class EventBus
{
public:
    static constexpr auto kTag{"EventBus"};

    using SignalType = Signal<const Event &>;
    using Connection = SignalType::Connection;
    using ScopedConnection = SignalType::ScopedConnection;
    using Callback = SignalType::Callback;

    using ExclusiveSignalType = Signal<Event>;
    using ExclusiveConnection = ExclusiveSignalType::Connection;
    using ExclusiveScopedConnection = ExclusiveSignalType::ScopedConnection;
    using ExclusiveCallback = ExclusiveSignalType::Callback;

    class Subscription
    {
    public:
        Subscription() = default;
        explicit Subscription(ScopedConnection connection)
            : m_connection(std::move(connection))
        {
        }
        explicit Subscription(ExclusiveScopedConnection connection)
            : m_connection(std::move(connection))
        {
        }

        Subscription(Subscription &&) noexcept = default;
        Subscription &operator=(Subscription &&) noexcept = default;

        Subscription(const Subscription &) = delete;
        Subscription &operator=(const Subscription &) = delete;

    private:
        std::variant<std::monostate, ScopedConnection, ExclusiveScopedConnection> m_connection{};
    };

    EventBus() = default;
    ~EventBus() = default;

    EventBus(const EventBus &) = delete;
    EventBus &operator=(const EventBus &) = delete;
    EventBus(EventBus &&) = delete;
    EventBus &operator=(EventBus &&) = delete;

    /**
     * @brief Register a callback for a specific event type
     *
     * @param type Event type to subscribe to
     * @param callback Function to invoke when event is dispatched
     * @return Connection handle for manual unsubscription, 0 if invalid type
     *
     * @note Prefer subscribeScoped() for automatic lifetime management
     * @note Callback is NOT invoked immediately - only during dispatch()
     * @warning Must be called from main context (not ISR-safe)
     *
     * @par Complexity
     * O(1) for subscription registration
     */
    [[nodiscard]] Connection subscribe(const EventType type, Callback &&callback)
    {
        if (type >= EventType::_Count)
        {
            return 0;
        }
        if (!m_exclusiveSignals[static_cast<std::size_t>(type)].empty())
        {
            assert(false && "subscribe() called for EventType with exclusive subscriber");
            return 0;
        }
        return m_signals[static_cast<std::size_t>(type)].connect(std::move(callback));
    }

    /**
     * @brief Register a callback with RAII-based automatic cleanup
     *
     * Returns a ScopedConnection that automatically unsubscribes when
     * destroyed. This is the preferred subscription method as it prevents
     * dangling callbacks when the subscriber is destroyed.
     *
     * @param type Event type to subscribe to
     * @param callback Function to invoke when event is dispatched
     * @return ScopedConnection that unsubscribes on destruction
     *
     * @note Store the returned ScopedConnection as a class member
     * @warning Must be called from main context (not ISR-safe)
     */
    [[nodiscard]] ScopedConnection subscribeScoped(const EventType type, Callback &&callback)
    {
        if (type >= EventType::_Count)
        {
            return {};
        }
        if (!m_exclusiveSignals[static_cast<std::size_t>(type)].empty())
        {
            assert(false && "subscribeScoped() called for EventType with exclusive subscriber");
            return {};
        }
        return m_signals[static_cast<std::size_t>(type)].connectScoped(std::move(callback));
    }

    [[nodiscard]] Subscription subscribeScopedAny(const EventType type, Callback &&callback)
    {
        return Subscription{subscribeScoped(type, std::move(callback))};
    }

    /**
     * @brief Register a single-subscriber callback with move delivery
     *
     * Only one exclusive subscriber is allowed per event type. If a normal
     * subscriber already exists, this returns 0. Events for this type will
     * be delivered by move into the callback during dispatch().
     *
     * @note Use when you want to move large payloads without copying.
     */
    [[nodiscard]] ExclusiveConnection subscribeExclusive(const EventType type, ExclusiveCallback &&callback)
    {
        if (type >= EventType::_Count)
        {
            return 0;
        }
        if (!m_exclusiveSignals[static_cast<std::size_t>(type)].empty())
        {
            assert(false && "subscribeExclusive() called when exclusive subscriber already exists");
            return 0;
        }
        if (!m_signals[static_cast<std::size_t>(type)].empty())
        {
            assert(false && "subscribeExclusive() called when normal subscribers already exist");
            return 0;
        }
        return m_exclusiveSignals[static_cast<std::size_t>(type)].connect(std::move(callback));
    }

    [[nodiscard]] ExclusiveScopedConnection subscribeExclusiveScoped(const EventType type, ExclusiveCallback &&callback)
    {
        if (type >= EventType::_Count)
        {
            return {};
        }
        if (!m_exclusiveSignals[static_cast<std::size_t>(type)].empty())
        {
            assert(false && "subscribeExclusiveScoped() called when exclusive subscriber already exists");
            return {};
        }
        if (!m_signals[static_cast<std::size_t>(type)].empty())
        {
            assert(false && "subscribeExclusiveScoped() called when normal subscribers already exist");
            return {};
        }
        return m_exclusiveSignals[static_cast<std::size_t>(type)].connectScoped(std::move(callback));
    }

    [[nodiscard]] Subscription subscribeExclusiveScopedAny(const EventType type, ExclusiveCallback &&callback)
    {
        return Subscription{subscribeExclusiveScoped(type, std::move(callback))};
    }

    /**
     * @brief Remove a subscription using its connection handle
     *
     * @param type Event type the connection was registered for
     * @param connection Handle returned by subscribe()
     *
     * @note No-op if connection is invalid or already unsubscribed
     * @note Prefer ScopedConnection over manual unsubscribe
     * @warning Must be called from main context (not ISR-safe)
     */
    void unsubscribe(const EventType type, const Connection connection)
    {
        if (type >= EventType::_Count)
        {
            return;
        }
        m_signals[static_cast<std::size_t>(type)].disconnect(connection);
    }

    void unsubscribeExclusive(const EventType type, const ExclusiveConnection connection)
    {
        if (type >= EventType::_Count)
        {
            return;
        }
        m_exclusiveSignals[static_cast<std::size_t>(type)].disconnect(connection);
    }

    /**
     * @brief Queue an event for asynchronous delivery
     *
     * Adds the event to the internal queue for the event's type.
     * Subscribers are NOT invoked immediately - events are delivered
     * during the next dispatch() call.
     *
     * @param event Event to publish (moved into queue)
     * @return true if queued successfully, false if queue full or invalid type
     *
     * @note ISR-safe: can be called from interrupt handlers
     * @note Events are delivered in FIFO order per event type
     *
     * @par Complexity
     * O(1) amortized for queue insertion
     */
    bool publish(Event &&event)
    {
        if (event.type >= EventType::_Count)
        {
            return false;
        }
        const auto idx = static_cast<std::size_t>(event.type);
        assert(m_exclusiveSignals[idx].empty() || m_signals[idx].empty());
        if (!m_exclusiveSignals[idx].empty())
        {
            return m_exclusiveSignals[idx].publish(std::move(event));
        }
        return m_signals[idx].publish(std::move(event));
    }

    /**
     * @brief Deleted overload to prevent accidental copies
     *
     * @param event Event to publish
     * @return false always
     *
     * @note Use rvalue reference overload to avoid copies
     */
    bool publish(const Event &event) = delete;

    /**
     * @brief Convenience overload to publish event without payload
     *
     * @param type Event type to publish
     * @return true if queued successfully
     */
    bool publish(const EventType type)
    {
        return publish(static_cast<Event>(type));
    }

    /**
     * @brief Deliver all queued events to their subscribers
     *
     * Processes all pending events across all event types, invoking
     * registered callbacks for each. Events published during dispatch
     * are queued for the next dispatch cycle.
     *
     * @return Total number of events dispatched
     *
     * @warning Must be called from main loop (not ISR-safe)
     * @warning Callbacks may throw - exceptions propagate to caller
     *
     * @par Complexity
     * O(E * S) where E = pending events, S = avg subscribers per type
     */
    std::size_t dispatch()
    {
        std::size_t totalDispatched{0};
        for (std::size_t i = 0; i < m_signals.size(); ++i)
        {
            if (!m_exclusiveSignals[i].empty())
            {
                totalDispatched += m_exclusiveSignals[i].dispatchMoveSingle();
            }
            else
            {
                totalDispatched += m_signals[i].dispatch();
            }
        }
        return totalDispatched;
    }

    /**
     * @brief Get count of events awaiting dispatch
     *
     * Useful for monitoring event bus saturation and debugging
     * event flow issues.
     *
     * @return Total pending events across all types
     */
    [[nodiscard]] std::size_t pendingCount() const
    {
        std::size_t total{0};
        for (const auto &signal: m_signals)
        {
            total += signal.pendingCount();
        }
        for (const auto &signal: m_exclusiveSignals)
        {
            total += signal.pendingCount();
        }
        return total;
    }

    /**
     * @brief Total dropped events across all signals (overflow)
     */
    [[nodiscard]] std::size_t droppedCount() const
    {
        std::size_t total{0};
        for (const auto &signal: m_signals)
        {
            total += signal.droppedCount();
        }
        for (const auto &signal: m_exclusiveSignals)
        {
            total += signal.droppedCount();
        }
        return total;
    }

    /**
     * @brief Sum of per-signal max pending counts (coarse peak indicator)
     */
    [[nodiscard]] std::size_t maxPendingCount() const
    {
        std::size_t total{0};
        for (const auto &signal: m_signals)
        {
            total += signal.maxPendingCount();
        }
        for (const auto &signal: m_exclusiveSignals)
        {
            total += signal.maxPendingCount();
        }
        return total;
    }

    /**
     * @brief Reset drop/max stats on all signals
     */
    void resetStats()
    {
        for (auto &signal: m_signals)
        {
            signal.resetStats();
        }
        for (auto &signal: m_exclusiveSignals)
        {
            signal.resetStats();
        }
    }

private:
    std::array<SignalType, static_cast<std::size_t>(EventType::_Count)> m_signals;
    std::array<ExclusiveSignalType, static_cast<std::size_t>(EventType::_Count)> m_exclusiveSignals;
};
} // namespace isic

#endif // ISIC_CORE_EVENTBUS_HPP
