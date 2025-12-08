#include "core/EventBus.hpp"
#include "core/Logger.hpp"

#include <memory>

namespace isic {
    namespace {
        constexpr auto *EVENT_TAG{"EventBus"};
        constexpr std::uint32_t MAX_LISTENERS{32};
    }

    EventBus::EventBus(const Config &cfg) : m_config(cfg) {
        // Queues store Event* pointers to avoid std::string corruption via memcpy
        m_queue = xQueueCreate(cfg.queueLength, sizeof(Event*));
        m_highPriorityQueue = xQueueCreate(cfg.highPriorityQueueLength, sizeof(Event*));
        m_listenersMutex = xSemaphoreCreateMutex();
        m_listeners.reserve(MAX_LISTENERS);
    }

    EventBus::~EventBus() {
        stop();

        // Drain and delete any remaining events in the queues
        Event* evt{nullptr};
        if (m_queue) {
            while (xQueueReceive(m_queue, &evt, 0) == pdTRUE) {
                delete evt;
            }
            vQueueDelete(m_queue);
            m_queue = nullptr;
        }

        if (m_highPriorityQueue) {
            while (xQueueReceive(m_highPriorityQueue, &evt, 0) == pdTRUE) {
                delete evt;
            }
            vQueueDelete(m_highPriorityQueue);
            m_highPriorityQueue = nullptr;
        }

        if (m_listenersMutex) {
            vSemaphoreDelete(m_listenersMutex);
            m_listenersMutex = nullptr;
        }
    }

    void EventBus::start() {
        if (m_taskHandle != nullptr) {
            return;
        }

        m_running = true;

        xTaskCreatePinnedToCore(
            &EventBus::eventTaskThunk,
            EVENT_TAG,
            m_config.taskStackSize,
            this,
            m_config.taskPriority,
            &m_taskHandle,
            m_config.taskCore
        );

        LOG_INFO(EVENT_TAG, "EventBus started: queue=%u, hiPri=%u", unsigned{m_config.queueLength}, unsigned{m_config.highPriorityQueueLength});
    }

    void EventBus::stop() {
        m_running = false;

        if (m_taskHandle) {
            // Give task time to exit cleanly
            vTaskDelay(pdMS_TO_TICKS(200));
            // Force delete if still running
            vTaskDelete(m_taskHandle);
            m_taskHandle = nullptr;
        }
    }

    EventBus::ListenerId EventBus::subscribe(IEventListener *listener, EventFilter filter) {
        if (!listener) {
            return 0;
        }

        if (xSemaphoreTake(m_listenersMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(EVENT_TAG, "Failed to acquire mutex for subscribe");
            return 0;
        }

        if (m_listeners.size() >= MAX_LISTENERS) {
            xSemaphoreGive(m_listenersMutex);
            LOG_ERROR(EVENT_TAG, "Max listeners reached (%u)", unsigned{MAX_LISTENERS});
            return 0;
        }

        const auto id{m_nextId++};
        m_listeners.push_back(Subscriber{id, listener, filter});
        xSemaphoreGive(m_listenersMutex);
        LOG_DEBUG(EVENT_TAG, "Subscribed listener id=%u, total=%u", unsigned{id}, unsigned{m_listeners.size()});
        return id;
    }

    void EventBus::unsubscribe(const ListenerId id) {
        if (xSemaphoreTake(m_listenersMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(EVENT_TAG, "Failed to acquire mutex for unsubscribe");
            return;
        }

        const auto sizeBefore{m_listeners.size()};
        std::erase_if(m_listeners, [id](const Subscriber &s) { return s.id == id; });
        xSemaphoreGive(m_listenersMutex);

        if (m_listeners.size() < sizeBefore) {
            LOG_DEBUG(EVENT_TAG, "Unsubscribed listener id=%u, remaining=%u", unsigned{id}, unsigned{m_listeners.size()});
        }
    }

    bool EventBus::publish(std::unique_ptr<Event> event, const TickType_t ticksToWait) {
        if (!event) {
            return false;
        }

        if (!m_queue) {
            // unique_ptr automatically cleans up
            return false;
        }

        // Use high-priority queue for critical events
        if (event->priority >= EventPriority::E_HIGH && m_highPriorityQueue) {
            return publishHighPriority(std::move(event), ticksToWait);
        }

        // Release ownership to raw pointer for FreeRTOS queue
        Event* rawPtr = event.release();
        if (xQueueSend(m_queue, &rawPtr, ticksToWait) != pdPASS) {
            LOG_WARNING(EVENT_TAG, "Event queue full, dropping event type=%u", static_cast<unsigned>(rawPtr->type));
            delete rawPtr;  // Re-take ownership and clean up
            return false;
        }

        return true;
    }

    bool EventBus::publishHighPriority(std::unique_ptr<Event> event, const TickType_t ticksToWait) {
        if (!event) {
            return false;
        }

        if (!m_highPriorityQueue) {
            // Fall back to normal queue
            return publish(std::move(event), ticksToWait);
        }

        // Release ownership to raw pointer for FreeRTOS queue
        Event* rawPtr = event.release();
        if (xQueueSendToFront(m_highPriorityQueue, &rawPtr, ticksToWait) != pdPASS) {
            LOG_WARNING(EVENT_TAG, "High priority queue full, type=%u", static_cast<unsigned>(rawPtr->type));
            delete rawPtr;  // Re-take ownership and clean up
            return false;
        }

        return true;
    }

    void EventBus::eventTaskThunk(void *arg) {
        static_cast<EventBus *>(arg)->eventTask();
    }

    void EventBus::eventTask() {
        LOG_INFO(EVENT_TAG, "Event bus task started");

        Event* rawEvent{nullptr};

        while (m_running) {
            // First check high-priority queue (non-blocking)
            bool hasEvent{false};

            if (m_highPriorityQueue) {
                hasEvent = (xQueueReceive(m_highPriorityQueue, &rawEvent, 0) == pdTRUE);
            }

            // If no high-priority event, check normal queue with timeout
            if (!hasEvent) {
                hasEvent = (xQueueReceive(m_queue, &rawEvent, pdMS_TO_TICKS(50)) == pdTRUE);
            }

            // Wrap in unique_ptr for automatic cleanup (RAII)
            if (hasEvent && rawEvent) {
                std::unique_ptr<Event> event{rawEvent};
                rawEvent = nullptr;
                dispatchEvent(*event);
                // unique_ptr automatically deletes when going out of scope
            }
        }

        LOG_INFO(EVENT_TAG, "Event bus task stopped");
        vTaskDelete(nullptr);
    }

    bool EventBus::dispatchEvent(const Event &event) {
        // Make a copy of listeners to avoid holding mutex during callbacks
        if (xSemaphoreTake(m_listenersMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            LOG_WARNING(EVENT_TAG, "Could not acquire mutex for dispatch");
            return false;
        }

        auto listenersCopy{m_listeners};
        xSemaphoreGive(m_listenersMutex);

        std::size_t delivered{0};

        for (const auto &[id, listener, filter]: listenersCopy) {
            if (listener && filter.accepts(event.type)) {
                listener->onEvent(event);
                ++delivered;
            }
        }

        return delivered > 0;
    }
}
