#include "services/Pn532Service.hpp"

#include "common/Logger.hpp"

namespace isic
{
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

    m_eventConnections.push_back(m_bus.subscribeScopedAny(EventType::PowerStateChange, [this](const Event &e) {
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

    // Decide between IRQ mode (zero overhead) or polling mode (fallback)
    m_useIrqMode = m_config.useIrq();
    m_pollIntervalMs = m_config.pollIntervalMs ? m_config.pollIntervalMs : Pn532Config::kDefaultReadTimeoutMs;

    // IMPORTANT: For IRQ mode, configure the IRQ pin BEFORE SAMConfig
    // SAMConfig generates an initial IRQ pulse that we need to ignore
    if (m_useIrqMode)
    {
        pinMode(m_config.irqPin, INPUT_PULLUP);
        LOG_DEBUG(m_name, "IRQ pin GPIO%d configured before SAMConfig", m_config.irqPin);
    }

    // Configure SAM (Secure Access Module)
    // Note: SAMConfig temporarily pulls IRQ LOW, then releases it
    if (!m_pn532->SAMConfig())
    {
        LOG_ERROR(m_name, "SAM config failed");
        m_pn532State = Pn532State::Error;
        setState(ServiceState::Error);
        return Status::Error("SAM config failed");
    }

    // Wait for IRQ to stabilize after SAMConfig (it pulses LOW during config)
    if (m_useIrqMode)
    {
        delay(10); // Allow IRQ to return HIGH after SAMConfig
    }

    m_pn532State = Pn532State::Ready;
    setState(ServiceState::Running);

    if (m_useIrqMode)
    {
        // Configure IRQ pin for reading
        pinMode(m_config.irqPin, INPUT_PULLUP);

        // Enable IRQ-based wakeup if configured for power management
        if (const auto &powerConfig = m_configService.getPowerConfig();
            powerConfig.enableNfcWakeup && powerConfig.nfcWakeupPin != 0xFF)
        {
            if (powerConfig.nfcWakeupPin != m_config.irqPin)
            {
                LOG_WARN(m_name, "NFC wakeup pin GPIO%d != PN532 IRQ pin GPIO%d",
                         powerConfig.nfcWakeupPin,
                         m_config.irqPin);
            }
            if (enableIrqWakeup())
            {
                LOG_INFO(m_name, "PN532 IRQ wakeup enabled on GPIO%d", powerConfig.nfcWakeupPin);
            }
            else
            {
                LOG_WARN(m_name, "Failed to enable PN532 IRQ wakeup");
            }
        }

        // Initialize IRQ state tracking
        m_irqPrev = m_irqCurr = digitalRead(m_config.irqPin);
        LOG_INFO(m_name, "Using IRQ polling mode on GPIO%d (initial state: %s)",
                 m_config.irqPin, m_irqCurr == HIGH ? "HIGH" : "LOW");
    }

    if (!m_useIrqMode)
    {
        LOG_INFO(m_name, "Using polling mode (interval: %lums)", m_pollIntervalMs);
    }

    LOG_INFO(m_name, "Pn532Service ready");
    return Status::Ok();
}

void Pn532Service::loop()
{
    if (m_pn532State != Pn532State::Ready || getState() != ServiceState::Running)
    {
        return;
    }

    if (m_useIrqMode)
    {
        // IRQ mode: start detection once, then wait for IRQ to go LOW
        if (!m_detectionStarted)
        {
            startDetection();
            return;
        }

        // Poll the IRQ pin state and detect falling edge (HIGH -> LOW)
        // This is more reliable than hardware interrupts on ESP32 with SPI
        m_irqCurr = digitalRead(m_config.irqPin);

        // When the IRQ is pulled LOW - the reader has got something for us
        if (m_irqCurr == LOW && m_irqPrev == HIGH)
        {
            LOG_DEBUG(m_name, "Got NFC IRQ (pin went LOW)");
            handleCardDetected();
        }

        m_irqPrev = m_irqCurr;
    }
    else
    {
        // Polling mode: directly poll at configured interval
        if (millis() - m_lastPollMs >= m_pollIntervalMs)
        {
            m_lastPollMs = millis();
            pollForCard();
        }
    }
}

void Pn532Service::end()
{
    m_pn532State = Pn532State::Disabled;
    m_detectionStarted = false;
    m_irqPrev = m_irqCurr = HIGH;
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

void Pn532Service::pollForCard()
{
    std::uint8_t uid[7]{};
    std::uint8_t uidLength{};
    if (m_pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, m_config.readTimeoutMs))
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
    m_detectionStarted = false; // Restart detection for next card
}

void Pn532Service::publishCardEvent(const std::uint8_t* uid, std::uint8_t uidLength)
{
    const auto len = std::min<std::size_t>(uidLength, 7);
    std::copy_n(uid, len, m_lastCardUid.begin());

    ++m_metrics.successfulReads;
    m_lastCardUidLength = uidLength;
    m_lastCardReadMs = millis();

    LOG_DEBUG(m_name, "Card: %s", cardUidToString(m_lastCardUid, uidLength).c_str());

    m_bus.publish({EventType::CardScanned, CardEvent{.timestampMs = m_lastCardReadMs, .uid = m_lastCardUid}});
}

bool Pn532Service::enterSleep()
{
    if (!m_pn532 || m_pn532State == Pn532State::Error)
    {
        LOG_WARN(m_name, "Cannot enter sleep: PN532 not ready");
        return false;
    }

    if (m_isAsleep)
    {
        return true; // Already asleep
    }

    LOG_INFO(m_name, "Putting PN532 into sleep mode");

    // TODO: Ensure any ongoing operations are complete before sleep (callers must ensure this), also check all this impl is correct
    // PN532 PowerDown command (0x16)
    // Reference: https://forums.adafruit.com/viewtopic.php?t=70344
    //
    // WakeUpEnable byte (parameter) controls wakeup sources:
    // - Bit 7: Generate IRQ if wakeup source is I2C, SPI, HSU, or GPIO
    // - Bit 6: Enable wakeup on I2C address match
    // - Bit 5: Enable wakeup on SPI chip select (critical for SPI mode!)
    // - Bit 4: Enable wakeup on HSU
    // - Bit 3: Enable wakeup on RF level detector (card detection)
    // - Bit 2-1: Reserved
    // - Bit 0: Generate IRQ if wakeup source is RF level detector
    //
    // For SPI mode with card detection wakeup:
    // - Bit 5 (0x20): SPI wakeup - REQUIRED for SPI mode
    // - Bit 3 (0x08): RF level detector wakeup - enables card detection during sleep
    // - Bit 0 (0x01): Generate IRQ on RF wakeup - pulls IRQ pin low when card detected

    std::uint8_t wakeupSources{0x20}; // Bit 5: Enable SPI wakeup (required for SPI mode)

    // Enable RF field detection wakeup and IRQ generation
    if (m_irqWakeupEnabled)
    {
        wakeupSources |= 0x08; // Bit 3: Enable RF level detector wakeup
        wakeupSources |= 0x01; // Bit 0: Generate IRQ when card detected
        LOG_INFO(m_name, "PN532 will generate IRQ on card detection during sleep");
    }

    // Prepare PowerDown command
    uint8_t cmd[2]{0x16, wakeupSources}; // Command byte + WakeUpEnable byte

    // Send PowerDown command and check for ACK
    // Important: After ACK, the PN532 sends a response frame, then enters sleep
    // The library's sendCommandCheckAck will handle the ACK verification
    if (!m_pn532->sendCommandCheckAck(cmd, 2, 100))
    {
        LOG_ERROR(m_name, "Failed to send PowerDown command - no ACK received");
        return false;
    }

    // TODO: Wakeup behavior:
    // - If bit 0 is set: IRQ pin goes LOW when card detected
    // - ESP32 can use this to wake from deep sleep via ext0/ext1 wakeup
    // - Call wakeup() after ESP32 wakes to restore PN532 to normal operation

    m_isAsleep = true;
    m_pn532State = Pn532State::Disabled;

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

    // Small delay to allow PN532 to fully wake and configure
    delay(10);

    // Verify the PN532 is responding correctly
    const auto version = m_pn532->getFirmwareVersion();
    if (!version)
    {
        LOG_ERROR(m_name, "PN532 wakeup failed - no firmware version response");
        // Don't update state - still considered asleep
        return false;
    }

    // PN532 successfully woke up and is responding
    m_isAsleep = false;
    m_pn532State = Pn532State::Ready;

    LOG_INFO(m_name, "PN532 woke from PowerDown successfully (FW: 0x%08X)", version);
    return true;
}

bool Pn532Service::enableIrqWakeup()
{
    if (!m_pn532)
    {
        return false;
    }

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

    m_pn532State = Pn532State::Ready;
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

bool Pn532Service::waitForIrqHigh(std::uint32_t timeoutMs)
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
    LOG_DEBUG(m_name, "PN532 power state change: %s -> %s", toString(power.previousState), toString(power.targetState));

    switch (power.targetState)
    {
        case PowerState::LightSleep:
        case PowerState::ModemSleep:
        case PowerState::DeepSleep:
        case PowerState::Hibernating:
            // Enter PN532 low-power sleep mode
            if (m_pn532State == Pn532State::Ready)
            {
                enterSleep();
            }
            break;

        case PowerState::Active:
            // Wake PN532 if it was sleeping
            if (m_isAsleep)
            {
                wakeup();
            }
            break;

        default:
            break;
    }
}
} // namespace isic
