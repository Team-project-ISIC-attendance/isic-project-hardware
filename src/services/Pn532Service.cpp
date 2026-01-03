#include "services/Pn532Service.hpp"

#include "common/Logger.hpp"

namespace isic
{
Pn532Service::Pn532Service(EventBus &bus, const Config &config)
    : ServiceBase("Pn532Service")
    , m_bus(bus)
    , m_config(config.pn532)
    , m_powerConfig(config.power)
{
    m_eventConnections.reserve(1);

    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::PowerStateChange, [this](const Event &e) {
        if (const auto *power = e.get<PowerEvent>())
        {
            handlePowerStateChange(*power);
        }
    }));
}

Pn532Service::~Pn532Service()
{
    if (m_pn532)
    {
        delete m_pn532;
        m_pn532 = nullptr;
    }
}

Status Pn532Service::begin()
{
    LOG_INFO(m_name, "Initializing Pn532Service...");
    setState(ServiceState::Initializing);

    if (!m_pn532)
    {
        m_pn532 = new Adafruit_PN532(m_config.spiSckPin, m_config.spiMisoPin, m_config.spiMosiPin, m_config.spiCsPin);
    }

    // Configure reset pin if specified
    if (m_config.resetPin != 0xFF)
    {
        pinMode(m_config.resetPin, OUTPUT);
        digitalWrite(m_config.resetPin, HIGH);
        delay(10);
        digitalWrite(m_config.resetPin, LOW);
        delay(10);
        digitalWrite(m_config.resetPin, HIGH);
        delay(100);
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

    // Enable IRQ-based wakeup if configured for power management
    if (m_powerConfig.enableNfcWakeup)
    {
        if (enableIrqWakeup())
        {
            LOG_INFO(m_name, "PN532 IRQ wakeup enabled on GPIO%d", m_powerConfig.nfcWakeupPin);
        }
        else
        {
            LOG_WARN(m_name, "Failed to enable PN532 IRQ wakeup");
        }
    }

    LOG_INFO(m_name, "Pn532Service ready");
    return Status::Ok();
}

void Pn532Service::loop()
{
    if ((m_pn532State != Pn532State::Ready) && (getState() != ServiceState::Running))
    {
        return;
    }

    if (millis() - m_lastPollMs < m_config.pollIntervalMs)
    {
        return;
    }

    m_lastPollMs = millis();
    scanForCard();
}

void Pn532Service::end()
{
    m_pn532State = Pn532State::Disabled;
    setState(ServiceState::Stopped);
}

void Pn532Service::scanForCard()
{
    std::uint8_t uid[7]{0};
    std::uint8_t uidLength{0};

    if (!m_pn532->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50))
    {
        return;
    }

    CardUid cardUid;

    const auto copyLen{static_cast<std::size_t>(uidLength < 7 ? uidLength : 7)};
    for (std::size_t i = 0; i < copyLen; i++)
    {
        cardUid[i] = uid[i];
    }

    ++m_metrics.successfulReads;
    m_metrics.lastReadMs = millis();
    m_lastCardUid = cardUid;
    m_lastCardUidLength = uidLength;
    m_lastCardReadMs = millis();

    LOG_DEBUG(m_name, "Card read: %s", cardUidToString(cardUid, uidLength).c_str());

    m_bus.publish({EventType::CardScanned,
                   CardEvent{
                           .uid = cardUid,
                           .uidLength = uidLength,
                           .timestampMs = static_cast<std::uint32_t>(millis()),
                   }});
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
    uint8_t cmd[2] {0x16, wakeupSources}; // Command byte + WakeUpEnable byte

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

    // Set passive activation retries to infinite (0xFF)
    // This allows the PN532 to continuously monitor for cards
    // Critical for wake-on-card functionality
    if (!m_pn532->setPassiveActivationRetries(0xFF))
    {
        LOG_WARN(m_name, "Failed to set passive activation retries (non-critical)");
    }
    else
    {
        LOG_DEBUG(m_name, "Passive activation retries set to infinite");
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
