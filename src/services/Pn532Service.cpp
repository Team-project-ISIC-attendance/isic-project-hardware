#define private public
#include <Adafruit_PN532.h>
#undef private

#include "services/Pn532Service.hpp"

#include "common/Logger.hpp"

#include <algorithm>

namespace isic
{
namespace
{
constexpr std::uint8_t kPn532PowerDownResponseLength{9};
constexpr std::uint8_t kPn532PowerDownResponseCode{0x17};
constexpr std::uint8_t kPn532PowerDownStatusOk{0x00};

constexpr std::uint8_t kPn532WakeupSourceRfLevel{0x01};
constexpr std::uint8_t kPn532WakeupSourceHsu{0x02};
constexpr std::uint8_t kPn532WakeupSourceSpi{0x04};
constexpr std::uint8_t kPn532GenerateIrqOnWake{0x01};

bool readPowerDownResponse(Adafruit_PN532 &pn532, std::uint8_t &status)
{
    std::uint8_t response[kPn532PowerDownResponseLength]{};
    pn532.readdata(response, sizeof(response));

    if (response[6] != kPn532PowerDownResponseCode)
    {
        return false;
    }

    status = response[7];
    return true;
}
} // namespace

void IRAM_ATTR Pn532Service::isrTrampoline()
{
    if (s_activeInstance)
    {
        s_activeInstance->m_irqTriggered.store(true, std::memory_order_relaxed);
    }
}

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

    m_pn532->begin();

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

    // Active scanning always uses polling for reliability.
    // IRQ is retained only for sleep-wake detection (power management).
    // Set m_useIrqMode only if we'll actually use IRQ for sleep-wake.
    const auto &powerConfig = m_configService.getPowerConfig();
    m_useIrqMode = m_config.irqPin != 0xFF && powerConfig.pn532SleepBetweenScans;
    m_pollIntervalMs = m_config.pollIntervalMs ? m_config.pollIntervalMs : Pn532Config::kDefaultReadTimeoutMs;
    m_powerState = PowerState::Active;
    m_targetPowerMode = Pn532PowerMode::ActiveScan;
    m_powerMode = Pn532PowerMode::ActiveScan;
    m_wakeRetryAtMs = 0;

    // Configure SAM (Secure Access Module)
    if (!m_pn532->SAMConfig())
    {
        LOG_ERROR(m_name, "SAM config failed");
        m_pn532State = Pn532State::Error;
        setState(ServiceState::Error);
        return Status::Error("SAM config failed");
    }

    m_pn532State = Pn532State::Ready;
    setState(ServiceState::Running);

    // Configure IRQ pin for sleep-wake detection only (power management).
    // For active scanning, polling is always used instead.
    if (m_useIrqMode)
    {
        pinMode(m_config.irqPin, INPUT_PULLUP);
        if (enableIrqWakeup())
        {
            LOG_INFO(m_name, "PN532 IRQ sleep-wake enabled on GPIO%d", m_config.irqPin);
            m_irqPrev = m_irqCurr = digitalRead(m_config.irqPin);
        }
        else
        {
            LOG_WARN(m_name, "Failed to enable PN532 IRQ sleep-wake; falling back to polling only");
            disableIrqWakeup();
            m_useIrqMode = false;
        }
    }
    else
    {
        LOG_INFO(m_name, "PN532 sleep configured for polling mode (no IRQ sleep-wake)");
    }

    LOG_INFO(m_name, "Using polling mode for active scans (interval: %lums)", m_pollIntervalMs);
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
    const bool lowPowerPollingMode = !m_useIrqMode &&
                                     m_targetPowerMode == Pn532PowerMode::PowerDown &&
                                     shouldSleepBetweenScans();

    if (m_powerMode == Pn532PowerMode::Recovering)
    {
        if (now < m_wakeRetryAtMs)
        {
            return;
        }

        LOG_INFO(m_name, "Retrying PN532 after wake failure backoff");
        m_powerMode = m_isAsleep ? Pn532PowerMode::PowerDown : Pn532PowerMode::ActiveScan;
    }

