#include "services/PowerService.hpp"
#include "core/Logger.hpp"

#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <soc/rtc.h>

namespace isic {

    namespace {
        constexpr auto* POWER_TAG = "PowerService";
        constexpr auto* POWER_TASK_NAME = "power_mgr";
        constexpr std::uint32_t POWER_TASK_STACK_SIZE = 4096;
        constexpr std::uint8_t POWER_TASK_PRIORITY = 2;
        constexpr std::uint8_t POWER_TASK_CORE = 0;
        constexpr std::uint32_t POWER_TASK_LOOP_MS = 100;

        constexpr std::size_t MAX_WAKE_LOCKS = 16;
    }

    PowerService::PowerService(EventBus& bus) : m_bus(bus) {
        m_wakeLockMutex = xSemaphoreCreateMutex();
        m_wakeLocks.reserve(MAX_WAKE_LOCKS);
        m_subscriptionId = m_bus.subscribe(this);
    }

    PowerService::~PowerService() {
        stop();
        if (m_wakeLockMutex) {
            vSemaphoreDelete(m_wakeLockMutex);
            m_wakeLockMutex = nullptr;
        }
        m_bus.unsubscribe(m_subscriptionId);
    }

    Status PowerService::begin(const AppConfig& cfg) {
        m_cfg = &cfg.power;
        m_lastActivityMs.store(millis());
        m_lastWakeupReason = determineWakeupReason();

        LOG_INFO(POWER_TAG, "PowerService starting, wakeup reason: %s",
                 toString(m_lastWakeupReason));

        // Configure wake sources if sleep is enabled
        if (m_cfg->sleepEnabled) {
            configureWakeSources();
        }

        // Set initial CPU frequency
        if (m_cfg->cpuFrequencyMhz > 0) {
            setCpuFrequency(m_cfg->cpuFrequencyMhz);
        }

        // Publish wakeup event
        const Event wakeEvt{
            .type = EventType::WakeupOccurred,
            .payload = WakeupEvent{
                .reason = m_lastWakeupReason,
                .sleepDurationMs = 0,  // Unknown on first boot
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(wakeEvt);

        // Start power management task
        m_running.store(true);
        xTaskCreatePinnedToCore(
            &PowerService::powerTaskThunk,
            POWER_TASK_NAME,
            POWER_TASK_STACK_SIZE,
            this,
            POWER_TASK_PRIORITY,
            &m_taskHandle,
            POWER_TASK_CORE
        );

        LOG_INFO(POWER_TAG, "PowerService initialized, sleep=%s, type=%s",
                 m_cfg->sleepEnabled ? "enabled" : "disabled",
                 m_cfg->sleepType == PowerConfig::SleepType::Deep ? "deep" : "light");

        return Status::OK();
    }

    void PowerService::stop() {
        m_running.store(false);
        if (m_taskHandle) {
            // Give task time to exit cleanly
            vTaskDelay(pdMS_TO_TICKS(200));
            m_taskHandle = nullptr;
        }
    }

    WakeLockHandle PowerService::requestWakeLock(const char* name) {
        if (xSemaphoreTake(m_wakeLockMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(POWER_TAG, "Failed to acquire wake lock mutex");
            return WakeLockHandle{};
        }

        // Find an unused slot or create new entry
        WakeLockEntry* entry = nullptr;
        for (auto& wl : m_wakeLocks) {
            if (!wl.active) {
                entry = &wl;
                break;
            }
        }

        if (!entry && m_wakeLocks.size() < MAX_WAKE_LOCKS) {
            m_wakeLocks.push_back({});
            entry = &m_wakeLocks.back();
        }

        if (!entry) {
            LOG_ERROR(POWER_TAG, "Max wake locks exceeded (%zu)", MAX_WAKE_LOCKS);
            xSemaphoreGive(m_wakeLockMutex);
            return WakeLockHandle{};
        }

        const auto lockId = m_nextWakeLockId++;
        entry->id = lockId;
        entry->name = name;
        entry->acquiredAt = millis();
        entry->active = true;

        const auto activeCount = getActiveWakeLockCount();
        xSemaphoreGive(m_wakeLockMutex);

        LOG_DEBUG(POWER_TAG, "Wake lock acquired: '%s' (id=%u, active=%u)",
                  name, lockId, activeCount);

        // Publish event
        const Event evt{
            .type = EventType::WakeLockAcquired,
            .payload = WakeLockEvent{
                .lockName = name,
                .lockId = lockId,
                .totalActiveLocks = activeCount
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(evt);

        // Reset idle timer
        resetIdleTimer();

        return WakeLockHandle{lockId, name};
    }

    void PowerService::releaseWakeLock(WakeLockHandle& handle) {
        if (!handle.isValid()) {
            return;
        }

        if (xSemaphoreTake(m_wakeLockMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            LOG_ERROR(POWER_TAG, "Failed to acquire wake lock mutex for release");
            return;
        }

        bool found = false;
        for (auto& wl : m_wakeLocks) {
            if (wl.active && wl.id == handle.id) {
                wl.active = false;
                found = true;
                break;
            }
        }

        const auto activeCount = getActiveWakeLockCount();
        xSemaphoreGive(m_wakeLockMutex);

        if (found) {
            LOG_DEBUG(POWER_TAG, "Wake lock released: '%s' (id=%u, remaining=%u)",
                      handle.name, handle.id, activeCount);

            // Publish event
            const Event evt{
                .type = EventType::WakeLockReleased,
                .payload = WakeLockEvent{
                    .lockName = handle.name,
                    .lockId = handle.id,
                    .totalActiveLocks = activeCount
                },
                .timestampMs = static_cast<std::uint64_t>(millis())
            };
            (void)m_bus.publish(evt);
        } else {
            LOG_WARNING(POWER_TAG, "Wake lock not found for release: id=%u", handle.id);
        }

        handle.invalidate();
    }

    bool PowerService::hasActiveWakeLocks() const {
        return getActiveWakeLockCount() > 0;
    }

    std::uint8_t PowerService::getActiveWakeLockCount() const {
        std::uint8_t count = 0;
        // Note: This may be called while mutex is already held, so we don't lock here
        // The caller should ensure thread safety when needed
        for (const auto& wl : m_wakeLocks) {
            if (wl.active) {
                ++count;
            }
        }
        return count;
    }

    bool PowerService::enterIdleSleep() {
        if (!m_cfg || !m_cfg->sleepEnabled) {
            LOG_DEBUG(POWER_TAG, "Sleep disabled, skipping idle sleep");
            return false;
        }

        if (hasActiveWakeLocks()) {
            LOG_DEBUG(POWER_TAG, "Cannot sleep: wake locks held (%u)",
                      getActiveWakeLockCount());
            return false;
        }

        if (m_cfg->sleepType == PowerConfig::SleepType::None) {
            return false;
        }

        LOG_INFO(POWER_TAG, "Entering light sleep");

        // Publish sleep entering event
        const Event evt{
            .type = EventType::SleepEntering,
            .payload = SleepEnteringEvent{
                .targetState = PowerState::LightSleep,
                .expectedDurationMs = m_cfg->timerWakeIntervalMs
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(evt, pdMS_TO_TICKS(50));  // Brief wait for delivery

        transitionTo(PowerState::LightSleep);
        m_sleepEnteredAt = millis();

        // Configure timer wake
        if (m_cfg->wakeSourceTimerEnabled && m_cfg->timerWakeIntervalMs > 0) {
            esp_sleep_enable_timer_wakeup(m_cfg->timerWakeIntervalMs * 1000ULL);
        }

        // Enter light sleep
        const auto result = esp_light_sleep_start();
        const auto sleepDuration = millis() - m_sleepEnteredAt;

        if (result == ESP_OK) {
            m_lastWakeupReason = determineWakeupReason();
            LOG_INFO(POWER_TAG, "Woke from light sleep after %lums, reason: %s",
                     sleepDuration, toString(m_lastWakeupReason));
        } else {
            LOG_WARNING(POWER_TAG, "Light sleep failed with error: %d", result);
        }

        transitionTo(PowerState::WakingUp);
        resetIdleTimer();

        // Publish wakeup event
        const Event wakeEvt{
            .type = EventType::WakeupOccurred,
            .payload = WakeupEvent{
                .reason = m_lastWakeupReason,
                .sleepDurationMs = sleepDuration,
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(wakeEvt);

        transitionTo(PowerState::Active);
        return true;
    }

    bool PowerService::enterDeepSleep() {
        if (!m_cfg || !m_cfg->sleepEnabled) {
            LOG_DEBUG(POWER_TAG, "Sleep disabled, skipping deep sleep");
            return false;
        }

        if (hasActiveWakeLocks()) {
            LOG_WARNING(POWER_TAG, "Cannot deep sleep: wake locks held (%u)",
                        getActiveWakeLockCount());
            return false;
        }

        if (m_cfg->sleepType != PowerConfig::SleepType::Deep) {
            LOG_DEBUG(POWER_TAG, "Deep sleep not configured");
            return false;
        }

        LOG_INFO(POWER_TAG, "Entering deep sleep");

        // Publish sleep entering event
        const Event evt{
            .type = EventType::SleepEntering,
            .payload = SleepEnteringEvent{
                .targetState = PowerState::DeepSleep,
                .expectedDurationMs = m_cfg->timerWakeIntervalMs
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(evt, pdMS_TO_TICKS(100));  // Wait longer for deep sleep

        transitionTo(PowerState::DeepSleep);

        // Configure wake sources for deep sleep
        if (m_cfg->wakeSourceTimerEnabled && m_cfg->timerWakeIntervalMs > 0) {
            esp_sleep_enable_timer_wakeup(m_cfg->timerWakeIntervalMs * 1000ULL);
        }

        // PN532 GPIO wake source (if configured)
        if (m_pn532WakeConfigured && m_cfg->wakeSourcePn532Enabled) {
            const auto gpioMask = 1ULL << m_pn532IrqPin;
            esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ALL_LOW);
        }

        // This function doesn't return - device resets on wake
        esp_deep_sleep_start();

        // Should never reach here
        return true;
    }

    void PowerService::stayAwakeFor(std::uint32_t durationMs) {
        const auto until = millis() + durationMs;
        m_forcedAwakeUntilMs.store(until);
        LOG_DEBUG(POWER_TAG, "Forced awake for %ums", durationMs);
    }

    void PowerService::resetIdleTimer() {
        m_lastActivityMs.store(millis());
    }

    bool PowerService::isSleepAllowed() const {
        if (!m_cfg || !m_cfg->sleepEnabled) {
            return false;
        }

        if (hasActiveWakeLocks()) {
            return false;
        }

        const auto now = millis();
        if (now < m_forcedAwakeUntilMs.load()) {
            return false;
        }

        return getIdleTimeMs() >= m_cfg->idleTimeoutMs;
    }

    std::uint32_t PowerService::getIdleTimeMs() const {
        return millis() - m_lastActivityMs.load();
    }

    void PowerService::updateConfig(const PowerConfig& cfg) {
        m_cfg = &cfg;

        if (m_cfg->sleepEnabled) {
            configureWakeSources();
        }

        if (m_cfg->cpuFrequencyMhz > 0) {
            setCpuFrequency(m_cfg->cpuFrequencyMhz);
        }

        LOG_INFO(POWER_TAG, "Power config updated: sleep=%s, type=%s, idle=%ums",
                 m_cfg->sleepEnabled ? "enabled" : "disabled",
                 m_cfg->sleepType == PowerConfig::SleepType::Deep ? "deep" : "light",
                 m_cfg->idleTimeoutMs);
    }

    void PowerService::configurePn532WakeSource(std::uint8_t pin) {
        m_pn532IrqPin = pin;

        // Configure the pin for RTC GPIO wake
        if (rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(pin))) {
            rtc_gpio_init(static_cast<gpio_num_t>(pin));
            rtc_gpio_set_direction(static_cast<gpio_num_t>(pin), RTC_GPIO_MODE_INPUT_ONLY);
            rtc_gpio_pullup_en(static_cast<gpio_num_t>(pin));
            rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(pin));

            m_pn532WakeConfigured = true;
            LOG_INFO(POWER_TAG, "PN532 wake source configured on GPIO %u", pin);
        } else {
            LOG_WARNING(POWER_TAG, "GPIO %u is not a valid RTC GPIO for wake", pin);
            m_pn532WakeConfigured = false;
        }
    }

    void PowerService::onEvent(const Event& event) {
        switch (event.type) {
            case EventType::ConfigUpdated: {
                if (const auto* ce = std::get_if<ConfigUpdatedEvent>(&event.payload)) {
                    if (ce->config) {
                        updateConfig(ce->config->power);
                    }
                }
                break;
            }
            case EventType::CardScanned:
            case EventType::AttendanceRecorded:
            case EventType::MqttMessageReceived:
                // Reset idle timer on activity
                resetIdleTimer();
                break;
            default:
                break;
        }
    }

    void PowerService::powerTaskThunk(void* arg) {
        static_cast<PowerService*>(arg)->powerTask();
    }

    void PowerService::powerTask() {
        LOG_DEBUG(POWER_TAG, "Power management task started");

        while (m_running.load()) {
            const auto currentState = m_currentState.load();

            if (currentState == PowerState::Active) {
                // Check if we should transition to idle
                if (!hasActiveWakeLocks() && getIdleTimeMs() > 5000) {
                    // No activity for 5s, transition to idle
                    transitionTo(PowerState::Idle);
                }
            } else if (currentState == PowerState::Idle) {
                // Check if we should sleep
                if (isSleepAllowed()) {
                    if (m_cfg && m_cfg->sleepType == PowerConfig::SleepType::Deep) {
                        enterDeepSleep();
                    } else {
                        enterIdleSleep();
                    }
                } else if (hasActiveWakeLocks()) {
                    // Activity detected, back to active
                    transitionTo(PowerState::Active);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(POWER_TASK_LOOP_MS));
        }

        LOG_DEBUG(POWER_TAG, "Power management task exiting");
        vTaskDelete(nullptr);
    }

    void PowerService::transitionTo(PowerState newState) {
        const auto oldState = m_currentState.exchange(newState);

        if (oldState != newState) {
            LOG_DEBUG(POWER_TAG, "Power state: %s -> %s",
                      toString(oldState), toString(newState));

            const Event evt{
                .type = EventType::PowerStateChanged,
                .payload = PowerStateChangedEvent{
                    .oldState = oldState,
                    .newState = newState,
                    .timestampMs = static_cast<std::uint64_t>(millis())
                },
                .timestampMs = static_cast<std::uint64_t>(millis())
            };
            (void)m_bus.publish(evt);
        }
    }

    void PowerService::configureWakeSources() {
        if (!m_cfg) return;

        // Timer wake is configured at sleep time
        // GPIO wake requires RTC GPIO setup (done in configurePn532WakeSource)

        LOG_DEBUG(POWER_TAG, "Wake sources: timer=%s, pn532=%s",
                  m_cfg->wakeSourceTimerEnabled ? "yes" : "no",
                  m_cfg->wakeSourcePn532Enabled ? "yes" : "no");
    }

    WakeupReason PowerService::determineWakeupReason() {
        const auto cause = esp_sleep_get_wakeup_cause();

        switch (cause) {
            case ESP_SLEEP_WAKEUP_TIMER:
                return WakeupReason::Timer;
            case ESP_SLEEP_WAKEUP_EXT0:
            case ESP_SLEEP_WAKEUP_EXT1:
                // Check if it's the PN532 pin
                if (m_pn532WakeConfigured) {
                    const auto ext1Mask = esp_sleep_get_ext1_wakeup_status();
                    if (ext1Mask & (1ULL << m_pn532IrqPin)) {
                        return WakeupReason::Pn532Interrupt;
                    }
                }
                return WakeupReason::GpioPin;
            case ESP_SLEEP_WAKEUP_TOUCHPAD:
                return WakeupReason::TouchPad;
            case ESP_SLEEP_WAKEUP_ULP:
                return WakeupReason::UlpCoprocessor;
            case ESP_SLEEP_WAKEUP_UNDEFINED:
            default:
                return WakeupReason::PowerOn;
        }
    }

    void PowerService::setCpuFrequency(std::uint8_t mhz) {
#ifdef HW_TARGET_ESP32
        // Map to valid frequencies
        auto freq = mhz;
        if (freq < 80) freq = 80;
        else if (freq < 160) freq = 160;
        else freq = 240;

        setCpuFrequencyMhz(freq);
        LOG_INFO(POWER_TAG, "CPU frequency set to %u MHz", freq);
#else
        (void)mhz;
        LOG_DEBUG(POWER_TAG, "CPU frequency control not supported on this platform");
#endif
    }

}  // namespace isic

