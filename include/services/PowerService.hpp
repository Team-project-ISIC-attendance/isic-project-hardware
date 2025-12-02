#ifndef HARDWARE_POWERSERVICE_HPP
#define HARDWARE_POWERSERVICE_HPP

/**
 * @file PowerService.hpp
 * @brief Power management service for ESP32 sleep modes.
 *
 * Manages power states, wake locks, and sleep/wake transitions.
 *
 * @note Thread-safe via FreeRTOS primitives.
 */

#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_pm.h>

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

#include "AppConfig.hpp"
#include "core/EventBus.hpp"
#include "core/Result.hpp"

// Forward declaration for dependencies
namespace isic {
    class IHealthCheck;
}

namespace isic {
    /**
     * @brief Wake lock handle returned when acquiring a wake lock.
     *        Must be released via releaseWakeLock() when no longer needed.
     */
    struct WakeLockHandle {
        std::uint32_t id{0};
        const char* name{""};

        [[nodiscard]] bool isValid() const noexcept {
            return id != 0;
        }

        void invalidate() noexcept {
            id = 0;
        }
    };

    /**
     * @brief PowerService manages power states, sleep modes, and wake locks.
     *
     * Responsibilities:
     * - Control ESP32 light/deep sleep modes
     * - Manage wake locks from various modules/services
     * - Configure and handle wake sources (PN532 interrupt, timer, etc.)
     * - Coordinate power state transitions with other components
     *
     * Sleep only occurs when:
     * 1. No wake locks are held
     * 2. Idle timeout has elapsed
     * 3. Sleep is enabled in configuration
     */
    class PowerService : public IEventListener {
    public:
        explicit PowerService(EventBus& bus);
        PowerService(const PowerService&) = delete;
        PowerService& operator=(const PowerService&) = delete;
        PowerService(PowerService&&) = delete;
        PowerService& operator=(PowerService&&) = delete;
        ~PowerService() override;

        /**
         * @brief Initialize the power service with configuration.
         * @param cfg Application configuration
         * @return Status indicating success or failure
         */
        [[nodiscard]] Status begin(const AppConfig& cfg);

        /**
         * @brief Stop the power service and release all resources.
         */
        void stop();

        // ==================== Wake Lock Management ====================

        /**
         * @brief Request a wake lock to prevent sleep.
         *        The system will not enter sleep while any wake locks are held.
         *
         * @param name Human-readable name for debugging (should be string literal)
         * @return Handle to the wake lock (check isValid())
         */
        [[nodiscard]] WakeLockHandle requestWakeLock(const char* name);

        /**
         * @brief Release a previously acquired wake lock.
         * @param handle The wake lock handle to release
         */
        void releaseWakeLock(WakeLockHandle& handle);

        /**
         * @brief Check if any wake locks are currently held.
         * @return true if at least one wake lock is active
         */
        [[nodiscard]] bool hasActiveWakeLocks() const;

        /**
         * @brief Get the count of currently active wake locks.
         */
        [[nodiscard]] std::uint8_t getActiveWakeLockCount() const;

        // ==================== Sleep Control ====================

        /**
         * @brief Request the system to enter idle/light sleep if possible.
         *        Will be blocked if wake locks are held.
         * @return true if sleep was entered, false if blocked
         */
        bool enterIdleSleep();

        /**
         * @brief Request the system to enter deep sleep.
         *        Note: Deep sleep causes a reset on wake, requiring re-init.
         *        Will be blocked if wake locks are held.
         * @return true if entering deep sleep (won't actually return on success)
         */
        bool enterDeepSleep();

        /**
         * @brief Force the system to stay active for the specified duration.
         *        Useful after wakeup to perform operations before allowing sleep.
         * @param durationMs Duration to stay awake in milliseconds
         */
        void stayAwakeFor(std::uint32_t durationMs);

        /**
         * @brief Reset the idle timer (call on any activity).
         */
        void resetIdleTimer();

        // ==================== State Queries ====================

        /**
         * @brief Get the current power state.
         */
        [[nodiscard]] PowerState getCurrentState() const noexcept { return m_currentState.load(); }

        /**
         * @brief Get the reason for the last wakeup.
         */
        [[nodiscard]] WakeupReason getLastWakeupReason() const noexcept { return m_lastWakeupReason; }

        /**
         * @brief Check if sleep is currently allowed.
         */
        [[nodiscard]] bool isSleepAllowed() const;

        /**
         * @brief Get milliseconds since last activity.
         */
        [[nodiscard]] std::uint32_t getIdleTimeMs() const;

        // ==================== Configuration ====================

        /**
         * @brief Update power configuration at runtime.
         * @param cfg New power configuration
         */
        void updateConfig(const PowerConfig& cfg);

        /**
         * @brief Configure PN532 interrupt pin as wake source.
         * @param pin GPIO pin number for PN532 IRQ
         */
        void configurePn532WakeSource(std::uint8_t pin);

        // ==================== EventBus Interface ====================
        void onEvent(const Event& event) override;

    private:
        // Internal task for managing power state machine
        static void powerTaskThunk(void* arg);
        void powerTask();

        // State machine helpers
        void transitionTo(PowerState newState);
        void configureWakeSources();
        WakeupReason determineWakeupReason();

        // CPU frequency management
        void setCpuFrequency(std::uint8_t mhz);

        // Wake lock tracking
        struct WakeLockEntry {
            std::uint32_t id{0};
            const char* name{""};
            std::uint64_t acquiredAt{0};
            bool active{false};
        };

        EventBus& m_bus;
        const PowerConfig* m_cfg{nullptr};

        // Task management
        TaskHandle_t m_taskHandle{nullptr};
        std::atomic<bool> m_running{false};

        // State
        std::atomic<PowerState> m_currentState{PowerState::Active};
        WakeupReason m_lastWakeupReason{WakeupReason::PowerOn};

        // Timing
        std::atomic<std::uint64_t> m_lastActivityMs{0};
        std::atomic<std::uint64_t> m_forcedAwakeUntilMs{0};
        std::uint64_t m_sleepEnteredAt{0};

        // Wake locks
        mutable SemaphoreHandle_t m_wakeLockMutex{nullptr};
        std::vector<WakeLockEntry> m_wakeLocks{};
        std::uint32_t m_nextWakeLockId{1};

        // PN532 wake configuration
        std::uint8_t m_pn532IrqPin{0};
        bool m_pn532WakeConfigured{false};

        // EventBus subscription
        EventBus::ListenerId m_subscriptionId{0};
    };

    /**
     * @brief RAII wrapper for wake locks. Automatically releases the lock when destroyed.
     *
     * @note Non-copyable, movable. Use with caution to avoid dangling locks.
     */
    class ScopedWakeLock {
    public:
        ScopedWakeLock(PowerService& service, const char* name) : m_service(service), m_handle(service.requestWakeLock(name)) {}
        ScopedWakeLock(const ScopedWakeLock&) = delete;
        ScopedWakeLock& operator=(const ScopedWakeLock&) = delete;
        ScopedWakeLock(ScopedWakeLock&& other) noexcept : m_service(other.m_service), m_handle(other.m_handle) {
            other.m_handle.invalidate();
        }
        ScopedWakeLock& operator=(ScopedWakeLock&&) = delete;
        ~ScopedWakeLock() {
            if (m_handle.isValid()) {
                m_service.releaseWakeLock(m_handle);
            }
        }

        [[nodiscard]] bool isValid() const noexcept { return m_handle.isValid(); }

    private:
        PowerService& m_service;
        WakeLockHandle m_handle;
    };
}

#endif  // HARDWARE_POWERSERVICE_HPP

