#include <Adafruit_PN532.h>
#include "services/Pn532Service.hpp"
#include "common/Logger.hpp"

#include <algorithm>

namespace isic
{
namespace
{
constexpr std::uint32_t kActivePollingFallbackIntervalMs{75};
constexpr std::uint32_t kActivePollingFallbackTimeoutMs{30};
constexpr std::uint32_t kSleepScanWindowMs{200};
constexpr std::uint32_t kModemSleepScanWindowMs{2000};
constexpr std::uint32_t kSleepDelayAfterReadMs{2000};
} // namespace

Pn532Service::Pn532Service(EventBus &bus, ConfigService &configService)
    : ServiceBase("Pn532Service")
    , m_bus(bus)
    , m_configService(configService)
    , m_config(m_configService.getPn532Config())
{
    m_eventConnections.reserve(1);
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::PowerStateChange, [this](const Event &e) {
        if (const auto *power = e.get<PowerEvent>())
        {
            handlePowerStateChange(*power);
        }
    }));
}

Status Pn532Service::begin()
{
    LOG_INFO(m_name, "Initializing Pn532Service...");
    setState(ServiceState::Initializing);

    if (!m_pn532)
    {
        m_pn532 = std::make_unique<Adafruit_PN532>(m_config.spiSckPin, m_config.spiMisoPin, m_config.spiMosiPin, m_config.spiCsPin);
    }

    constexpr int kInitMaxAttempts{3};
    std::uint32_t version{0};
    for (int attempt = 1; attempt <= kInitMaxAttempts; ++attempt)
    {
        if (!m_pn532->begin())
        {
            LOG_WARN(m_name, "PN532 begin() failed (attempt %d/%d)", attempt, kInitMaxAttempts);
        }
        else
        {
            version = m_pn532->getFirmwareVersion();
        }

        if (version)
        {
            break;
        }

        LOG_WARN(m_name, "PN532 not found (attempt %d/%d), retrying in 150ms...", attempt, kInitMaxAttempts);
        delay(150);
    }

    if (!version)
    {
        LOG_ERROR(m_name, "PN532 not found after %d attempts", kInitMaxAttempts);
        m_pn532State = Pn532State::Error;
        setState(ServiceState::Error);
        return Status::Error("PN532 not found");
    }

    const auto ic{(version >> 24) & 0xFF};
    const auto ver{(version >> 16) & 0xFF};
    const auto rev{(version >> 8) & 0xFF};
    LOG_INFO(m_name, "PN532 found: IC=0x%02X ver=%d.%d", ic, ver, rev);

    m_activeIrqConfigured = m_config.useIrq();
    m_activeIrqEnabled = m_activeIrqConfigured;
    m_activePollingFallback = false;
    m_detectionStarted = false;
    m_lastDetectionFailureMs = 0;
    m_activeIrqRetryAtMs = 0;
    m_pollIntervalMs = m_config.pollIntervalMs ? m_config.pollIntervalMs : Pn532Config::kDefaultReadTimeoutMs;
    m_powerState = PowerState::Active;
    m_targetPowerMode = Pn532PowerMode::ActiveScan;
    m_powerMode = Pn532PowerMode::ActiveScan;
    m_wakeRetryAtMs = 0;
    m_consecutiveErrors = 0;
    m_isAsleep = false;
    m_lastCardReadMs = 0;
    m_lastPollMs = 0;

    if (m_activeIrqConfigured)
    {
        pinMode(m_config.irqPin, INPUT_PULLUP);
    }

    if (!m_pn532->SAMConfig())
    {
        LOG_ERROR(m_name, "SAM config failed");
        m_pn532State = Pn532State::Error;
        setState(ServiceState::Error);
        return Status::Error("SAM config failed");
    }

    m_pn532State = Pn532State::Ready;
    setState(ServiceState::Running);

    if (m_activeIrqConfigured)
    {
        if (!waitForIrqHigh(100))
        {
            activateActivePollingFallback(millis(), "IRQ did not stabilize after SAMConfig");
        }
        else
        {
            m_irqPrev = m_irqCurr = digitalRead(m_config.irqPin);
            LOG_INFO(m_name, "Active scan mode -> irq-primary on GPIO%d", m_config.irqPin);
        }
    }
    else
    {
        LOG_INFO(m_name, "Active scan mode -> polling-only (IRQ unavailable)");
    }

    m_bus.publish(EventType::NfcReady);

    if (isUsingActiveIrqPrimary())
    {
        startDetection();
    }

    LOG_INFO(m_name, "Pn532Service ready");
    return Status::Ok();
}

