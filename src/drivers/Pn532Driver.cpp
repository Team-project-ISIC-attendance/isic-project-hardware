#include "drivers/Pn532Driver.hpp"
#include "services/PowerService.hpp"
#include "core/Logger.hpp"

#include <SPI.h>

namespace isic {

    namespace {
        constexpr auto* PN532_TAG = "PN532";
        constexpr auto* PN532_TASK_NAME = "pn532_scan";
        constexpr std::uint32_t PN532_TASK_STACK_SIZE = 4096;
        constexpr std::uint8_t PN532_TASK_PRIORITY = 3;
        constexpr std::uint8_t PN532_TASK_CORE = 1;

        // SPI pins for ESP32 (using default VSPI)
        constexpr std::uint8_t PN532_SCK = 18;
        constexpr std::uint8_t PN532_MISO = 19;
        constexpr std::uint8_t PN532_MOSI = 23;
        constexpr std::uint8_t PN532_SS = 5;

        // Default timeouts
        constexpr std::uint32_t CARD_READ_TIMEOUT_MS = 500;
        constexpr std::uint32_t PING_TIMEOUT_MS = 1000;
    }

    // Static instance for IRQ handler
    Pn532Driver* Pn532Driver::s_instance = nullptr;

    Pn532Driver::Pn532Driver(EventBus& bus) : m_bus(bus) {
        m_stateMutex = xSemaphoreCreateMutex();
        m_subscriptionId = m_bus.subscribe(this);
        s_instance = this;
    }

    Pn532Driver::~Pn532Driver() {
        stop();
        s_instance = nullptr;
        if (m_stateMutex) {
            vSemaphoreDelete(m_stateMutex);
            m_stateMutex = nullptr;
        }
        m_bus.unsubscribe(m_subscriptionId);
    }

    Status Pn532Driver::begin(const Pn532Config& cfg) {
        m_cfg = &cfg;

        LOG_INFO(PN532_TAG, "Initializing PN532 driver (IRQ=%u, RST=%u)",
                 m_cfg->irqPin, m_cfg->resetPin);

        transitionTo(Pn532Status::Initializing);

        if (!initHardware()) {
            transitionTo(Pn532Status::Error);
            recordError(Pn532Error::InitFailed, "Hardware initialization failed");
            return Status::Error(ErrorCode::NotInitialized, "PN532 init failed");
        }

        // Configure IRQ pin for interrupt
        pinMode(m_cfg->irqPin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(m_cfg->irqPin), &Pn532Driver::irqHandler, FALLING);

        transitionTo(Pn532Status::Ready);

        // Start scanning task
        m_running.store(true);
        xTaskCreatePinnedToCore(
            &Pn532Driver::scanTaskThunk,
            PN532_TASK_NAME,
            PN532_TASK_STACK_SIZE,
            this,
            PN532_TASK_PRIORITY,
            &m_taskHandle,
            PN532_TASK_CORE
        );

        LOG_INFO(PN532_TAG, "PN532 initialized successfully");
        return Status::OK();
    }

    void Pn532Driver::stop() {
        m_running.store(false);

        if (m_cfg) {
            detachInterrupt(digitalPinToInterrupt(m_cfg->irqPin));
        }

        if (m_taskHandle) {
            vTaskDelay(pdMS_TO_TICKS(200));
            m_taskHandle = nullptr;
        }

        m_nfc.reset();
        transitionTo(Pn532Status::Offline);
    }

    bool Pn532Driver::isHealthy() const {
        const auto status = getStatus();
        return status == Pn532Status::Ready;
    }

    std::optional<CardId> Pn532Driver::getLastCardId() const {
        if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return std::nullopt;
        }

        std::optional<CardId> result;
        if (m_state.isCardPresent) {
            result = m_state.lastCardId;
        }

