#ifndef HARDWARE_EVENTBUS_HPP
#define HARDWARE_EVENTBUS_HPP

/**
 * @file EventBus.hpp
 * @brief High-performance event bus for inter-component communication.
 *
 * The EventBus provides a thread-safe, priority-aware publish-subscribe
 * system for decoupled component communication on FreeRTOS.
 *
 * @note Events are heap-allocated and ownership is transferred to the EventBus
 *       when published. The EventBus deletes events after dispatching to all
 *       listeners. This avoids std::string corruption through FreeRTOS queue memcpy.
 */

#include <Arduino.h>

#include <vector>
#include <memory>

#include "core/Types.hpp"

namespace isic {
    /**
     * @brief Event listener interface.
     *
     * Components that want to receive events must implement this interface
     * and register with the EventBus.
     *
     * @note All event listeners must implement this interface.
     */
    class IEventListener {
    public:
        virtual ~IEventListener() = default;
        virtual void onEvent(const Event& event) = 0;
    };

    /**
     * @brief Event filter to allow selective event subscription.
     *
     * Uses a bitmask for O(1) filtering of up to 64 event types.
     *
     * @note All operations are constexpr and noexcept for compile-time optimization.
     */
    struct EventFilter {
        std::uint64_t eventTypeMask{~0ULL};  // Bitmask of EventTypes to receive

        [[nodiscard]] constexpr bool accepts(EventType type) const noexcept {
            return (eventTypeMask & (1ULL << static_cast<std::uint8_t>(type))) != 0;
        }

        [[nodiscard]] static constexpr EventFilter all() noexcept {
            return EventFilter{~0ULL};
        }

        [[nodiscard]] static constexpr EventFilter none() noexcept {
            return EventFilter{0};
        }

        [[nodiscard]] static constexpr EventFilter only(EventType type) noexcept {
            return EventFilter{1ULL << static_cast<std::uint8_t>(type)};
        }

        constexpr EventFilter& include(EventType type) noexcept {
            eventTypeMask |= (1ULL << static_cast<std::uint8_t>(type));
            return *this;
        }

        constexpr EventFilter& exclude(EventType type) noexcept {
            eventTypeMask &= ~(1ULL << static_cast<std::uint8_t>(type));
            return *this;
        }
    };

    /**
     * @brief High-performance EventBus with priority support.
     *
     * Features:
     *  - Thread-safe publish-subscribe model
     *  - Event prioritization (high-priority queue)
     *  - Listener filtering by event type
     *  - Configurable queue sizes and task parameters
     *
     * @note Designed for FreeRTOS-based systems.
     */
    class EventBus {
    public:
        using ListenerId = std::uint32_t;

        /**
         * @brief Configuration parameters for EventBus.
         */
        struct Config {
            std::uint32_t queueLength{64};
            std::uint32_t highPriorityQueueLength{16};
            std::uint32_t taskStackSize{4096};
            std::uint8_t taskPriority{2};
            std::uint8_t taskCore{0};
        };

        explicit EventBus(const Config& cfg);
        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;
        EventBus(EventBus&&) = delete;
        EventBus& operator=(EventBus&&) = delete;
        ~EventBus();

        /**
         * @brief Start the event bus task.
         */
        void start();

        /**
         * @brief Stop the event bus task.
         */
        void stop();

        /**
         * @brief Subscribe a listener to events.
         * @param listener Pointer to listener
         * @param filter Optional filter for event types
         * @return Listener ID for unsubscription
         */
        [[nodiscard]] ListenerId subscribe(IEventListener* listener, EventFilter filter = EventFilter::all());

        /**
         * @brief Unsubscribe a listener.
         * @param id Listener ID returned from subscribe
         */
        void unsubscribe(ListenerId id);

        /**
         * @brief Publish an event (non-blocking by default).
         *
         * Takes ownership of the event via unique_ptr. The EventBus will
         * automatically clean up the event after dispatching to all listeners.
         * If queueing fails, the event is cleaned up immediately.
         *
         * Usage:
         *   bus.publish(std::make_unique<Event>(Event{...}));
         *
         * @param event unique_ptr to event (ownership transferred)
         * @param ticksToWait How long to wait if queue is full (0 = non-blocking)
         * @return true if event was queued
         */
        [[nodiscard]] bool publish(std::unique_ptr<Event> event, TickType_t ticksToWait = 0);

        /**
         * @brief Publish a high-priority event.
         *
         * High-priority events are processed before normal events.
         * Takes ownership of the event via unique_ptr.
         *
         * @param event unique_ptr to event (ownership transferred)
         * @param ticksToWait How long to wait if queue is full (0 = non-blocking)
         * @return true if event was queued
         */
        [[nodiscard]] bool publishHighPriority(std::unique_ptr<Event> event, TickType_t ticksToWait = 0);

        /**
         * @brief Check if event bus is running.
         */
        [[nodiscard]] bool isRunning() const noexcept {
            return m_running;
        }

    private:
        static void eventTaskThunk(void* arg);
        void eventTask();

        bool dispatchEvent(const Event& event);

        struct Subscriber {
            ListenerId id{0};
            IEventListener* listener{nullptr};
            EventFilter filter{EventFilter::all()};
        };

        std::vector<Subscriber> m_listeners{};

        ListenerId m_nextId{1};
        QueueHandle_t m_queue{nullptr};
        QueueHandle_t m_highPriorityQueue{nullptr};
        TaskHandle_t m_taskHandle{nullptr};
        SemaphoreHandle_t m_listenersMutex{nullptr};
        Config m_config{};
        volatile bool m_running{false};
    };
}  // namespace isic

#endif  // HARDWARE_EVENTBUS_HPP