    if (m_useIrqMode && m_targetPowerMode == Pn532PowerMode::PowerDown && shouldSleepBetweenScans())
    {
        if (!m_isAsleep)
        {
            if (!shouldDelaySleepAfterRead(now))
            {
                if (enterSleep())
                {
                    return;
                }
                // enterSleep() failed (PN532 busy or SPI error) — fall through to
                // active scanning so cards are never missed due to a sleep failure.
            }
        }
        else if (m_useIrqMode && m_powerState == PowerState::Active)
        {
            // IRQ-while-asleep only works reliably in Active state where WiFi is up.
            // In LightSleep/ModemSleep fall through to pollWhileAsleep() as a fallback.
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

    // Polling scan: reliable on all hardware configurations.
    // IRQ pin is retained for sleep-wake signalling only (pollWhileAsleep / handleWakeRead).
    {
        const auto pollIntervalMs = lowPowerPollingMode ? getSleepPollIntervalMs() : m_pollIntervalMs;
        const auto readTimeoutMs = lowPowerPollingMode ? getSleepReadTimeoutMs() : m_config.readTimeoutMs;
        if (millis() - m_lastPollMs >= pollIntervalMs)
        {
            m_lastPollMs = millis();
            pollForCard(readTimeoutMs);
        }
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
    // Only used in IRQ mode - starts async detection, PN532 signals via IRQ when card found
    const auto now = millis();
    if (m_lastDetectionFailureMs != 0 && (now - m_lastDetectionFailureMs) < m_config.recoveryDelayMs)
    {
        return;
    }

    // Reset IRQ state tracking for edge detection
    m_irqPrev = m_irqCurr = HIGH;
    m_irqTriggered.store(false, std::memory_order_relaxed);

    // Use the library's startPassiveTargetIDDetection() which is designed for IRQ mode
    // This sends InListPassiveTarget command and waits for ACK only (not the response)
    // The PN532 will pull IRQ LOW when a card is detected
    //
    // IMPORTANT: This function returns true if a card is ALREADY present (IRQ already LOW),
    // in which case we should read it immediately without waiting for IRQ interrupt
    const bool cardAlreadyPresent = m_pn532->startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A);

    if (cardAlreadyPresent)
    {
        // Card was already in the field - read it immediately
        LOG_DEBUG(m_name, "Card already present during detection start");
        m_detectionStarted = true;
        handleCardDetected();
        return;
    }

    // Check if detection command was sent successfully by verifying IRQ is HIGH
    // (PN532 pulls IRQ LOW when it has response data ready, HIGH means waiting for card)
    m_irqCurr = digitalRead(m_config.irqPin);
    if (m_irqCurr == HIGH)
    {
        // Command sent successfully, now waiting for card
        m_detectionStarted = true;
        m_lastDetectionFailureMs = 0;
        m_consecutiveErrors = 0;
        return;
    }

    // IRQ is LOW but startPassiveTargetIDDetection returned false - something is wrong
    ++m_metrics.readErrors;
    ++m_consecutiveErrors;
    m_lastDetectionFailureMs = now;
    m_detectionStarted = false;

    LOG_WARN(m_name, "Failed to start card detection (retry in %lums, errors=%u)",
             m_config.recoveryDelayMs,
             m_consecutiveErrors);

    if (m_consecutiveErrors >= m_config.maxConsecutiveErrors)
    {
        ++m_metrics.recoveryAttempts;
        m_consecutiveErrors = 0;
        if (m_useIrqMode && recoverIrqMode())
        {
            LOG_WARN(m_name, "PN532 recovered - retrying IRQ detection");
            return;
        }
        if (m_useIrqMode)
        {
            m_useIrqMode = false;
            m_detectionStarted = false;
            LOG_WARN(m_name, "IRQ detection failing - falling back to polling (%lums)", m_pollIntervalMs);
        }
    }
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
        ++m_metrics.readErrors;
        ++m_consecutiveErrors;
    }
    m_detectionStarted = false;  // Restart detection for next card
}

void Pn532Service::handleWakeRead()
{
    ++m_metrics.irqWakeups;

    if (!wakeup())
    {
        enterRecovering(millis());
        return;
    }

    // After PowerDown RF wakeup, PN532 is freshly re-initialized (SAMConfig called in wakeup()).
    // There is no pending InListPassiveTarget response, so readDetectedPassiveTargetID() would fail.
    // Do a fresh poll instead — the card is still in field since it triggered the wakeup.
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
            return 150; // Idle but awake: keep taps feeling immediate.
        case PowerState::LightSleep:
            return 300; // WiFi power-save: slower polling, still sub-second tap response.
        case PowerState::ModemSleep:
            return 750; // WiFi off: save battery, keep first tap comfortably under 1s.
        default:
            return m_pollIntervalMs; // Active state fallback (polling mode only)
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
    bool cardFound = false;
    std::uint8_t uid[7]{};
    std::uint8_t uidLength{};

    // Use blocking readPassiveTargetID for both modes. startPassiveTargetIDDetection() leaves
    // an InListPassiveTarget command pending if no card is found within the window, which causes
    // the subsequent enterSleep() PowerDown command to fail (no ACK from busy PN532).
    cardFound = m_pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, kSleepScanWindowMs);

