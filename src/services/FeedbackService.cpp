#include "services/FeedbackService.hpp"

#include "common/Logger.hpp"
#include "services/ConfigService.hpp"

#include <Arduino.h>

namespace isic
{
namespace
{
// Card scanned - quick acknowledgment
constexpr FeedbackPattern PATTERN_CARD_SCANNED{
        .ledOnMs = 50,
        .ledOffMs = 50,
        .beepMs = 50,
        .beepFrequencyHz = 2000,
        .repeatCount = 1,
        .useErrorLed = false};

// Success - double blink with beep
constexpr FeedbackPattern PATTERN_SUCCESS{
        .ledOnMs = 100,
        .ledOffMs = 50,
        .beepMs = 100,
        .beepFrequencyHz = 2500,
        .repeatCount = 2,
        .useErrorLed = false};

// Error - longer, triple blink
constexpr FeedbackPattern PATTERN_ERROR{
        .ledOnMs = 200,
        .ledOffMs = 100,
        .beepMs = 200,
        .beepFrequencyHz = 1000,
        .repeatCount = 3,
        .useErrorLed = true};

// Processing - rapid blink, no beep
constexpr FeedbackPattern PATTERN_PROCESSING{
        .ledOnMs = 50,
        .ledOffMs = 50,
        .beepMs = 0,
        .beepFrequencyHz = 0,
        .repeatCount = 5,
        .useErrorLed = false};

// Connected - single long blink with beep
constexpr FeedbackPattern PATTERN_CONNECTED{
        .ledOnMs = 500,
        .ledOffMs = 0,
        .beepMs = 100,
        .beepFrequencyHz = 2500,
        .repeatCount = 1,
        .useErrorLed = false};

// Disconnected - double blink, no beep
constexpr FeedbackPattern PATTERN_DISCONNECTED{
        .ledOnMs = 100,
        .ledOffMs = 100,
        .beepMs = 0,
        .beepFrequencyHz = 0,
        .repeatCount = 2,
        .useErrorLed = false};

// OTA Start - slow continuous blink
constexpr FeedbackPattern PATTERN_OTA_START{
        .ledOnMs = 1000,
        .ledOffMs = 1000,
        .beepMs = 200,
        .beepFrequencyHz = 1500,
        .repeatCount = 0xFF, // Infinite
        .useErrorLed = false};

// OTA Complete - rapid success pattern
constexpr FeedbackPattern PATTERN_OTA_COMPLETE{
        .ledOnMs = 100,
        .ledOffMs = 50,
        .beepMs = 100,
        .beepFrequencyHz = 3000,
        .repeatCount = 5,
        .useErrorLed = false};
} // anonymous namespace

FeedbackService::FeedbackService(EventBus &bus, FeedbackConfig &config)
    : ServiceBase("FeedbackService")
    , m_bus(bus)
    , m_config(config)
{
    m_eventConnections.reserve(1); // Known subscription count

    m_eventConnections.push_back(
            m_bus.subscribeScoped(EventType::AttendanceRecorded, [this](const Event &) {
                signalSuccess();
            }));
}

Status FeedbackService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing...");

    if (!m_config.enabled)
    {
        LOG_INFO(m_name, "Disabled by config");
        m_enabled = false;
        setState(ServiceState::Running);
        return Status::Ok();
    }

    // Configure LED pin
    if (m_config.ledEnabled && m_config.ledPin != 0xFF)
    {
        pinMode(m_config.ledPin, OUTPUT);
        setLed(false); // Ensure off state respects polarity
        LOG_DEBUG(m_name, "LED GPIO%u, activeHigh=%d", m_config.ledPin, m_config.ledActiveHigh);
    }

    // Configure buzzer pin
    if (m_config.buzzerEnabled && m_config.buzzerPin != 0xFF)
    {
        pinMode(m_config.buzzerPin, OUTPUT);
        setBuzzer(false);
        LOG_DEBUG(m_name, "Buzzer GPIO%u, freq=%uHz", m_config.buzzerPin, m_config.beepFrequencyHz);
    }

    setState(ServiceState::Running);
    LOG_INFO(m_name, "Ready");
    return Status::Ok();
}

