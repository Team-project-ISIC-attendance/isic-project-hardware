#include "services/UserFeedbackService.hpp"
#include "core/Logger.hpp"

namespace isic {

    namespace {
        constexpr auto* FEEDBACK_TAG = "Feedback";
        constexpr std::size_t MAX_QUEUE_SIZE = 16;
    }

    UserFeedbackService::UserFeedbackService(EventBus& bus) : m_bus(bus) {
        m_subscriptionId = m_bus.subscribe(this,
            EventFilter::only(EventType::ConfigUpdated)
                .include(EventType::FeedbackRequested)
                .include(EventType::CardScanned)
                .include(EventType::CardReadError)
                .include(EventType::MqttConnected)
                .include(EventType::MqttDisconnected)
                .include(EventType::OtaStateChanged));
    }

    UserFeedbackService::~UserFeedbackService() {
        stop();
        m_bus.unsubscribe(m_subscriptionId);
        if (m_feedbackQueue) {
            vQueueDelete(m_feedbackQueue);
            m_feedbackQueue = nullptr;
        }
    }

    Status UserFeedbackService::begin(const AppConfig& cfg) {
        m_cfg = &cfg.feedback;
        m_enabled.store(m_cfg->enabled);

        // Store pin configuration
        m_successLedPin = m_cfg->ledSuccessPin;
        m_errorLedPin = m_cfg->ledErrorPin;
        m_buzzerPin = m_cfg->buzzerPin;
        m_ledActiveHigh = m_cfg->ledActiveHigh;

        // Initialize GPIO pins
        if (m_cfg->ledEnabled) {
            pinMode(m_successLedPin, OUTPUT);
            digitalWrite(m_successLedPin, m_ledActiveHigh ? LOW : HIGH);

            if (m_errorLedPin != m_successLedPin) {
                pinMode(m_errorLedPin, OUTPUT);
                digitalWrite(m_errorLedPin, m_ledActiveHigh ? LOW : HIGH);
            }
        }

        if (m_cfg->buzzerEnabled) {
            // Setup LEDC for buzzer PWM (ESP32 Arduino 3.x API)
            ledcAttach(m_buzzerPin, m_cfg->beepFrequencyHz, BUZZER_LEDC_RESOLUTION);
            ledcWrite(m_buzzerPin, 0);  // Start silent
        }

        // Create feedback queue
        const auto queueSize = m_cfg->queueSize > 0 ? m_cfg->queueSize : MAX_QUEUE_SIZE;
        m_feedbackQueue = xQueueCreate(queueSize, sizeof(FeedbackPattern));
        if (!m_feedbackQueue) {
            LOG_ERROR(FEEDBACK_TAG, "Failed to create feedback queue");
            return Status::Error(ErrorCode::ResourceExhausted, "Queue creation failed");
        }

        // Start feedback task
        m_running.store(true);
        xTaskCreatePinnedToCore(
            &UserFeedbackService::feedbackTaskThunk,
            "feedback",
            m_cfg->taskStackSize,
            this,
            m_cfg->taskPriority,
            &m_taskHandle,
            1  // Core 1
        );

        LOG_INFO(FEEDBACK_TAG, "Feedback service started: LED=%s, Buzzer=%s",
                 m_cfg->ledEnabled ? "on" : "off",
                 m_cfg->buzzerEnabled ? "on" : "off");

        return Status::OK();
    }

    void UserFeedbackService::stop() {
        m_running.store(false);

        // Turn off all outputs
        if (m_cfg && m_cfg->ledEnabled) {
            setSuccessLed(false);
            setErrorLed(false);
        }

        if (m_cfg && m_cfg->buzzerEnabled) {
            ledcWrite(m_buzzerPin, 0);
        }

        if (m_taskHandle) {
            vTaskDelay(pdMS_TO_TICKS(200));
            m_taskHandle = nullptr;
        }
    }

    void UserFeedbackService::signalSuccess() {
        if (!m_enabled.load() || !m_cfg) return;

        FeedbackPattern pattern = PATTERN_SUCCESS;
        pattern.ledOnMs = m_cfg->successBlinkMs;
        pattern.beepMs = m_cfg->successBeepMs;
        pattern.beepFrequencyHz = m_cfg->beepFrequencyHz;

        queuePattern(pattern);
    }