    if (cardFound)
    {
        ++m_metrics.sleepWakeReads;
        publishCardEvent(uid, uidLength);
        return;
    }

    if (shouldSleepBetweenScans())
    {
        waitForIrqHigh(50);
        enterSleep();
    }
}

void Pn532Service::publishCardEvent(const std::uint8_t *uid, const std::uint8_t uidLength)
{
    const auto len = std::min<std::size_t>(uidLength, 7);
    std::copy_n(uid, len, m_lastCardUid.begin());

    ++m_metrics.cardsRead;
    ++m_metrics.successfulReads;
    m_consecutiveErrors = 0;
    m_lastCardUidLength = uidLength;
    m_lastCardReadMs = millis();

    LOG_DEBUG(m_name, "Card: %s", cardUidToString(m_lastCardUid, uidLength).c_str());
    m_targetPowerMode = Pn532PowerMode::ActiveScan;
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
        return true;  // Already asleep
    }

    LOG_INFO(m_name, "Putting PN532 into sleep mode");

    // TODO: Ensure any ongoing operations are complete before sleep (callers must ensure this), also check all this impl is correct
    // PN532 PowerDown command (0x16)
    // Reference: https://forums.adafruit.com/viewtopic.php?t=70344
    //
    // PowerDown uses a WakeUpEnable bitmask plus an optional GenerateIRQ parameter.
    // For SPI boards the SPI wake source is bit 2 (0x04), not bit 5.
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

    // Send PowerDown command and check for ACK
    if (!m_pn532->sendCommandCheckAck(cmd, cmdLength, 100))
    {
        LOG_ERROR(m_name, "Failed to send PowerDown command - no ACK received");
        return false;
    }

    // PowerDown still emits a normal response frame after the ACK. If we leave that
    // unread, IRQ can stay asserted and the next wake/detect cycle becomes unreliable.
    std::uint8_t powerDownStatus{kPn532PowerDownStatusOk};
    if (!readPowerDownResponse(*m_pn532, powerDownStatus))
    {
        LOG_ERROR(m_name, "Invalid PowerDown response frame");
        return false;
    }
    if (powerDownStatus != kPn532PowerDownStatusOk)
    {
        LOG_ERROR(m_name, "PN532 rejected PowerDown command (status=0x%02X)", powerDownStatus);
        return false;
    }

    // The PN532 needs about 1ms after the response before it is reliably in PowerDown.
    delay(1);

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
    if (m_useIrqMode)
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
        return true; // Already awake
    }

    LOG_INFO(m_name, "Waking PN532 from PowerDown mode");

    // Wakeup sequence for SPI mode (per Adafruit library and PN532 datasheet):
    // Reference: https://forums.adafruit.com/viewtopic.php?t=70344
    // Reference: https://github.com/adafruit/Adafruit-PN532/blob/master/Adafruit_PN532.cpp#L197
    //
    // The PN532 wakes from PowerDown when NSS/CS is held LOW
    // We must use the library's wakeup() method which:
    // 1. Holds CS LOW for 2ms (triggers wakeup)
    // 2. Calls SAMConfig() to restore normal mode
    m_pn532->wakeup();

    // SAMConfig (called inside wakeup) pulls IRQ LOW during its response, then releases.
    // Wait for IRQ HIGH before proceeding — otherwise the SPI bus is dirty and the first
    // readPassiveTargetID command gets corrupted.
    if (m_useIrqMode)
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
    if (m_useIrqMode)
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

    // TODO: need check this impl is correct
    // Reference: https://community.home-assistant.io/t/wake-esp8266-from-deep-sleep-on-tag-read-by-pn532/187760
    //
    // Key points for IRQ-based wakeup:
    // 1. SAMConfig must enable IRQ (3rd parameter = 0x01) - already done in begin()
    // 2. PowerDown command needs bits 0 and 3 set for RF detection IRQ
    // 3. IRQ pin must be configured before deep sleep
    // 4. ESP32: Use esp_sleep_enable_ext0_wakeup() to wake on IRQ LOW
    //    ESP8266: Connect IRQ to RST pin - IRQ LOW triggers hardware reset

    // Configure ESP32/ESP8266 side IRQ pin
    // IMPORTANT: Must be configured as INPUT with pull-up
    // The PN532 will pull this LOW when a card is detected during PowerDown
    pinMode(m_config.irqPin, INPUT_PULLUP);

    LOG_DEBUG(m_name, "ESP32 GPIO%d configured for IRQ (INPUT_PULLUP)", m_config.irqPin);

    // The PN532 IRQ functionality is configured via SAMConfig
    // which is already called in begin() with IRQ enabled (param 3 = 0x01)
    // We just need to ensure SAM is properly configured
    if (!m_pn532->SAMConfig())
    {
        LOG_ERROR(m_name, "Failed to reconfigure SAM for IRQ");
        return false;
    }

    LOG_DEBUG(m_name, "SAM reconfigured with IRQ support");

    // NOTE: We intentionally do NOT call setPassiveActivationRetries() here.
    // The Adafruit library has a bug where it doesn't read the response frame,
    // leaving the IRQ pin stuck LOW. For IRQ wakeup from deep sleep, the default
    // retry settings work fine since the PN532 will continuously scan for cards
    // in PowerDown mode with RF wakeup enabled.

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

    // NOTE: We intentionally skip setPassiveActivationRetries() here.
    // The Adafruit library doesn't read the response frame, leaving IRQ stuck LOW.
    // Default retry settings work fine for both polling and IRQ modes.
    
    if (m_useIrqMode)
    {
        m_irqWakeupEnabled = enableIrqWakeup();
    }

    m_isAsleep = false;
    m_pn532State = Pn532State::Ready;
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
    // Reset IRQ state tracking
    m_irqPrev = m_irqCurr = digitalRead(m_config.irqPin);
    m_lastDetectionFailureMs = 0;
    m_detectionStarted = false;
    return true;
}

