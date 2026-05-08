#include <Adafruit_PN532.h>
#include "services/Pn532Service.hpp"
#include "common/Logger.hpp"

#include <algorithm>

namespace isic
{
namespace
{
constexpr std::uint8_t kPn532WakeupSourceRfLevel{0x01};
constexpr std::uint8_t kPn532WakeupSourceSpi{0x04};
constexpr std::uint8_t kPn532GenerateIrqOnWake{0x01};

constexpr std::uint32_t kActivePollingFallbackIntervalMs{75};
constexpr std::uint32_t kActivePollingFallbackTimeoutMs{30};
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

    if (!m_pn532->begin())
    {
        LOG_ERROR(m_name, "PN532 begin() failed");
        m_pn532State = Pn532State::Error;
        setState(ServiceState::Error);
        return Status::Error("PN532 begin() failed");
    }

    const auto version{m_pn532->getFirmwareVersion()};
    if (!version)
    {
        LOG_ERROR(m_name, "PN532 not found");
        m_pn532State = Pn532State::Error;
        setState(ServiceState::Error);
        return Status::Error("PN532 not found");
    }

    const auto ic{(version >> 24) & 0xFF};
    const auto ver{(version >> 16) & 0xFF};
    const auto rev{(version >> 8) & 0xFF};
    LOG_INFO(m_name, "PN532 found: IC=0x%02X ver=%d.%d", ic, ver, rev);

    const auto &powerConfig = m_configService.getPowerConfig();
    m_activeIrqConfigured = m_config.useIrq();
    m_activeIrqEnabled = m_activeIrqConfigured;
    m_activePollingFallback = false;
    m_sleepIrqWakeEnabled = powerConfig.enableNfcWakeup &&
                            powerConfig.pn532SleepBetweenScans &&
                            m_config.irqPin != 0xFF;
    if (m_sleepIrqWakeEnabled && powerConfig.nfcWakeupPin != 0xFF && powerConfig.nfcWakeupPin != m_config.irqPin)
    {
        LOG_WARN(m_name, "NFC wakeup pin GPIO%d != PN532 IRQ pin GPIO%d; disabling sleep IRQ wake",
                 powerConfig.nfcWakeupPin,
                 m_config.irqPin);
        m_sleepIrqWakeEnabled = false;
    }

    m_irqWakeupEnabled = false;
    m_detectionStarted = false;
    m_lastDetectionFailureMs = 0;
    m_activeIrqRetryAtMs = 0;
    m_pollIntervalMs = m_config.pollIntervalMs ? m_config.pollIntervalMs : Pn532Config::kDefaultReadTimeoutMs;
    m_powerState = PowerState::Active;
    m_targetPowerMode = Pn532PowerMode::ActiveScan;
    m_powerMode = Pn532PowerMode::ActiveScan;
    m_wakeRetryAtMs = 0;
    m_consecutiveErrors = 0;

    if (m_activeIrqConfigured || m_sleepIrqWakeEnabled)
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
        pinMode(m_config.irqPin, INPUT_PULLUP);
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

    if (m_sleepIrqWakeEnabled)
    {
        const auto sleepWakeConfigured = enableIrqWakeup();
        m_sleepIrqWakeEnabled = sleepWakeConfigured;
        if (sleepWakeConfigured)
        {
            if (!waitForIrqHigh(100))
            {
                LOG_WARN(m_name, "IRQ did not return HIGH after sleep wake configuration");
            }
            m_irqPrev = m_irqCurr = digitalRead(m_config.irqPin);
            LOG_INFO(m_name, "PN532 IRQ sleep-wake enabled on GPIO%d", m_config.irqPin);
        }
        else
        {
            disableIrqWakeup();
            LOG_WARN(m_name, "Failed to enable PN532 IRQ sleep-wake; using awake fallback when idle");
        }
    }
    else
    {
        LOG_INFO(m_name, "PN532 sleep configured without IRQ wake");
    }

    m_bus.publish(EventType::NfcReady);