void Pn532Service::loop()
{
    if (getState() != ServiceState::Running || !m_pn532 || m_pn532State == Pn532State::Error)
    {
        return;
    }

    const auto now{millis()};

    if (m_powerMode == Pn532PowerMode::Recovering)
    {
        if (now < m_wakeRetryAtMs)
        {
            return;
        }
        LOG_INFO(m_name, "Retrying PN532 after recovery backoff");
        m_powerMode = Pn532PowerMode::ActiveScan;
    }

    if (m_targetPowerMode == Pn532PowerMode::PowerDown && shouldSleepBetweenScans())
    {
        if (!m_isAsleep)
        {
            if (!shouldDelaySleepAfterRead(now))
            {
                if (!enterSleep())
                {
                    enterRecovering(now);
                }
            }
        }
        else
        {
            pollWhileAsleep();
        }

        if (m_isAsleep)
        {
            return;
        }
    }

    maybeRecoverActiveIrq(now);

    if (m_pn532State != Pn532State::Ready)
    {
        return;
    }

    if (isUsingActiveIrqPrimary())
    {
        if (!m_detectionStarted)
        {
            startDetection();
            return;
        }

        m_irqCurr = digitalRead(m_config.irqPin);
        if (m_irqCurr == LOW && m_irqPrev == HIGH)
        {
            LOG_DEBUG(m_name, "Got NFC IRQ (pin went LOW)");
            handleCardDetected();
        }
        m_irqPrev = m_irqCurr;
        return;
    }

    if (millis() - m_lastPollMs >= getActivePollingIntervalMs())
    {
        m_lastPollMs = millis();
        pollForCard(getActivePollingTimeoutMs());
    }
}

void Pn532Service::end()
{
    m_pn532State = Pn532State::Disabled;
    m_detectionStarted = false;
    m_powerMode = Pn532PowerMode::PowerDown;
    m_isAsleep = false;
    m_irqPrev = m_irqCurr = HIGH;
    m_eventConnections.clear();
    setState(ServiceState::Stopped);
}

void Pn532Service::startDetection()
{
    const auto now = millis();
    if (m_lastDetectionFailureMs != 0 && (now - m_lastDetectionFailureMs) < m_config.recoveryDelayMs)
    {
        return;
    }

    m_irqPrev = m_irqCurr = HIGH;

    const bool cardAlreadyPresent = m_pn532->startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A);
    if (cardAlreadyPresent)
    {
        LOG_DEBUG(m_name, "Card already present during detection start");
        m_detectionStarted = true;
        handleCardDetected();
        return;
    }

    m_irqCurr = digitalRead(m_config.irqPin);
    if (m_irqCurr == HIGH)
    {
        m_detectionStarted = true;
        m_lastDetectionFailureMs = 0;
        m_consecutiveErrors = 0;
        return;
    }

    m_detectionStarted = false;
    noteActiveIrqFailure(now, "IRQ stayed LOW after startPassiveTargetIDDetection");
}

void Pn532Service::pollForCard(const std::uint32_t timeoutMs)
{
    std::uint8_t uid[7]{};
    std::uint8_t uidLength{};
    if (m_pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, timeoutMs))
    {
        publishCardEvent(uid, uidLength);
    }
}

void Pn532Service::handleCardDetected()
{
    std::uint8_t uid[7]{};
    std::uint8_t uidLength{};
    if (m_pn532->readDetectedPassiveTargetID(uid, &uidLength))
    {
        publishCardEvent(uid, uidLength);
    }
    else
    {
        noteActiveIrqFailure(millis(), "readDetectedPassiveTargetID failed");
    }
    m_detectionStarted = false;
}

void Pn532Service::pollWhileAsleep()
{
    const auto now = millis();
    if (now - m_lastPollMs < getPollIntervalWhileAsleep())
    {
        return;
    }
    m_lastPollMs = now;

    ++m_metrics.sleepWakeReads;

    if (!wakeup())
    {
        enterRecovering(millis());
        return;
    }

    std::uint8_t uid[7]{};
    std::uint8_t uidLength{};
    const auto scanWindowMs = (m_powerState == PowerState::ModemSleep) ? kModemSleepScanWindowMs : kSleepScanWindowMs;
    if (m_pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, scanWindowMs))
    {
        publishCardEvent(uid, uidLength);
        return;
    }

    if (shouldSleepBetweenScans())
    {
        if (!enterSleep())
        {
            enterRecovering(millis());
        }
    }
}