    void UserFeedbackService::signalError() {
        if (!m_enabled.load() || !m_cfg) return;

        FeedbackPattern pattern = PATTERN_ERROR;
        pattern.ledOnMs = m_cfg->errorBlinkMs;
        pattern.repeatCount = m_cfg->errorBeepCount;
        pattern.beepFrequencyHz = m_cfg->beepFrequencyHz;

        queuePattern(pattern);
    }

    void UserFeedbackService::signalProcessing() {
        if (!m_enabled.load()) return;
        queuePattern(PATTERN_PROCESSING);
    }

    void UserFeedbackService::signalConnected() {
        if (!m_enabled.load()) return;
        queuePattern(PATTERN_CONNECTED);
    }

    void UserFeedbackService::signalDisconnected() {
        if (!m_enabled.load()) return;
        queuePattern(PATTERN_DISCONNECTED);
    }

    void UserFeedbackService::signalOtaStarted() {
        if (!m_enabled.load()) return;
        queuePattern(PATTERN_OTA_START);
    }

    void UserFeedbackService::signalOtaComplete() {
        if (!m_enabled.load()) return;
        queuePattern(PATTERN_OTA_COMPLETE);
    }

    void UserFeedbackService::signalCustom(const FeedbackPattern& pattern) {
        if (!m_enabled.load()) return;
        queuePattern(pattern);
    }

    void UserFeedbackService::setSuccessLed(bool on) {
        if (!m_cfg || !m_cfg->ledEnabled) return;

        const auto state = m_ledActiveHigh ? (on ? HIGH : LOW) : (on ? LOW : HIGH);
        digitalWrite(m_successLedPin, state);
    }

    void UserFeedbackService::setErrorLed(bool on) {
        if (!m_cfg || !m_cfg->ledEnabled) return;

        const auto pin = (m_errorLedPin != m_successLedPin) ? m_errorLedPin : m_successLedPin;
        const auto state = m_ledActiveHigh ? (on ? HIGH : LOW) : (on ? LOW : HIGH);
        digitalWrite(pin, state);
    }

    void UserFeedbackService::playTone(std::uint16_t frequencyHz, std::uint16_t durationMs) {
        if (!m_cfg || !m_cfg->buzzerEnabled) return;

        ledcWriteTone(m_buzzerPin, frequencyHz);
        vTaskDelay(pdMS_TO_TICKS(durationMs));
        ledcWrite(m_buzzerPin, 0);
    }

    void UserFeedbackService::updateConfig(const FeedbackConfig& cfg) {
        m_cfg = &cfg;
        m_enabled.store(cfg.enabled);

        // Update pin configuration if changed
        if (cfg.ledSuccessPin != m_successLedPin) {
            m_successLedPin = cfg.ledSuccessPin;
            pinMode(m_successLedPin, OUTPUT);
        }

        if (cfg.ledErrorPin != m_errorLedPin) {
            m_errorLedPin = cfg.ledErrorPin;
            pinMode(m_errorLedPin, OUTPUT);
        }

        if (cfg.buzzerPin != m_buzzerPin) {
            ledcDetach(m_buzzerPin);
            m_buzzerPin = cfg.buzzerPin;
            ledcAttach(m_buzzerPin, m_cfg->beepFrequencyHz, BUZZER_LEDC_RESOLUTION);
        }

        m_ledActiveHigh = cfg.ledActiveHigh;

        LOG_INFO(FEEDBACK_TAG, "Config updated: enabled=%s", cfg.enabled ? "yes" : "no");
    }

    void UserFeedbackService::setEnabled(bool enabled) {
        m_enabled.store(enabled);
    }

