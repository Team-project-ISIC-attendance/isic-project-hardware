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
        return m_signals[static_cast<std::size_t>(type)].connectScoped(std::move(callback));
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

    /**
     * @brief Queue an event for asynchronous delivery
     *
     * Adds the event to the internal queue for the event's type.
     * Subscribers are NOT invoked immediately - events are delivered
     * during the next dispatch() call.
     *
     * @param event Event to publish (copied into queue)
     * @return true if queued successfully, false if queue full or invalid type
     *
     * @note ISR-safe: can be called from interrupt handlers
     * @note Events are delivered in FIFO order per event type
     *
     * @par Complexity
     * O(1) amortized for queue insertion
     */
    bool publish(const Event &event)
    {
        if (event.type >= EventType::_Count)
        {
            return false;
        }
        return m_signals[static_cast<std::size_t>(event.type)].publish(event);
    }

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
        for (auto &signal: m_signals)
        {
            totalDispatched += signal.dispatch();
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
        return total;
    }

private:
    std::array<SignalType, static_cast<std::size_t>(EventType::_Count)> m_signals;
};
} // namespace isic

#endif // ISIC_CORE_EVENTBUS_HPP