bool Pn532Service::enterSleep()
{
    if (m_isAsleep)
    {
        return true;
    }

    // WakeUpEnable = 0x20: SPI is the only wakeup source
    std::uint8_t cmd[2] = {PN532_COMMAND_POWERDOWN, 0x20};
    if (!m_pn532->sendCommandCheckAck(cmd, 2, 200))
    {
        LOG_WARN(m_name, "PN532 PowerDown command not acknowledged");
        return false;
    }

    m_isAsleep = true;
    m_detectionStarted = false;
    m_powerMode = Pn532PowerMode::PowerDown;
    ++m_metrics.sleepEntries;
    LOG_DEBUG(m_name, "PN532 entered hardware power-down");
    return true;
}

bool Pn532Service::wakeup()
{
    if (!m_isAsleep)
    {
        return true;
    }

    // CS LOW for 2ms — SPI wakeup sequence per PN532 datasheet
    digitalWrite(m_config.spiCsPin, LOW);
    delay(2);
    digitalWrite(m_config.spiCsPin, HIGH);

    m_isAsleep = false;
    m_detectionStarted = false;
    m_wakeRetryAtMs = 0;

    if (!m_pn532->SAMConfig())
    {
        LOG_WARN(m_name, "SAMConfig failed after wakeup, attempting full reinit");
        if (!reinitializePn532())
        {
            LOG_ERROR(m_name, "PN532 wakeup reinit failed");
            m_isAsleep = false;
            return false;
        }
        LOG_INFO(m_name, "PN532 recovered via reinit after wakeup");
        return true;
    }

    m_powerMode = Pn532PowerMode::ActiveScan;
    LOG_DEBUG(m_name, "PN532 woke from hardware power-down");
    return true;
}

void Pn532Service::noteActiveIrqFailure(const std::uint32_t nowMs, const char *reason)
{
    ++m_metrics.readErrors;
    ++m_metrics.activeIrqFailures;
    ++m_consecutiveErrors;
    m_lastDetectionFailureMs = nowMs;

    LOG_WARN(m_name, "Active IRQ detection failure: %s (errors=%u)", reason, m_consecutiveErrors);

    if (m_consecutiveErrors < m_config.maxConsecutiveErrors)
    {
        return;
    }

    m_consecutiveErrors = 0;
    activateActivePollingFallback(nowMs, reason);
}

void Pn532Service::activateActivePollingFallback(const std::uint32_t nowMs, const char *reason)
{
    m_activeIrqEnabled = false;
    m_detectionStarted = false;
    m_lastDetectionFailureMs = nowMs;
    m_activeIrqRetryAtMs = nowMs + m_config.recoveryDelayMs;

    if (!m_activePollingFallback)
    {
        m_activePollingFallback = true;
        ++m_metrics.activePollFallbackEntries;
        LOG_WARN(m_name, "Active scan mode -> poll-fallback (%s)", reason);
        return;
    }

    LOG_WARN(m_name, "Active poll-fallback remains active (%s)", reason);
}

void Pn532Service::maybeRecoverActiveIrq(const std::uint32_t nowMs)
{
    if (!m_activeIrqConfigured || !m_activePollingFallback || nowMs < m_activeIrqRetryAtMs)
    {
        return;
    }

    if (recoverIrqMode())
    {
        m_activeIrqEnabled = true;
        m_activePollingFallback = false;
        m_activeIrqRetryAtMs = 0;
        LOG_INFO(m_name, "Active scan mode -> recovered-to-irq");
        return;
    }

    m_activeIrqRetryAtMs = nowMs + m_config.recoveryDelayMs;
    LOG_WARN(m_name, "Active IRQ recovery failed; staying in poll-fallback");
}

bool Pn532Service::isUsingActiveIrqPrimary() const
{
    return m_activeIrqEnabled && !m_activePollingFallback;
}

std::uint32_t Pn532Service::getActivePollingIntervalMs() const
{
    if (m_activePollingFallback)
    {
        return kActivePollingFallbackIntervalMs;
    }

    return m_pollIntervalMs;
}

std::uint32_t Pn532Service::getActivePollingTimeoutMs() const
{
    if (m_activePollingFallback)
    {
        return kActivePollingFallbackTimeoutMs;
    }

    return m_config.readTimeoutMs;
}

