#include "services/FeedbackService.hpp"

#include "common/Logger.hpp"
#include "services/ConfigService.hpp"

#include <Arduino.h>

namespace isic
{
/*
 * Feedback patterns — LED + buzzer reference
 *
 * Event              Color    Beep   Meaning
 * ─────────────────────────────────────────────
 * Attendance OK      —        1×     Scan recorded
 * Error              Red      3×     Read/record failed
 * Processing         Blue     —      Waiting on network
 * Connected          Green    —      WiFi + MQTT up
 * Disconnected       Magenta  —      Connection lost
 * OTA updating       Cyan∞    1×     Do not power off
 * OTA done           Green    5×     Firmware updated
 */
namespace
{
constexpr std::uint16_t kAttendanceSuccessBeepMs{80};
constexpr std::uint16_t kAttendanceSuccessBeepFrequencyHz{2800};

// Read/record error — red triple blink + 3 low beeps
constexpr FeedbackPattern PATTERN_ERROR{
        .ledOnMs = 150, .ledOffMs = 100, .beepMs = 120, .beepFrequencyHz = 700,
        .repeatCount = 3, .color = LedColor::Red};

// Processing/waiting — blue pulse, silent
constexpr FeedbackPattern PATTERN_PROCESSING{
        .ledOnMs = 80, .ledOffMs = 80, .beepMs = 0, .beepFrequencyHz = 0,
        .repeatCount = 4, .color = LedColor::Blue};

// WiFi + MQTT connected — green steady glow, silent
constexpr FeedbackPattern PATTERN_CONNECTED{
        .ledOnMs = 400, .ledOffMs = 0, .beepMs = 0, .beepFrequencyHz = 0,
        .repeatCount = 1, .color = LedColor::Green};

// Connection lost — magenta double blink, silent
constexpr FeedbackPattern PATTERN_DISCONNECTED{
        .ledOnMs = 200, .ledOffMs = 200, .beepMs = 0, .beepFrequencyHz = 0,
        .repeatCount = 2, .color = LedColor::Magenta};

// OTA in progress — cyan slow infinite blink + single beep on first cycle
constexpr FeedbackPattern PATTERN_OTA_START{
        .ledOnMs = 800, .ledOffMs = 800, .beepMs = 100, .beepFrequencyHz = 1200,
        .repeatCount = 0xFF, .color = LedColor::Cyan};

// OTA complete — green rapid blink + 5 beeps
constexpr FeedbackPattern PATTERN_OTA_COMPLETE{
        .ledOnMs = 100, .ledOffMs = 50, .beepMs = 60, .beepFrequencyHz = 3200,
        .repeatCount = 5, .color = LedColor::Green};
} // anonymous namespace

FeedbackService::FeedbackService(EventBus &bus, FeedbackConfig &config)
    : ServiceBase("FeedbackService")
    , m_bus(bus)
    , m_config(config)
{
    // Silence buzzer immediately — pin would float until begin() otherwise
    if (m_config.buzzerEnabled && m_config.buzzerPin != 0xFF)
    {
        pinMode(m_config.buzzerPin, OUTPUT);
        digitalWrite(m_config.buzzerPin, LOW);
    }

    m_eventConnections.reserve(1);

    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::AttendanceRecorded, [this](const Event &) {
        signalSuccess();
    }));
}

Status FeedbackService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing...");

    if (!m_config.enabled)
    {
        LOG_WARN(m_name, "Disabled by config — no LED/buzzer output");
        m_enabled = false;
        setState(ServiceState::Running);
        return Status::Ok();
    }

    // Configure LED pin(s)
    if (hasLedConfigured())
    {
        if (isRgb())
        {
            pinMode(m_config.ledRedPin,   OUTPUT);
            pinMode(m_config.ledGreenPin, OUTPUT);
            pinMode(m_config.ledBluePin,  OUTPUT);
            setLed(LedColor::Off);
            LOG_INFO(m_name, "RGB LED ready R=GPIO%u G=GPIO%u B=GPIO%u activeHigh=%d",
                     m_config.ledRedPin, m_config.ledGreenPin, m_config.ledBluePin, m_config.ledActiveHigh);
        }
        else if (m_config.ledPin != 0xFF)
        {
            pinMode(m_config.ledPin, OUTPUT);
            setLed(LedColor::Off);
            LOG_INFO(m_name, "Single LED ready GPIO%u activeHigh=%d", m_config.ledPin, m_config.ledActiveHigh);
        }
    }

    // Configure buzzer pin
    if (m_config.buzzerEnabled && m_config.buzzerPin != 0xFF)
    {
        pinMode(m_config.buzzerPin, OUTPUT);
        digitalWrite(m_config.buzzerPin, LOW);
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

            if (m_currentPattern.repeatCount != 0xFF && (m_currentRepeat >= m_currentPattern.repeatCount))
            {
                m_inPattern = false;
                setLed(LedColor::Off);
                setBuzzer(false);
                return;
            }

            // Start next cycle
            m_cycleStartMs = now;
        }

        // Recalculate elapsed for current cycle
        const auto cycleElapsed{now - m_cycleStartMs};

        const bool ledOn{cycleElapsed < m_currentPattern.ledOnMs};
        setLed(ledOn ? m_currentPattern.color : LedColor::Off);

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
    setLed(LedColor::Off);
    setBuzzer(false);
    clearQueue();
    m_inPattern = false;
    m_eventConnections.clear();
    setState(ServiceState::Stopped);
}

