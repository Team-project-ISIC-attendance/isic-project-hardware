#ifndef HARDWARE_PN532DRIVER_HPP
#define HARDWARE_PN532DRIVER_HPP

/**
 * @file Pn532Driver.hpp
 * @brief Production-grade PN532 NFC driver with health monitoring.
 *
 * Provides NFC card reading with self-recovery, health monitoring,
 * and wake-from-sleep support via IRQ pin.
 */

#include <Arduino.h>
#include <Adafruit_PN532.h>

#include <optional>
#include <cstdint>
#include <atomic>
#include <string>
#include <memory>

#include "AppConfig.hpp"
#include "core/Types.hpp"
#include "core/EventBus.hpp"
#include "core/IHealthCheck.hpp"
#include "core/Result.hpp"

// Forward declarations for PowerService
namespace isic {
    class PowerService;
}

namespace isic {
    /**
     * @brief Error codes specific to PN532 operations.
     */
    enum class Pn532Error : std::uint8_t {
        None = 0,
        InitFailed,
        CommunicationTimeout,
        CommunicationError,
        InvalidResponse,
        CardReadFailed,
        RecoveryFailed,
        HardwareNotFound,
    };

    [[nodiscard]] constexpr const char *toString(const Pn532Error error) noexcept {
        switch (error) {
            case Pn532Error::None:                  return "none";
            case Pn532Error::InitFailed:            return "init_failed";
            case Pn532Error::CommunicationTimeout:  return "comm_timeout";
            case Pn532Error::CommunicationError:    return "comm_error";
            case Pn532Error::InvalidResponse:       return "invalid_response";
            case Pn532Error::CardReadFailed:        return "card_read_failed";
            case Pn532Error::RecoveryFailed:        return "recovery_failed";
            case Pn532Error::HardwareNotFound:      return "hw_not_found";
            default:                                return "unknown";
        }
    }

    /**
     * @brief Detailed state information for PN532 driver.
     */
    struct Pn532State {
        Pn532Status status{Pn532Status::Uninitialized};
        Pn532Error lastError{Pn532Error::None};
        std::string lastErrorMessage{};

        std::uint64_t lastCommunicationMs{0};
        std::uint64_t lastSuccessfulReadMs{0};
        std::uint64_t errorStartMs{0};

        std::uint8_t consecutiveErrorCount{0};
        std::uint8_t recoveryAttempts{0};
        std::uint32_t totalErrorCount{0};
        std::uint32_t totalCardsRead{0};

        bool isCardPresent{false};
        CardId lastCardId{};
    };

    /**
     * @brief Production-grade PN532 NFC driver with health monitoring and wake support.
     *
     * Features:
     * - Rich status model with multiple states
     * - Self-check and automatic recovery
     * - Event-driven architecture via EventBus
     * - Wake-from-sleep support via IRQ pin
     * - Non-blocking card polling
     * - Configurable timeouts and retry logic
     * - Health monitoring integration
     *
     * @note The driver runs its own RTOS task for non-blocking card scanning.
     */
    class Pn532Driver : public IHealthCheck, public IEventListener {
    public:
        explicit Pn532Driver(EventBus &bus);
        Pn532Driver(const Pn532Driver &) = delete;
        Pn532Driver &operator=(const Pn532Driver &) = delete;
        Pn532Driver(Pn532Driver &&) = delete;
        Pn532Driver &operator=(Pn532Driver &&) = delete;
        ~Pn532Driver() override;

        /**
         * @brief Initialize the driver with configuration.
         * @param cfg PN532 configuration
         * @return Status indicating success or failure
         */
        [[nodiscard]] Status begin(const Pn532Config &cfg);

        /**
         * @brief Stop the driver and release resources.
         */
        void stop();

        // ==================== Status & Health ====================

        /**
         * @brief Get the current status of the PN532.
         */
        [[nodiscard]] Pn532Status getStatus() const { return m_state.status; }

        /**
         * @brief Get the complete state information.
         */
        [[nodiscard]] const Pn532State &getState() const { return m_state; }

        /**
         * @brief Check if the PN532 is healthy and ready for card reads.
         */
        [[nodiscard]] bool isHealthy() const;

        /**
         * @brief Check if a card is currently present.
         */
        [[nodiscard]] bool isCardPresent() const { return m_state.isCardPresent; }

        /**
         * @brief Get the last read card ID (if any).
         */
        [[nodiscard]] std::optional<CardId> getLastCardId() const;

        // ==================== Card Operations ====================

        /**
         * @brief Attempt to read a card. Non-blocking if no card present.
         * @return Card ID if a new card was detected, nullopt otherwise
         */
        [[nodiscard]] std::optional<CardId> tryReadCard();

        /**
         * @brief Force a re-scan for cards.
         */
        void triggerScan();

        // ==================== Recovery & Diagnostics ====================

        /**
         * @brief Perform a health check / self-test.
         * @return true if the device is responding correctly
         */
        [[nodiscard]] bool performDiagnostic();

        /**
         * @brief Attempt to recover from error state.
         * @return true if recovery was successful
         */
        [[nodiscard]] bool attemptRecovery();

        /**
         * @brief Reset the PN532 hardware.
         */
        void hardwareReset();

        // ==================== Wake/Sleep Support ====================

        /**
         * @brief Configure the IRQ pin for wake-from-sleep.
         * @param powerService PowerService to configure wake source
         */
        void configureWakeSource(PowerService &powerService);

        /**
         * @brief Put the PN532 into low-power mode.
         */
        void enterLowPowerMode();

        /**
         * @brief Wake the PN532 from low-power mode.
         */
        void wakeFromLowPower();

        // ==================== Configuration ====================

        /**
         * @brief Update configuration at runtime.
         * @param cfg New PN532 configuration
         */
        void updateConfig(const Pn532Config &cfg);

        // ==================== IHealthCheck Interface ====================

        [[nodiscard]] HealthStatus getHealth() const override;
        [[nodiscard]] std::string_view getComponentName() const override { return "PN532"; }
        bool performHealthCheck() override;

        // ==================== IEventListener Interface ====================

        void onEvent(const Event &event) override;

    private:
        // Internal task for card scanning
        static void scanTaskThunk(void *arg);
        void scanTask();

        // State management
        void transitionTo(Pn532Status newStatus);
        void recordError(Pn532Error error, const std::string &message = "");
        void clearError();

        // Hardware operations
        bool initHardware();
        bool pingDevice();
        std::optional<CardId> readCardInternal();

        // Event publishing
        void publishStatusChange(Pn532Status oldStatus, Pn532Status newStatus);
        void publishError(Pn532Error error, const std::string &message);
        void publishCardEvent(const CardId &cardId, bool present);

        // Configuration
        const Pn532Config *m_cfg{nullptr};

        // Hardware interface (SPI-based)
        std::unique_ptr<Adafruit_PN532> m_nfc{nullptr};

        // State
        Pn532State m_state{};
        mutable SemaphoreHandle_t m_stateMutex{nullptr};

        // Task management
        TaskHandle_t m_taskHandle{nullptr};
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_scanTriggered{false};

        // EventBus
        EventBus &m_bus;
        EventBus::ListenerId m_subscriptionId{0};

        // Timing
        std::uint64_t m_lastHealthCheckMs{0};
        std::uint64_t m_lastPollMs{0};

        // IRQ handling
        static Pn532Driver *s_instance;
        static void IRAM_ATTR irqHandler();
        volatile bool m_irqTriggered{false};
    };
}

#endif  // HARDWARE_PN532DRIVER_HPP