bool Pn532Service::waitForIrqHigh(const std::uint32_t timeoutMs)
{
    // Wait for IRQ pin to go HIGH (idle state)
    // The PN532 pulls IRQ LOW when it has data ready or during certain operations
    // We must wait for it to release before starting new operations
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

bool Pn532Service::attachIrqInterrupt()
{
    pinMode(m_config.irqPin, INPUT_PULLUP);
    // Use waitForIrqHigh instead of single check to handle transient LOW states
    if (!waitForIrqHigh(50))
    {
        LOG_WARN(m_name, "IRQ pin GPIO%d is stuck LOW at attach; check wiring or pull-up", m_config.irqPin);
        s_activeInstance = nullptr;
        m_irqTriggered.store(false, std::memory_order_relaxed);
        return false;
    }

    s_activeInstance = this;
    attachInterrupt(digitalPinToInterrupt(m_config.irqPin), isrTrampoline, FALLING);
    m_irqTriggered.store(false, std::memory_order_relaxed);
    LOG_INFO(m_name, "IRQ interrupt attached on GPIO%d", m_config.irqPin);
    return true;
}

void Pn532Service::detachIrqInterrupt()
{
    detachInterrupt(digitalPinToInterrupt(m_config.irqPin));
    s_activeInstance = nullptr;
    m_irqTriggered.store(false, std::memory_order_relaxed);
    LOG_INFO(m_name, "IRQ interrupt detached");
}

void Pn532Service::handlePowerStateChange(const PowerEvent &power)
{
    m_powerState = power.targetState;
    m_targetPowerMode = power.pn532TargetMode;
    LOG_DEBUG(m_name, "PN532 power state change: %s -> %s", toString(power.previousState), toString(power.targetState));

    switch (m_targetPowerMode)
    {
        case Pn532PowerMode::PowerDown:
            if (!m_useIrqMode)
            {
                m_isAsleep = false;
                m_powerMode = Pn532PowerMode::PowerDown;
                LOG_INFO(m_name, "Polling low-power mode active (PN532 stays awake, slower scan cadence)");
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