void Pn532Service::publishCardEvent(const std::uint8_t *uid, const std::uint8_t uidLength)
{
    const auto len = std::min<std::size_t>(uidLength, 7);
    std::copy_n(uid, len, m_lastCardUid.begin());

    ++m_metrics.cardsRead;
    ++m_metrics.successfulReads;
    m_consecutiveErrors = 0;
    m_lastDetectionFailureMs = 0;
    m_lastCardUidLength = uidLength;
    m_lastCardReadMs = millis();

    LOG_DEBUG(m_name, "Card: %s", cardUidToString(m_lastCardUid, uidLength).c_str());
    m_powerMode = Pn532PowerMode::ActiveScan;
    m_wakeRetryAtMs = 0;
    m_bus.publish({EventType::CardScanned, CardEvent{.timestampMs = millis(), .uid = m_lastCardUid}});
}

void Pn532Service::enterRecovering(const std::uint32_t nowMs)
{
    m_powerMode = Pn532PowerMode::Recovering;
    m_isAsleep = false;
    m_wakeRetryAtMs = nowMs + m_config.recoveryDelayMs;
    ++m_metrics.recoveryAttempts;
    LOG_WARN(m_name, "PN532 entering recovery backoff for %lums", m_config.recoveryDelayMs);
}

bool Pn532Service::reinitializePn532()
{
    if (!m_pn532)
    {
        return false;
    }

    if (!m_pn532->begin())
    {
        LOG_ERROR(m_name, "PN532 reinit failed - begin() failed");
        return false;
    }

    const auto version{m_pn532->getFirmwareVersion()};
    if (!version)
    {
        LOG_ERROR(m_name, "PN532 reinit failed - no firmware response");
        return false;
    }

    if (!m_pn532->SAMConfig())
    {
        LOG_ERROR(m_name, "PN532 reinit failed - SAM config failed");
        return false;
    }

    if (m_activeIrqConfigured)
    {
        pinMode(m_config.irqPin, INPUT_PULLUP);
    }

    if (m_activeIrqConfigured && !waitForIrqHigh(100))
    {
        LOG_WARN(m_name, "IRQ did not return HIGH after PN532 reinit");
        return false;
    }

    m_pn532State = Pn532State::Ready;
    m_detectionStarted = false;
    m_powerMode = Pn532PowerMode::ActiveScan;
    m_wakeRetryAtMs = 0;
    return true;
}

bool Pn532Service::recoverIrqMode()
{
    LOG_WARN(m_name, "Attempting PN532 recovery for IRQ detection");
    if (!reinitializePn532())
    {
        LOG_ERROR(m_name, "PN532 recovery failed");
        return false;
    }

    m_activeIrqEnabled = true;
    m_irqPrev = m_irqCurr = digitalRead(m_config.irqPin);
    m_lastDetectionFailureMs = 0;
    m_detectionStarted = false;
    return true;
}

bool Pn532Service::waitForIrqHigh(const std::uint32_t timeoutMs)
{
    const auto start = millis();
    while (digitalRead(m_config.irqPin) == LOW)
    {
        if (millis() - start >= timeoutMs)
        {
            return false;
        }
        delay(1);
    }
    return true;
}

std::uint32_t Pn532Service::getPollIntervalWhileAsleep() const
{
    if (m_powerState == PowerState::ModemSleep)
    {
        return m_config.deepSleepPollIntervalMs;
    }
    return m_config.lightSleepPollIntervalMs;
}

bool Pn532Service::shouldSleepBetweenScans() const
{
    return m_targetPowerMode == Pn532PowerMode::PowerDown && m_powerMode != Pn532PowerMode::Recovering;
}

bool Pn532Service::shouldDelaySleepAfterRead(const std::uint32_t nowMs) const
{
    if (m_lastCardReadMs == 0)
    {
        return false;
    }
    return (nowMs - m_lastCardReadMs) < kSleepDelayAfterReadMs;
}

void Pn532Service::handlePowerStateChange(const PowerEvent &power)
{
    m_powerState = power.targetState;
    m_targetPowerMode = power.pn532TargetMode;

    LOG_DEBUG(m_name, "Power state: %s -> %s, PN532 target=%s",
              toString(power.previousState), toString(power.targetState), toString(m_targetPowerMode));

    if (m_targetPowerMode == Pn532PowerMode::PowerDown)
    {
        if (m_pn532State == Pn532State::Ready && !m_isAsleep && !shouldDelaySleepAfterRead(millis()))
        {
            if (!enterSleep())
            {
                enterRecovering(millis());
            }
        }
    }
    else if (m_targetPowerMode == Pn532PowerMode::ActiveScan)
    {
        if (m_isAsleep)
        {
            if (!wakeup())
            {
                enterRecovering(millis());
            }
        }
        m_detectionStarted = false;
    }
}
} // namespace isic