        xSemaphoreGive(m_stateMutex);
        return result;
    }

    std::optional<CardId> Pn532Driver::tryReadCard() {
        return readCardInternal();
    }

    void Pn532Driver::triggerScan() {
        m_scanTriggered.store(true);
    }

    bool Pn532Driver::performDiagnostic() {
        LOG_DEBUG(PN532_TAG, "Performing diagnostic check");

        if (!m_nfc) {
            recordError(Pn532Error::HardwareNotFound, "NFC object not initialized");
            return false;
        }

        // Try to get firmware version as a diagnostic
        if (!pingDevice()) {
            recordError(Pn532Error::CommunicationError, "Ping failed");
            return false;
        }

        clearError();
        return true;
    }

    bool Pn532Driver::attemptRecovery() {
        if (m_cfg && m_state.recoveryAttempts >= m_cfg->maxRecoveryAttempts) {
            LOG_ERROR(PN532_TAG, "Max recovery attempts reached (%u)",
                      m_cfg->maxRecoveryAttempts);
            transitionTo(Pn532Status::Offline);
            recordError(Pn532Error::RecoveryFailed, "Max attempts exceeded");
            return false;
        }

        LOG_INFO(PN532_TAG, "Attempting recovery (attempt %u)",
                 m_state.recoveryAttempts + 1);

        transitionTo(Pn532Status::Recovering);

        if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            m_state.recoveryAttempts++;
            xSemaphoreGive(m_stateMutex);
        }

        // Hardware reset
        hardwareReset();

        // Delay before reinit
        vTaskDelay(pdMS_TO_TICKS(m_cfg ? m_cfg->recoveryDelayMs : 2000));

        // Reinitialize
        if (initHardware()) {
            clearError();
            transitionTo(Pn532Status::Ready);

            // Publish recovery event
            const Event evt{
                .type = EventType::Pn532Recovered,
                .payload = Pn532RecoveredEvent{
                    .recoveryAttempts = m_state.recoveryAttempts,
                    .downtimeMs = static_cast<std::uint32_t>(millis() - m_state.errorStartMs)
                },
                .timestampMs = static_cast<std::uint64_t>(millis())
            };
            (void)m_bus.publish(evt);

            LOG_INFO(PN532_TAG, "Recovery successful");
            return true;
        }

        LOG_WARNING(PN532_TAG, "Recovery attempt failed");
        transitionTo(Pn532Status::Error);
        return false;
    }

    void Pn532Driver::hardwareReset() {
        if (!m_cfg) return;

        LOG_DEBUG(PN532_TAG, "Hardware reset via RST pin %u", m_cfg->resetPin);

        pinMode(m_cfg->resetPin, OUTPUT);
        digitalWrite(m_cfg->resetPin, LOW);
        vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite(m_cfg->resetPin, HIGH);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    void Pn532Driver::configureWakeSource(PowerService& powerService) {
        if (!m_cfg) return;
        powerService.configurePn532WakeSource(m_cfg->irqPin);
        LOG_INFO(PN532_TAG, "Configured IRQ pin %u as wake source", m_cfg->irqPin);
    }

    void Pn532Driver::enterLowPowerMode() {
        if (!m_nfc) return;

        LOG_DEBUG(PN532_TAG, "Entering low power mode");
        // PN532 supports SAM configuration for low power
        // This puts the PN532 into virtual card mode or power down
        m_nfc->SAMConfig();
    }

    void Pn532Driver::wakeFromLowPower() {
        if (!m_nfc) return;

        LOG_DEBUG(PN532_TAG, "Waking from low power mode");
        // Re-initialize SAM for normal operation
        m_nfc->SAMConfig();
    }

    void Pn532Driver::updateConfig(const Pn532Config& cfg) {
        m_cfg = &cfg;
        LOG_INFO(PN532_TAG, "Config updated: poll=%ums, health=%ums",
                 m_cfg->pollIntervalMs, m_cfg->healthCheckIntervalMs);
    }

    HealthStatus Pn532Driver::getHealth() const {
        HealthStatus status{};
        status.componentName = getComponentName();
        status.lastUpdatedMs = millis();

        if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            status.errorCount = m_state.totalErrorCount;
            status.uptimeMs = millis();

            switch (m_state.status) {
                case Pn532Status::Ready:
                    status.state = HealthState::Healthy;
                    status.message = "Ready";
                    break;
                case Pn532Status::Recovering:
                case Pn532Status::Initializing:
                    status.state = HealthState::Degraded;
                    status.message = "Recovering";
                    break;
                case Pn532Status::Error:
                case Pn532Status::Offline:
                    status.state = HealthState::Unhealthy;
                    status.message = m_state.lastErrorMessage;
                    break;
                default:
                    status.state = HealthState::Unknown;
                    status.message = "Unknown state";
                    break;
            }

            xSemaphoreGive(m_stateMutex);
        } else {
            status.state = HealthState::Unknown;
            status.message = "Unable to read state";
        }

        return status;
    }

    bool Pn532Driver::performHealthCheck() {
        if (getStatus() != Pn532Status::Ready) {
            return false;
        }

        return performDiagnostic();
    }

    void Pn532Driver::onEvent(const Event& event) {
        switch (event.type) {
            case EventType::ConfigUpdated: {
                if (const auto* ce = std::get_if<ConfigUpdatedEvent>(&event.payload)) {
                    if (ce->config) {
                        updateConfig(ce->config->pn532);
                    }
                }
                break;
            }
            case EventType::WakeupOccurred: {
                // Wake the PN532 when ESP32 wakes
                if (const auto* we = std::get_if<WakeupEvent>(&event.payload)) {
                    if (we->reason == WakeupReason::Pn532Interrupt) {
                        LOG_INFO(PN532_TAG, "Woke due to PN532 interrupt");
                        triggerScan();
                    }
                }
                wakeFromLowPower();
                break;
            }
            case EventType::SleepEntering: {
                // Put PN532 into low power before ESP32 sleeps
                enterLowPowerMode();
                break;
            }
            default:
                break;
        }
    }

    void Pn532Driver::scanTaskThunk(void* arg) {
        static_cast<Pn532Driver*>(arg)->scanTask();
    }

    void Pn532Driver::scanTask() {
        LOG_DEBUG(PN532_TAG, "Scan task started");

        while (m_running.load()) {
            const auto now = millis();
            const auto pollInterval = m_cfg ? m_cfg->pollIntervalMs : 100;
            const auto healthInterval = m_cfg ? m_cfg->healthCheckIntervalMs : 5000;

            // Check if we should poll for cards
            bool shouldPoll = (now - m_lastPollMs >= pollInterval) ||
                              m_scanTriggered.exchange(false) ||
                              m_irqTriggered;

            if (shouldPoll && getStatus() == Pn532Status::Ready) {
                m_irqTriggered = false;
                m_lastPollMs = now;

                const auto cardId = readCardInternal();
                if (cardId) {
                    // Card detected - publish event
                    const Event evt{
                        .type = EventType::CardScanned,
                        .payload = CardScannedEvent{
                            .cardId = *cardId,
                            .timestampMs = static_cast<std::uint64_t>(now)
                        },
                        .timestampMs = static_cast<std::uint64_t>(now)
                    };
                    (void)m_bus.publish(evt);

                    LOG_DEBUG(PN532_TAG, "Card scanned, ID published");
                }
            }

            // Periodic health check
            if (now - m_lastHealthCheckMs >= healthInterval) {
                m_lastHealthCheckMs = now;

                if (getStatus() == Pn532Status::Ready) {
                    if (!performDiagnostic()) {
                        LOG_WARNING(PN532_TAG, "Health check failed");
                        // Will trigger recovery on next iteration if errors persist
                    }
                } else if (getStatus() == Pn532Status::Error) {
                    // Attempt recovery
                    (void)attemptRecovery();
                }
            }

            // Check for too many consecutive errors
            if (m_cfg && m_state.consecutiveErrorCount >= m_cfg->maxConsecutiveErrors) {
                if (getStatus() != Pn532Status::Recovering &&
                    getStatus() != Pn532Status::Offline) {
                    LOG_WARNING(PN532_TAG, "Too many errors (%u), starting recovery",
                               m_state.consecutiveErrorCount);
                    (void)attemptRecovery();
                }
            }

            vTaskDelay(pdMS_TO_TICKS(10));  // Short sleep to yield CPU
        }

        LOG_DEBUG(PN532_TAG, "Scan task exiting");
        vTaskDelete(nullptr);
    }

    void Pn532Driver::transitionTo(Pn532Status newStatus) {
        Pn532Status oldStatus = Pn532Status::Uninitialized;

        if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            oldStatus = m_state.status;
            m_state.status = newStatus;
            xSemaphoreGive(m_stateMutex);
        }

        if (oldStatus != newStatus) {
            LOG_DEBUG(PN532_TAG, "Status: %s -> %s",
                      toString(oldStatus), toString(newStatus));
            publishStatusChange(oldStatus, newStatus);
        }
    }

    void Pn532Driver::recordError(Pn532Error error, const std::string& message) {
        if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (m_state.lastError == Pn532Error::None) {
                m_state.errorStartMs = millis();
            }

            m_state.lastError = error;
            m_state.lastErrorMessage = message;
            m_state.consecutiveErrorCount++;
            m_state.totalErrorCount++;

            xSemaphoreGive(m_stateMutex);
        }

        publishError(error, message);
    }

    void Pn532Driver::clearError() {
        if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            m_state.lastError = Pn532Error::None;
            m_state.lastErrorMessage.clear();
            m_state.consecutiveErrorCount = 0;
            m_state.recoveryAttempts = 0;
            m_state.lastCommunicationMs = millis();
            xSemaphoreGive(m_stateMutex);
        }
    }

    bool Pn532Driver::initHardware() {
        // Create PN532 instance with SPI
        m_nfc = std::make_unique<Adafruit_PN532>(PN532_SS);

        // Initialize SPI
        SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

        m_nfc->begin();

        // Check if PN532 is responding
        const auto versionData = m_nfc->getFirmwareVersion();
        if (!versionData) {
            LOG_ERROR(PN532_TAG, "PN532 not found - check wiring");
            return false;
        }

        LOG_INFO(PN532_TAG, "PN532 found - FW: %u.%u",
                 (versionData >> 16) & 0xFF, (versionData >> 8) & 0xFF);

        // Configure for reading MIFARE cards
        m_nfc->SAMConfig();

        return true;
    }

    bool Pn532Driver::pingDevice() {
        if (!m_nfc) return false;

        const auto version = m_nfc->getFirmwareVersion();
        if (version) {
            if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                m_state.lastCommunicationMs = millis();
                xSemaphoreGive(m_stateMutex);
            }
            return true;
        }
        return false;
    }

    std::optional<CardId> Pn532Driver::readCardInternal() {
        if (!m_nfc || getStatus() != Pn532Status::Ready) {
            return std::nullopt;
        }

        std::uint8_t uid[10] = {0};
        std::uint8_t uidLength = 0;
        const auto timeout = m_cfg ? m_cfg->cardReadTimeoutMs : CARD_READ_TIMEOUT_MS;

        // Non-blocking read with short timeout
        const auto success = m_nfc->readPassiveTargetID(
            PN532_MIFARE_ISO14443A, uid, &uidLength, timeout
        );

        if (!success) {
            // No card present - update state if card was previously present
            if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (m_state.isCardPresent) {
                    m_state.isCardPresent = false;
                    xSemaphoreGive(m_stateMutex);
                    publishCardEvent(m_state.lastCardId, false);
                } else {
                    xSemaphoreGive(m_stateMutex);
                }
            }
            return std::nullopt;
        }

        // Card detected
        CardId cardId{};

        // Copy UID to CardId (pad or truncate as needed)
        const auto copyLen = std::min(static_cast<std::size_t>(uidLength), static_cast<std::size_t>(CARD_ID_SIZE));
        std::copy(uid, uid + copyLen, cardId.begin());

        // Update state
        if (xSemaphoreTake(m_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            const bool wasPresent = m_state.isCardPresent;
            const bool sameCard = (m_state.lastCardId == cardId);

            m_state.isCardPresent = true;
            m_state.lastCardId = cardId;
            m_state.lastSuccessfulReadMs = millis();
            m_state.lastCommunicationMs = millis();
            m_state.totalCardsRead++;

            xSemaphoreGive(m_stateMutex);

            // Only publish if new card or card was removed and re-presented
            if (!wasPresent || !sameCard) {
                publishCardEvent(cardId, true);
                return cardId;
            }
        }

        return std::nullopt;  // Same card still present
    }

    void Pn532Driver::publishStatusChange(Pn532Status oldStatus, Pn532Status newStatus) {
        const Event evt{
            .type = EventType::Pn532StatusChanged,
            .payload = Pn532StatusChangedEvent{
                .oldStatus = oldStatus,
                .newStatus = newStatus,
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(evt);
    }

    void Pn532Driver::publishError(Pn532Error error, const std::string& message) {
        const Event evt{
            .type = EventType::Pn532Error,
            .payload = Pn532ErrorEvent{
                .errorMessage = message,
                .errorCode = static_cast<std::uint32_t>(error),
                .consecutiveErrors = m_state.consecutiveErrorCount,
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(evt);
    }

    void Pn532Driver::publishCardEvent(const CardId& cardId, bool present) {
        const EventType type = present ? EventType::Pn532CardPresent : EventType::Pn532CardRemoved;

        const Event evt{
            .type = type,
            .payload = Pn532CardEvent{
                .cardId = cardId,
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void)m_bus.publish(evt);
    }

    void IRAM_ATTR Pn532Driver::irqHandler() {
        if (s_instance) {
            s_instance->m_irqTriggered = true;
        }
    }

}  // namespace isic