void FeedbackService::signalSuccess()
{
    LOG_INFO(m_name, "Signal: attendance recorded (single beep)");
    stopCurrent();
    clearQueue();

    if (m_enabled && m_config.buzzerEnabled && m_config.buzzerPin != 0xFF)
    {
        tone(m_config.buzzerPin, kAttendanceSuccessBeepFrequencyHz, kAttendanceSuccessBeepMs);
    }
}

void FeedbackService::signalError()
{
    LOG_INFO(m_name, "Signal: error (red x3)");
    queuePattern(PATTERN_ERROR);
}

void FeedbackService::signalProcessing()
{
    LOG_INFO(m_name, "Signal: processing (blue x5)");
    queuePattern(PATTERN_PROCESSING);
}

void FeedbackService::signalConnected()
{
    LOG_INFO(m_name, "Signal: connected (green long)");
    queuePattern(PATTERN_CONNECTED);
}

void FeedbackService::signalDisconnected()
{
    LOG_INFO(m_name, "Signal: disconnected (yellow x2)");
    queuePattern(PATTERN_DISCONNECTED);
}

void FeedbackService::signalOtaStart()
{
    LOG_INFO(m_name, "Signal: OTA start (cyan blink)");
    clearQueue();
    queuePattern(PATTERN_OTA_START);
}

void FeedbackService::signalOtaComplete()
{
    LOG_INFO(m_name, "Signal: OTA complete (green x5)");
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
    if (!m_enabled || !hasLedConfigured())
    {
        return;
    }

    setLed(LedColor::White);
    delay(durationMs);
    setLed(LedColor::Off);
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

    LOG_DEBUG(m_name, "Pattern start: color=%u repeat=%u on=%ums off=%ums",
              static_cast<uint8_t>(pattern.color), pattern.repeatCount,
              pattern.ledOnMs, pattern.ledOffMs);

    setLed(pattern.ledOnMs > 0 ? pattern.color : LedColor::Off);
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
    setLed(LedColor::Off);
    setBuzzer(false);
}

bool FeedbackService::hasLedConfigured() const noexcept
{
    return m_config.ledEnabled && (isRgb() || m_config.ledPin != 0xFF);
}

bool FeedbackService::isRgb() const noexcept
{
    return m_config.ledRedPin != 0xFF && m_config.ledGreenPin != 0xFF && m_config.ledBluePin != 0xFF;
}

void FeedbackService::setLed(const LedColor color)
{
    if (!hasLedConfigured() || color == m_ledCurrentColor)
    {
        return;
    }

    m_ledCurrentColor = color;

    if (isRgb())
    {
        const auto bits = static_cast<std::uint8_t>(color);
        const bool ah = m_config.ledActiveHigh;
        LOG_DEBUG(m_name, "RGB color=%u (R=%d G=%d B=%d)",
                  bits, !!(bits & 0b001), !!(bits & 0b010), !!(bits & 0b100));
        digitalWrite(m_config.ledRedPin,   (bits & 0b001) ? (ah ? HIGH : LOW) : (ah ? LOW : HIGH));
        digitalWrite(m_config.ledGreenPin, (bits & 0b010) ? (ah ? HIGH : LOW) : (ah ? LOW : HIGH));
        digitalWrite(m_config.ledBluePin,  (bits & 0b100) ? (ah ? HIGH : LOW) : (ah ? LOW : HIGH));
    }
    else if (m_config.ledPin != 0xFF)
    {
        const bool on = (color != LedColor::Off);
        LOG_DEBUG(m_name, "LED %s", on ? "ON" : "OFF");
        const bool pinState = m_config.ledActiveHigh ? on : !on;
        digitalWrite(m_config.ledPin, pinState ? HIGH : LOW);
    }
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
        const auto freq{(frequencyHz > 0) ? frequencyHz : m_config.beepFrequencyHz};
        LOG_DEBUG(m_name, "Buzzer ON %uHz GPIO%u", freq, m_config.buzzerPin);
        tone(m_config.buzzerPin, freq);
        m_buzzerChannelInitialized = true;
    }
    else
    {
        LOG_DEBUG(m_name, "Buzzer OFF");
        if (m_buzzerChannelInitialized)
        {
            noTone(m_config.buzzerPin);
        }
        digitalWrite(m_config.buzzerPin, LOW);
    }
}
} // namespace isic