    if (isUsingActiveIrqPrimary())
    {
        // Arm IRQ detection during boot so the first scan does not wait for the scheduler tick.
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
    const bool lowPowerPollingMode = !m_sleepIrqWakeEnabled &&
                                     m_targetPowerMode == Pn532PowerMode::PowerDown &&
                                     shouldSleepBetweenScans() &&
                                     !isUsingActiveIrqPrimary();

    if (m_powerMode == Pn532PowerMode::Recovering)
    {
        if (now < m_wakeRetryAtMs)
        {
            return;
        }

        LOG_INFO(m_name, "Retrying PN532 after wake failure backoff");
        m_powerMode = m_isAsleep ? Pn532PowerMode::PowerDown : Pn532PowerMode::ActiveScan;
    }

    maybeRecoverActiveIrq(now);

    if (m_sleepIrqWakeEnabled && m_targetPowerMode == Pn532PowerMode::PowerDown && shouldSleepBetweenScans())
    {
        if (!m_isAsleep)
        {
            if (!shouldDelaySleepAfterRead(now))
            {
                if (enterSleep())
                {
                    return;
                }
            }
        }
        else if (m_powerState == PowerState::Active)
        {
            m_irqCurr = digitalRead(m_config.irqPin);
            if (m_irqCurr == LOW && m_irqPrev == HIGH)
            {
                LOG_DEBUG(m_name, "Got NFC IRQ while reader asleep");
                handleWakeRead();
            }
            m_irqPrev = m_irqCurr;
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

    if (lowPowerPollingMode)
    {
        m_powerMode = Pn532PowerMode::PowerDown;
        m_isAsleep = false;
    }

    if (m_isAsleep)
    {
        if (!wakeup())
        {
            enterRecovering(now);
            return;
        }
    }

    if (!lowPowerPollingMode)
    {
        m_powerMode = Pn532PowerMode::ActiveScan;
    }

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

    const auto pollIntervalMs = lowPowerPollingMode ? getSleepPollIntervalMs() : getActivePollingIntervalMs();
    const auto readTimeoutMs = lowPowerPollingMode ? getSleepReadTimeoutMs() : getActivePollingTimeoutMs();
    if (millis() - m_lastPollMs >= pollIntervalMs)
    {
        m_lastPollMs = millis();
        pollForCard(readTimeoutMs);
    }
}

void Pn532Service::end()
{
    m_pn532State = Pn532State::Disabled;
    m_detectionStarted = false;
    m_isAsleep = false;
    m_powerMode = Pn532PowerMode::PowerDown;
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
    return m_activeIrqEnabled && !m_activePollingFallback && !m_isAsleep;
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

void Pn532Service::handleWakeRead()
{
    ++m_metrics.irqWakeups;

    if (!wakeup())
    {
        enterRecovering(millis());
        return;
    }

    std::uint8_t uid[7]{};
    std::uint8_t uidLength{};
    if (m_pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, m_config.readTimeoutMs))
    {
        ++m_metrics.sleepWakeReads;
        publishCardEvent(uid, uidLength);
    }
    else
    {
        ++m_metrics.readErrors;
        ++m_metrics.wakeReadFailures;
        ++m_consecutiveErrors;
    }

    m_detectionStarted = false;
}

std::uint32_t Pn532Service::getSleepPollIntervalMs() const
{
    switch (m_powerState)
    {
        case PowerState::Active:
            return 150;
        case PowerState::LightSleep:
            return 300;
        case PowerState::ModemSleep:
            return 750;
        default:
            return m_pollIntervalMs;
    }
}

std::uint32_t Pn532Service::getSleepReadTimeoutMs() const
{
    switch (m_powerState)
    {
        case PowerState::Active:
            return 90;
        case PowerState::LightSleep:
            return 120;
        case PowerState::ModemSleep:
            return 180;
        default:
            return m_config.readTimeoutMs;
    }
}

void Pn532Service::pollWhileAsleep()
{
    if (millis() - m_lastPollMs < getSleepPollIntervalMs())
    {
        return;
    }

    m_lastPollMs = millis();
    if (!wakeup())
    {
        enterRecovering(millis());
        return;
    }

    static constexpr std::uint32_t kSleepScanWindowMs{800};
    std::uint8_t uid[7]{};
    std::uint8_t uidLength{};

    if (m_pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, kSleepScanWindowMs))
    {
        ++m_metrics.sleepWakeReads;
        publishCardEvent(uid, uidLength);
        return;
    }

    if (shouldSleepBetweenScans())
    {
        waitForIrqHigh(50);
        if (!enterSleep())
        {
            enterRecovering(millis());
        }
    }
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
    m_bus.publish({EventType::CardScanned, CardEvent{.timestampMs = m_lastCardReadMs, .uid = m_lastCardUid}});
}

bool Pn532Service::enterSleep()
{
    if (!m_pn532 || m_pn532State != Pn532State::Ready)
    {
        LOG_WARN(m_name, "Cannot enter sleep: PN532 not ready");
        return false;
    }

    if (m_isAsleep)
    {
        return true;
    }

    LOG_INFO(m_name, "Putting PN532 into sleep mode");

    if (m_detectionStarted && m_activeIrqConfigured && !m_activePollingFallback)
    {
        LOG_DEBUG(m_name, "Resetting active IRQ detection before PowerDown");
        if (!reinitializePn532())
        {
            LOG_ERROR(m_name, "Failed to reset PN532 before sleep");
            return false;
        }
    }

    std::uint8_t wakeupSources{kPn532WakeupSourceSpi};
    std::uint8_t cmd[3]{0x16, wakeupSources, 0x00};
    std::uint8_t cmdLength{2};

    if (m_irqWakeupEnabled)
    {
        wakeupSources |= kPn532WakeupSourceRfLevel;
        cmd[2] = kPn532GenerateIrqOnWake;
        cmdLength = 3;
        LOG_INFO(m_name, "PN532 will generate IRQ on card detection during sleep");
    }
    else
    {
        LOG_INFO(m_name, "PN532 sleep wakeup limited to SPI host activity");
    }

    cmd[1] = wakeupSources;

    if (!m_pn532->sendCommandCheckAck(cmd, cmdLength, 100))
    {
        LOG_ERROR(m_name, "Failed to send PowerDown command - no ACK received");
        return false;
    }

    // The PowerDown response frame is intentionally not read here.
    // The PN532 enters PowerDown after ACK; on wakeup the SPI state machine resets,
    // so the unread response bytes do not corrupt subsequent transactions.
    delay(5);

    m_isAsleep = true;
    m_detectionStarted = false;
    m_lastPollMs = millis();
    m_pn532State = Pn532State::Disabled;
    m_powerMode = Pn532PowerMode::PowerDown;
    ++m_metrics.sleepEntries;
    if (m_powerState == PowerState::Active)
    {
        ++m_metrics.earlySleepEntries;
    }
    if (m_activeIrqConfigured || m_sleepIrqWakeEnabled)
    {
        m_irqPrev = m_irqCurr = digitalRead(m_config.irqPin);
    }

    LOG_INFO(m_name, "PN532 entered PowerDown mode (wakeup: 0x%02X)", wakeupSources);
    return true;
}

bool Pn532Service::wakeup()
{
    if (!m_pn532)
    {
        return false;
    }

    if (!m_isAsleep)
    {
        return true;
    }

    LOG_INFO(m_name, "Waking PN532 from PowerDown mode");
    m_pn532->wakeup();

    if (m_activeIrqConfigured || m_sleepIrqWakeEnabled)
    {
        if (!waitForIrqHigh(100))
        {
            LOG_WARN(m_name, "IRQ did not go HIGH after wakeup — proceeding anyway");
        }
    }
    else
    {
        delay(30);
    }

    m_isAsleep = false;
    m_pn532State = Pn532State::Ready;
    m_detectionStarted = false;
    m_powerMode = Pn532PowerMode::ActiveScan;
    m_wakeRetryAtMs = 0;
    if (m_activeIrqConfigured || m_sleepIrqWakeEnabled)
    {
        pinMode(m_config.irqPin, INPUT_PULLUP);
        m_irqPrev = m_irqCurr = digitalRead(m_config.irqPin);
    }

    LOG_INFO(m_name, "PN532 woke from PowerDown successfully");
    return true;
}

bool Pn532Service::enableIrqWakeup()
{
    if (!m_pn532)
    {
        return false;
    }

    pinMode(m_config.irqPin, INPUT_PULLUP);
    LOG_INFO(m_name, "Enabling PN532 IRQ wakeup on card detection");

    if (!m_pn532->SAMConfig())
    {
        LOG_ERROR(m_name, "Failed to reconfigure SAM for IRQ");
        return false;
    }

    m_irqWakeupEnabled = true;

    LOG_INFO(m_name, "PN532 IRQ wakeup enabled on GPIO%d", m_config.irqPin);
    LOG_INFO(m_name, "IRQ pin will go LOW when card detected during PowerDown");

    return true;
}

void Pn532Service::disableIrqWakeup()
{
    m_irqWakeupEnabled = false;
    LOG_INFO(m_name, "IRQ wakeup disabled");
}

bool Pn532Service::shouldSleepBetweenScans() const
{
    return m_configService.getPowerConfig().pn532SleepBetweenScans;
}

bool Pn532Service::shouldDelaySleepAfterRead(const std::uint32_t nowMs) const
{
    return (nowMs - m_lastCardReadMs) < m_configService.getPowerConfig().readerReadyHoldMs;
}

void Pn532Service::enterRecovering(const std::uint32_t nowMs)
{
    m_powerMode = Pn532PowerMode::Recovering;
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

    if (m_activeIrqConfigured || m_sleepIrqWakeEnabled)
    {
        pinMode(m_config.irqPin, INPUT_PULLUP);
    }

    if (m_activeIrqConfigured && !waitForIrqHigh(100))
    {
        LOG_WARN(m_name, "IRQ did not return HIGH after PN532 reinit");
        return false;
    }

    if (m_sleepIrqWakeEnabled)
    {
        m_irqWakeupEnabled = enableIrqWakeup();
        if (!m_irqWakeupEnabled)
        {
            LOG_WARN(m_name, "PN532 sleep IRQ wake re-enable failed after reinit");
            m_sleepIrqWakeEnabled = false;
        }
        else if (!waitForIrqHigh(100))
        {
            LOG_WARN(m_name, "IRQ did not return HIGH after sleep wake re-enable");
        }
    }

    m_isAsleep = false;
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

void Pn532Service::handlePowerStateChange(const PowerEvent &power)
{
    m_powerState = power.targetState;
    m_targetPowerMode = power.pn532TargetMode;
    LOG_DEBUG(m_name, "PN532 power state change: %s -> %s", toString(power.previousState), toString(power.targetState));

    switch (m_targetPowerMode)
    {
        case Pn532PowerMode::PowerDown:
            if (!m_sleepIrqWakeEnabled)
            {
                m_isAsleep = false;
                if (isUsingActiveIrqPrimary())
                {
                    m_powerMode = Pn532PowerMode::ActiveScan;
                    LOG_INFO(m_name, "Sleep IRQ unavailable; keeping PN532 awake in active IRQ mode");
                }
                else
                {
                    m_powerMode = Pn532PowerMode::PowerDown;
                    LOG_INFO(m_name, "Polling low-power mode active (PN532 stays awake, slower scan cadence)");
                }
                break;
            }

            if (shouldSleepBetweenScans() && m_pn532State == Pn532State::Ready && !shouldDelaySleepAfterRead(millis()))
            {
                if (!enterSleep())
                {
                    enterRecovering(millis());
                }
            }
            break;

        case Pn532PowerMode::ActiveScan:
            if (m_isAsleep)
            {
                if (!wakeup())
                {
                    enterRecovering(millis());
                }
            }
            m_detectionStarted = false;
            break;

        case Pn532PowerMode::Recovering:
            enterRecovering(millis());
            break;

        default:
            break;
    }
}
} // namespace isic
