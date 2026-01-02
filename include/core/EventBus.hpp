#ifndef ISIC_CORE_EVENTBUS_HPP
#define ISIC_CORE_EVENTBUS_HPP

#include "common/Types.hpp"
#include "core/Signal.hpp"

#include <array>

namespace isic
{
/**
 * @brief Async Event Bus with publish/subscribe pattern
 *
 * Usage Pattern:
 * @code
 * // Initialization
 * EventBus bus;
 * bus.subscribe(EventType::WifiConnected, [](const Event& e) {
 *     // Handle event
 * });
 *
 * // Publishing (from anywhere, including ISR)
 * bus.publish(EventType::WifiConnected);
 *
 * // Main loop (REQUIRED)
 * void loop() {
 *     bus.dispatch(); // Process all pending events
 *     // ... other loop tasks
 * }
 * @endcode
 *
 * @warning dispatch() MUST be called regularly from main loop
 */
class EventBus
{
public:
    static constexpr auto TAG{"EventBus"};

    EventBus() = default;
    ~EventBus() = default;

    /**
     * @brief Scoped connection type for RAII management
     */
    using ScopedConnection = Signal<const Event &>::ScopedConnection;

    /**
     * @brief Subscribe to a specific event type
     *
     * @param type EventType to listen for
     * @param callback Function to execute asynchronously when event is dispatched
     * @return Connection handle for unsubscription
     *
     * @note Callback executes later during dispatch(), not immediately on publish()
     */
    [[nodiscard]] Signal<const Event &>::Connection subscribe(const EventType type, Signal<const Event &>::Callback callback)
    {
        if (type >= EventType::_Count)
        {
            return 0; // Invalid type, no subscription
        }
        return m_signals[static_cast<std::size_t>(type)].connect(std::move(callback));
    }

    /**
     * @brief Subscribe and return a scoped connection (auto-unsubscribe on destruction)
     *
     * @param type EventType to listen for
     * @param callback Function to execute asynchronously
     * @return ScopedConnection for automatic management
     */
    [[nodiscard]] ScopedConnection subscribeScoped(const EventType type, Signal<const Event &>::Callback callback)
    {
        if (type >= EventType::_Count)
        {
            return {}; // Return empty ScopedConnection, no subscription
        }
        return m_signals[static_cast<std::size_t>(type)].connectScoped(std::move(callback));
    }

    /**
     * @brief Unsubscribe from an event
     *
     * @param type EventType was subscribed to
     * @param connection Connection handle returned by subscribe
     */
    void unsubscribe(const EventType type, const Signal<const Event &>::Connection connection)
    {
        if (type >= EventType::_Count)
        {
            return; // Invalid type, nothing to unsubscribe
        }

        m_signals[static_cast<std::size_t>(type)].disconnect(connection);
    }

    /**
     * @brief Publish an event for async dispatch
     *
     * @param event The event object to publish
     * @return true if queued successfully, false if buffer full
     *
     * @note Does NOT invoke subscribers immediately - call dispatch() to process
     * @note Safe from ISR, callbacks, or any execution context
     *
     * @warning ESP8266 ISR: Event must be lightweight (no heap allocation in copy)
     */
    bool publish(const Event &event)
    {
        if (event.eventType >= EventType::_Count)
        {
            return false; // Invalid event type
        }
        return m_signals[static_cast<std::size_t>(event.eventType)].publish(event);
    }

    /**
     * @brief Helper to publish simple event type without data
     *
     * @param type Event type to publish
     * @return true if queued successfully
     */
    bool publish(const EventType type)
    {
        return publish(Event(type));
    }

    /**
     * @brief Dispatch all pending events to subscribers
     *
     * @return Total number of events dispatched across all types
     *
     * @note Call this from loop() or main task
     * @note Deterministic execution time (processes all pending, then returns)
     * @note Safe to call publish() from within callbacks (queued for next dispatch)
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
     * @brief Get total pending events across all types
     *
     * @return Number of events awaiting dispatch
     *
     * @note Useful for monitoring event bus saturation
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
    std::array<Signal<const Event &>, static_cast<std::size_t>(EventType::_Count)> m_signals;
};
}

#endif // ISIC_CORE_EVENTBUS_HPP