void FeedbackService::loop()
{
    if (!m_enabled)
    {
        return;
    }

    if (m_inPattern)
    {
        // Calculate elapsed time in current cycle
        const auto now{millis()};
        const auto elapsed{now - m_cycleStartMs};
        const auto cycleTime{static_cast<std::uint32_t>(m_currentPattern.ledOnMs + m_currentPattern.ledOffMs)};

        // Check if cycle completed
        if (cycleTime > 0 && elapsed >= cycleTime)
        {
            m_currentRepeat++;

            // Check if pattern completed (0xFF = infinite)
            if (m_currentPattern.repeatCount != 0xFF && (m_currentRepeat >= m_currentPattern.repeatCount))
            {
                // Pattern complete
                m_inPattern = false;
                setLed(false);
                setBuzzer(false);
                return;
            }

            // Start next cycle
            m_cycleStartMs = now;
        }

        // Recalculate elapsed for current cycle
        const auto cycleElapsed{now - m_cycleStartMs};

        // LED state: ON during ledOnMs, OFF during ledOffMs
        const bool ledOn{cycleElapsed < m_currentPattern.ledOnMs};
        setLed(ledOn);

        // Buzzer state: ON during beepMs at start of cycle
        const bool buzzerOn{(m_currentPattern.beepMs > 0) && (cycleElapsed < m_currentPattern.beepMs)};
        setBuzzer(buzzerOn, m_currentPattern.beepFrequencyHz);
    }
    else
    {
        // Not executing pattern - check queue
        if (m_queueCount > 0)
        {
            executePattern(m_patternQueue[m_queueHead]);
            m_queueHead = static_cast<std::uint8_t>((m_queueHead + 1) % FeedbackConfig::Constants::kPatternQueueSize);
            m_queueCount--;
        }
    }
}

void FeedbackService::end()
{
    setLed(false);
    setBuzzer(false);
    clearQueue();
    m_inPattern = false;
    m_eventConnections.clear();
    setState(ServiceState::Stopped);
}

void FeedbackService::signalSuccess()
{
    queuePattern(PATTERN_SUCCESS);
}

void FeedbackService::signalError()
{
    queuePattern(PATTERN_ERROR);
}

void FeedbackService::signalProcessing()
{
    queuePattern(PATTERN_PROCESSING);
}

void FeedbackService::signalConnected()
{
    queuePattern(PATTERN_CONNECTED);
}

void FeedbackService::signalDisconnected()
{
    queuePattern(PATTERN_DISCONNECTED);
}

void FeedbackService::signalOtaStart()
{
    clearQueue(); // OTA takes priority
    queuePattern(PATTERN_OTA_START);
}

void FeedbackService::signalOtaComplete()
{
    queuePattern(PATTERN_OTA_COMPLETE);
}

void FeedbackService::signalCustom(const FeedbackPattern &pattern)
{
    queuePattern(pattern);
}

void FeedbackService::beepOnce(const std::uint16_t durationMs)
{
    if (!m_enabled)
        return;

    if (m_config.buzzerEnabled && m_config.buzzerPin != 0xFF)
    {
        tone(m_config.buzzerPin, m_config.beepFrequencyHz, durationMs);
    }
}

void FeedbackService::ledOnce(const std::uint16_t durationMs)
{
    if (!m_enabled)
    {
        return;
    }

    // This is blocking - use sparingly
    setLed(true);
    delay(durationMs);
    setLed(false);
}

void FeedbackService::queuePattern(const FeedbackPattern &pattern)
{
    if (!m_enabled)
    {
        return;
    }

    if (m_queueCount >= FeedbackConfig::Constants::kPatternQueueSize)
    {
        LOG_WARN(m_name, "Queue full, dropping pattern");
        return;
    }

    m_patternQueue[m_queueTail] = pattern;
    m_queueTail = static_cast<std::uint8_t>((m_queueTail + 1) % FeedbackConfig::Constants::kPatternQueueSize);
    m_queueCount++;
}

void FeedbackService::executePattern(const FeedbackPattern &pattern)
{
    m_currentPattern = pattern;
    m_currentRepeat = 0;
    m_cycleStartMs = millis();
    m_inPattern = true;

    // Immediately start first cycle
    setLed(pattern.ledOnMs > 0);
    if (pattern.beepMs > 0)
    {
        setBuzzer(true, pattern.beepFrequencyHz);
    }
}

void FeedbackService::clearQueue() noexcept
{
    m_queueHead = 0;
    m_queueTail = 0;
    m_queueCount = 0;
}

void FeedbackService::stopCurrent() noexcept
{
    m_inPattern = false;
    setLed(false);
    setBuzzer(false);
}

void FeedbackService::setLed(const bool on)
{
    if (!m_config.ledEnabled || m_config.ledPin == 0xFF)
    {
        return;
    }

    // Avoid redundant writes
    if (on == m_ledCurrentState)
    {
        return;
    }

    m_ledCurrentState = on;

    // Handle active-low LEDs (common on ESP8266 boards)
    const bool pinState{m_config.ledActiveHigh ? on : !on};
    digitalWrite(m_config.ledPin, pinState ? HIGH : LOW);
}

void FeedbackService::setBuzzer(const bool on, std::uint16_t frequencyHz)
{
    if (!m_config.buzzerEnabled || m_config.buzzerPin == 0xFF)
    {
        return;
    }

    // Avoid redundant writes
    if (on == m_buzzerCurrentState)
    {
        return;
    }

    m_buzzerCurrentState = on;

    if (on)
    {
        // Use config frequency if not specified
        const auto freq{(frequencyHz > 0) ? frequencyHz : m_config.beepFrequencyHz};
        tone(m_config.buzzerPin, freq);
    }
    else
    {
        noTone(m_config.buzzerPin);
    }
}
} // namespace isic