    void UserFeedbackService::onEvent(const Event& event) {
        if (!m_enabled.load()) return;

        switch (event.type) {
            case EventType::ConfigUpdated:
                if (const auto* ce = std::get_if<ConfigUpdatedEvent>(&event.payload)) {
                    if (ce->config) {
                        updateConfig(ce->config->feedback);
                    }
                }
                break;

            case EventType::FeedbackRequested:
                if (const auto* fr = std::get_if<FeedbackRequestEvent>(&event.payload)) {
                    switch (fr->signal) {
                        case FeedbackSignal::Success:      signalSuccess(); break;
                        case FeedbackSignal::Error:        signalError(); break;
                        case FeedbackSignal::Processing:   signalProcessing(); break;
                        case FeedbackSignal::Connected:    signalConnected(); break;
                        case FeedbackSignal::Disconnected: signalDisconnected(); break;
                        case FeedbackSignal::OtaStarted:   signalOtaStarted(); break;
                        case FeedbackSignal::OtaComplete:  signalOtaComplete(); break;
                        default: break;
                    }
                }
                break;

            case EventType::CardScanned:
                // Card scanned successfully - feedback handled by AttendanceModule
                break;

            case EventType::CardReadError:
                signalError();
                break;

            case EventType::MqttConnected:
                signalConnected();
                break;

            case EventType::MqttDisconnected:
                signalDisconnected();
                break;

            case EventType::OtaStateChanged:
                if (const auto* ota = std::get_if<OtaStateChangedEvent>(&event.payload)) {
                    const auto newState = static_cast<OtaState>(ota->newState);
                    if (newState == OtaState::Downloading) {
                        signalOtaStarted();
                    } else if (newState == OtaState::Completed) {
                        signalOtaComplete();
                    } else if (newState == OtaState::Failed) {
                        signalError();
                    }
                }
                break;

            default:
                break;
        }
    }

    bool UserFeedbackService::queuePattern(const FeedbackPattern& pattern) {
        if (!m_feedbackQueue) return false;

        // Non-blocking enqueue
        if (xQueueSend(m_feedbackQueue, &pattern, 0) != pdTRUE) {
            LOG_DEBUG(FEEDBACK_TAG, "Feedback queue full, dropping pattern");
            return false;
        }

        return true;
    }

    void UserFeedbackService::feedbackTaskThunk(void* arg) {
        static_cast<UserFeedbackService*>(arg)->feedbackTask();
    }

    void UserFeedbackService::feedbackTask() {
        LOG_DEBUG(FEEDBACK_TAG, "Feedback task started");

        FeedbackPattern pattern{};

        while (m_running.load()) {
            if (xQueueReceive(m_feedbackQueue, &pattern, pdMS_TO_TICKS(100)) == pdTRUE) {
                executePattern(pattern);
            }
        }

        LOG_DEBUG(FEEDBACK_TAG, "Feedback task exiting");
        vTaskDelete(nullptr);
    }

    void UserFeedbackService::executePattern(const FeedbackPattern& pattern) {
        if (!m_cfg) return;

        for (std::uint8_t i = 0; i < pattern.repeatCount; ++i) {
            // LED blink
            if (m_cfg->ledEnabled && pattern.ledOnMs > 0) {
                const auto pin = pattern.useErrorLed ? m_errorLedPin : m_successLedPin;
                blinkLed(pin, m_ledActiveHigh, pattern.ledOnMs, pattern.ledOffMs);
            }

            // Buzzer beep
            if (m_cfg->buzzerEnabled && pattern.beepMs > 0) {
                beep(pattern.beepFrequencyHz, pattern.beepMs);
            }

            // Delay between repeats
            if (i < pattern.repeatCount - 1 && pattern.ledOffMs > 0) {
                vTaskDelay(pdMS_TO_TICKS(pattern.ledOffMs));
            }
        }
    }

    void UserFeedbackService::blinkLed(std::uint8_t pin, bool activeHigh,
                                        std::uint16_t onMs, std::uint16_t offMs) {
        // Turn on
        digitalWrite(pin, activeHigh ? HIGH : LOW);
        vTaskDelay(pdMS_TO_TICKS(onMs));

        // Turn off
        digitalWrite(pin, activeHigh ? LOW : HIGH);
        if (offMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(offMs));
        }
    }

    void UserFeedbackService::beep(std::uint16_t frequencyHz, std::uint16_t durationMs) {
        ledcWriteTone(m_buzzerPin, frequencyHz);
        vTaskDelay(pdMS_TO_TICKS(durationMs));
        ledcWrite(m_buzzerPin, 0);
    }

}  // namespace isic

