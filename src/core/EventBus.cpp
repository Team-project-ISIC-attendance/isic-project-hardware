#include "core/EventBus.hpp"
#include "core/Logger.hpp"

namespace isic {

    namespace {
        constexpr auto* EVENT_TAG = "EventBus";
        constexpr std::size_t MAX_LISTENERS = 32;
    }

    EventBus::EventBus(const Config& cfg) : m_config(cfg) {
        m_queue = xQueueCreate(cfg.queueLength, sizeof(Event));
        m_highPriorityQueue = xQueueCreate(cfg.highPriorityQueueLength, sizeof(Event));
        m_listenersMutex = xSemaphoreCreateMutex();
        m_metricsMutex = xSemaphoreCreateMutex();
        m_listeners.reserve(MAX_LISTENERS);
    }

    EventBus::~EventBus() {
        stop();

        if (m_queue) {
            vQueueDelete(m_queue);
            m_queue = nullptr;
        }

        if (m_highPriorityQueue) {
            vQueueDelete(m_highPriorityQueue);
            m_highPriorityQueue = nullptr;
        }

        if (m_listenersMutex) {
            vSemaphoreDelete(m_listenersMutex);
            m_listenersMutex = nullptr;
        }

        if (m_metricsMutex) {
            vSemaphoreDelete(m_metricsMutex);
            m_metricsMutex = nullptr;
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

        LOG_INFO(EVENT_TAG, "EventBus started: queue=%zu, hiPri=%zu",
                 m_config.queueLength, m_config.highPriorityQueueLength);
    }

    void EventBus::stop() {
        m_running = false;

        if (m_taskHandle) {
            // Give task time to exit cleanly
            vTaskDelay(pdMS_TO_TICKS(200));
            // Force delete if still running
            if (m_taskHandle) {
                vTaskDelete(m_taskHandle);
            }
            m_taskHandle = nullptr;
        }
    }

    EventBus::ListenerId EventBus::subscribe(IEventListener* listener, EventFilter filter) {
        if (!listener) {
            return 0;
        }

        if (xSemaphoreTake(m_listenersMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(EVENT_TAG, "Failed to acquire mutex for subscribe");
            return 0;
        }

        if (m_listeners.size() >= MAX_LISTENERS) {
            xSemaphoreGive(m_listenersMutex);
            LOG_ERROR(EVENT_TAG, "Max listeners reached (%zu)", MAX_LISTENERS);
            return 0;
        }

        const auto id = m_nextId++;
        m_listeners.push_back(Subscriber{id, listener, filter});

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.listenerCount = m_listeners.size();
            xSemaphoreGive(m_metricsMutex);
        }

        xSemaphoreGive(m_listenersMutex);

        LOG_DEBUG(EVENT_TAG, "Subscribed listener id=%u, total=%zu", id, m_listeners.size());
        return id;
    }

    void EventBus::unsubscribe(const ListenerId id) {
        if (xSemaphoreTake(m_listenersMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(EVENT_TAG, "Failed to acquire mutex for unsubscribe");
            return;
        }

        const auto sizeBefore = m_listeners.size();
        std::erase_if(m_listeners, [id](const Subscriber& s) { return s.id == id; });

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.listenerCount = m_listeners.size();
            xSemaphoreGive(m_metricsMutex);
        }

        xSemaphoreGive(m_listenersMutex);

        if (m_listeners.size() < sizeBefore) {
            LOG_DEBUG(EVENT_TAG, "Unsubscribed listener id=%u, remaining=%zu",
                     id, m_listeners.size());
        }
    }

    bool EventBus::publish(const Event& event, const TickType_t ticksToWait) {
        if (!m_queue) {
            return false;
        }

        // Use high-priority queue for critical events
        if (event.priority >= EventPriority::E_HIGH && m_highPriorityQueue) {
            return publishHighPriority(event, ticksToWait);
        }

        if (xQueueSend(m_queue, &event, ticksToWait) != pdPASS) {
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.eventsDropped++;
                xSemaphoreGive(m_metricsMutex);
            }
            LOG_WARNING(EVENT_TAG, "Event queue full, dropping event type=%u",
                       static_cast<std::uint8_t>(event.type));
            return false;
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.eventsPublished++;
            m_metrics.currentQueueSize = uxQueueMessagesWaiting(m_queue);
            if (m_metrics.currentQueueSize > m_metrics.peakQueueSize) {
                m_metrics.peakQueueSize = m_metrics.currentQueueSize;
            }
            xSemaphoreGive(m_metricsMutex);
        }

        return true;
    }

    bool EventBus::publishHighPriority(const Event& event, TickType_t ticksToWait) {
        if (!m_highPriorityQueue) {
            // Fall back to normal queue
            return publish(event, ticksToWait);
        }

        if (xQueueSendToFront(m_highPriorityQueue, &event, ticksToWait) != pdPASS) {
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                m_metrics.eventsDropped++;
                xSemaphoreGive(m_metricsMutex);
            }
            LOG_WARNING(EVENT_TAG, "High priority queue full, type=%u",
                       static_cast<std::uint8_t>(event.type));
            return false;
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.eventsPublished++;
            xSemaphoreGive(m_metricsMutex);
        }

        return true;
    }

    EventBus::Metrics EventBus::getMetrics() const {
        Metrics result{};
        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            result = m_metrics;
            if (m_queue) {
                result.currentQueueSize = uxQueueMessagesWaiting(m_queue);
            }
            xSemaphoreGive(m_metricsMutex);
        }
        return result;
    }

    void EventBus::eventTaskThunk(void* arg) {
        static_cast<EventBus*>(arg)->eventTask();
    }

    void EventBus::eventTask() {
        LOG_INFO(EVENT_TAG, "Event bus task started");

        Event event{};

        while (m_running) {
            // First check high-priority queue (non-blocking)
            bool hasEvent = false;

            if (m_highPriorityQueue) {
                hasEvent = (xQueueReceive(m_highPriorityQueue, &event, 0) == pdTRUE);
            }

            // If no high-priority event, check normal queue with timeout
            if (!hasEvent) {
                hasEvent = (xQueueReceive(m_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE);
            }

            if (hasEvent) {
                dispatchEvent(event);
            }
        }

        LOG_INFO(EVENT_TAG, "Event bus task stopped");
        vTaskDelete(nullptr);
    }

    bool EventBus::dispatchEvent(const Event& event) {
        // Make a copy of listeners to avoid holding mutex during callbacks
        if (xSemaphoreTake(m_listenersMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            LOG_WARNING(EVENT_TAG, "Could not acquire mutex for dispatch");
            return false;
        }

        auto listenersCopy = m_listeners;
        xSemaphoreGive(m_listenersMutex);

        std::uint32_t delivered = 0;

        for (const auto& [id, listener, filter] : listenersCopy) {
            if (listener && filter.accepts(event.type)) {
                listener->onEvent(event);
                ++delivered;
            }
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.eventsDelivered += delivered;
            m_metrics.currentQueueSize = uxQueueMessagesWaiting(m_queue);
            xSemaphoreGive(m_metricsMutex);
        }

        return delivered > 0;
    }

}  // namespace isic
