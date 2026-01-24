#include "services/PowerService.hpp"
#include "common/Logger.hpp"
#include "platform/PlatformESP.hpp"
#include "platform/PlatformPower.hpp"
#include "services/ConfigService.hpp"

namespace isic
{

PowerService::PowerService(EventBus &bus, const PowerConfig &config)
    : ServiceBase("PowerService")
    , m_bus(bus)
    , m_config(config)
{
    eventConnections_.reserve(7);
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::WifiConnected, [this](const Event &e) {
        handleWifiConnected(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::WifiDisconnected, [this](const Event &e) {
        handleWifiDisconnected(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::MqttConnected, [this](const Event &e) {
        handleMqttConnected(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::MqttDisconnected, [this](const Event &e) {
        handleMqttDisconnected(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::CardScanned, [this](const Event &e) {
        handleCardScanned(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::MqttMessage, [this](const Event &e) {
        handleMqttMessage(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::NfcReady, [this](const Event &e) {
        handleNfcReady(e);
    }));
}

PowerService::~PowerService()
{
    PowerService::end();
}

Status PowerService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing PowerService...");

    const auto detectedReason = detectWakeupReason();
    m_wakeupReason = detectedReason;

    // Load RTC data (deep sleep may present as external reset on some boards)
    const bool rtcLoaded = loadFromRtcMemory();
    const bool deepSleepResume = rtcLoaded && rtcData_.lastRequestedState == PowerState::DeepSleep;

    if (deepSleepResume)
    {
        LOG_INFO(m_name, "Restored RTC data: wakeups=%u, totalSleepMs=%u", rtcData_.wakeupCount, rtcData_.totalSleepMs);

        m_metrics.wakeupCount = rtcData_.wakeupCount;

        const bool nfcPending = rtcData_.pendingNfcWakeup != 0;
        const bool possibleNfcWake =
                nfcPending &&
                (m_wakeupReason == WakeupReason::External ||
                 m_wakeupReason == WakeupReason::PowerOn ||
                 m_wakeupReason == WakeupReason::Unknown);
        if (possibleNfcWake)
        {
            m_pendingNfcWakeup = true;
            LOG_INFO(m_name, ">>> NFC WAKEUP PENDING - waiting for card scan <<<");

            // If wake reason looks like a power-on reset but we expected NFC wake,
            // treat it as external to trigger fast NFC handling.
            if (m_wakeupReason == WakeupReason::PowerOn || m_wakeupReason == WakeupReason::Unknown)
            {
                m_wakeupReason = WakeupReason::External;
            }
        }

        checkChainedSleep();
    }
    else
    {
        // Fresh boot - initialize RTC data
        rtcData_ = RtcData{};
        rtcData_.magic = RtcData::MAGIC;
    }

    if (m_wakeupReason != detectedReason)
    {
        LOG_INFO(m_name, "Wakeup reason adjusted: %s -> %s", toString(detectedReason), toString(m_wakeupReason));
    }
    LOG_INFO(m_name, "Wakeup reason: %s", toString(m_wakeupReason));

    setNfcWakeGate(false);

    rtcData_.wakeupCount++;
    m_metrics.wakeupCount = rtcData_.wakeupCount;

    m_lastActivityMs = millis();

    m_currentState = PowerState::Active;

    setState(ServiceState::Ready);
    publishWakeupOccurred(m_wakeupReason);

    LOG_INFO(m_name, "=== POWER CONFIG ===");
    LOG_INFO(m_name, "  autoSleep=%d, idleTimeout=%ums", m_config.autoSleepEnabled, m_config.idleTimeoutMs);
    LOG_INFO(m_name, "  lightSleep=%ums, modemSleep=%ums, deepSleep=%ums",
             m_config.lightSleepDurationMs, m_config.modemSleepDurationMs, m_config.sleepIntervalMs);
    LOG_INFO(m_name, "  smartSleep=%d (short<%ums, medium<%ums)",
             m_config.smartSleepEnabled, m_config.smartSleepShortThresholdMs, m_config.smartSleepMediumThresholdMs);
    const int irqPin = (m_config.nfcWakeupPin == 0xFF) ? -1 : m_config.nfcWakeupPin;
    const int gatePin = (m_config.nfcWakeGatePin == 0xFF) ? -1 : m_config.nfcWakeGatePin;
    LOG_INFO(m_name, "  nfcWakeup=%d (IRQ GPIO%d, gate GPIO%d)",
             m_config.enableNfcWakeup, irqPin, gatePin);
    LOG_INFO(m_name, "===================");
    LOG_INFO(m_name, "Ready (wakeup #%u, reason=%s)", m_metrics.wakeupCount, toString(m_wakeupReason));
    return Status::Ok();
}

void PowerService::loop()
{
    if (m_state != ServiceState::Ready && m_state != ServiceState::Running)
    {
        return;
    }

    // Periodic status log (every 5 seconds)
    static std::uint32_t lastStatusLog = 0;
    const auto now = millis();
    if (now - lastStatusLog >= 5000)
    {
        lastStatusLog = now;
        LOG_DEBUG(m_name, "[STATUS] state=%s, wifi=%d, mqtt=%d, sleepActive=%d, sleepPending=%d",
                  m_state == ServiceState::Running ? "Running" : "Ready",
                  m_flags.wifiReady, m_flags.mqttReady, m_flags.sleepActive, m_flags.sleepPending);
    }

    if (m_flags.sleepPending)
    {
        const auto elapsed = now - m_sleepRequestedAtMs;
        if (elapsed >= PowerConfig::Constants::kSleepDelayMs)
        {
            LOG_INFO(m_name, "Executing pending sleep...");
            executePendingSleep();
            m_flags.sleepPending = false;
        }
        return;
    }

    // Handle active sleep timer (light or modem - unified)
    if (m_flags.sleepActive)
    {
        const auto elapsed = now - m_sleepStartMs;
        const auto remaining = m_sleepDurationMs > elapsed ? m_sleepDurationMs - elapsed : 0;

        // Log sleep progress every second
        static std::uint32_t lastSleepLog = 0;
        if (now - lastSleepLog >= 1000)
        {
            lastSleepLog = now;
            LOG_DEBUG(m_name, "[SLEEPING] %s: %ums / %ums (wake in %ums)",
                      m_flags.isModemSleep ? "modem" : "light",
                      elapsed, m_sleepDurationMs, remaining);
        }

        if (elapsed >= m_sleepDurationMs)
        {
            LOG_INFO(m_name, "Sleep timer expired, waking up...");
            wakeFromSleep();
        }
        return;
    }

    // State-specific logic
    if (m_state == ServiceState::Ready)
    {
        handleReadyState();
    }
    else if (m_state == ServiceState::Running)
    {
        handleRunningState();
    }
}

void PowerService::end()
{
    setState(ServiceState::Stopping);
    LOG_INFO(m_name, "Shutting down...");

    if (m_flags.sleepPending)
    {
        cancelSleepRequest();
    }

    m_flags.sleepActive = false;
    saveToRtcMemory();
    eventConnections_.clear();

    setState(ServiceState::Stopped);
    LOG_INFO(m_name, "Stopped");
}

void PowerService::handleReadyState()
{
    if (m_config.autoSleepEnabled && m_flags.wifiReady)
    {
        setState(ServiceState::Running);
        LOG_INFO(m_name, "Transitioning to Running - auto-sleep active");
    }
}

void PowerService::handleRunningState()
{
    // Network-aware automatic modem sleep
    if (m_config.modemSleepOnMqttDisconnect && !m_flags.mqttReady && !m_flags.sleepActive && !m_flags.sleepPending)
    {
        LOG_INFO(m_name, "MQTT disconnected, entering modem sleep");
        enterModemSleepAsync(m_config.modemSleepDurationMs);
        m_metrics.networkAwareSleeps++;
        return;
    }

    if (m_config.autoSleepEnabled && !m_flags.sleepPending && !m_flags.sleepActive)
    {
        checkIdleTimeout();
    }
}

void PowerService::handleWifiConnected(const Event & /* event */)
{
    LOG_INFO(m_name, "WiFi connected - activity reset");
    m_flags.wifiReady = true;
    recordActivityInternal(ActivityType::WifiConnected);

    if (m_config.autoSleepEnabled && m_state == ServiceState::Ready)
    {
        setState(ServiceState::Running);
        LOG_INFO(m_name, "Auto-sleep now active");
    }
}

void PowerService::handleWifiDisconnected(const Event & /* event */)
{
    LOG_INFO(m_name, "WiFi disconnected");
    m_flags.wifiReady = false;

    if (m_state == ServiceState::Running)
    {
        setState(ServiceState::Ready);
        LOG_INFO(m_name, "Auto-sleep paused (no WiFi)");
    }
}

void PowerService::handleMqttConnected(const Event & /* event */)
{
    LOG_INFO(m_name, "MQTT connected - activity reset");
    m_flags.mqttReady = true;
    recordActivityInternal(ActivityType::MqttConnected);

    if (m_flags.sleepActive && m_flags.isModemSleep)
    {
        LOG_INFO(m_name, "MQTT reconnected, waking from modem sleep");
        wakeFromSleep();
    }
}

void PowerService::handleMqttDisconnected(const Event & /* event */)
{
    LOG_INFO(m_name, "MQTT disconnected");
    m_flags.mqttReady = false;
}

void PowerService::handleCardScanned(const Event & /* event */)
{
    LOG_INFO(m_name, ">>> CARD SCANNED - IRQ triggered <<<");
    recordActivityInternal(ActivityType::CardScanned);

    if (m_flags.sleepActive)
    {
        LOG_INFO(m_name, "Card scan waking from %s sleep", m_flags.isModemSleep ? "modem" : "light");
        wakeFromSleep();
    }
    else if (m_flags.sleepPending)
    {
        LOG_INFO(m_name, "Card scan cancelled pending sleep");
        cancelSleepRequest();
    }
    else
    {
        LOG_INFO(m_name, "Card scan - idle timer reset");
    }
}

void PowerService::handleMqttMessage(const Event & /* event */)
{
    LOG_DEBUG(m_name, "MQTT message - activity reset");
    recordActivityInternal(ActivityType::MqttMessage);
}

void PowerService::handleNfcReady(const Event & /* event */)
{
    recordActivityInternal(ActivityType::NfcReady);
}

PowerState PowerService::selectSmartSleepDepth()
{
    if (!m_config.smartSleepEnabled)
    {
        return PowerState::LightSleep;
    }

    const auto estimatedIdleMs = estimateIdleDuration();
    m_metrics.smartSleepUsed++;

    // Short idle: light sleep
    if (estimatedIdleMs < m_config.smartSleepShortThresholdMs)
    {
        return PowerState::LightSleep;
    }

    // Medium idle: modem sleep if MQTT down
    if (estimatedIdleMs < m_config.smartSleepMediumThresholdMs)
    {
        return m_flags.mqttReady ? PowerState::LightSleep : PowerState::ModemSleep;
    }

    // Long idle: deep sleep if ready
    // NOTE: On ESP8266, NFC wake requires wiring PN532 IRQ -> RST (hardware reset).
    // We still allow deep sleep here to support RST wake on both platforms.

    return canEnterSleep() ? PowerState::DeepSleep : PowerState::ModemSleep;
}

std::uint32_t PowerService::estimateIdleDuration() const
{
    // TODO: we can have smarter estimation based on activity patterns or history or hardcoded schedules
    // If we're already past idle timeout, estimate medium duration
    if (const auto currentIdleMs = getTimeSinceLastActivityMs(); currentIdleMs >= m_config.idleTimeoutMs)
    {
        return m_config.smartSleepMediumThresholdMs;
    }

    // Otherwise, return the configured idle timeout
    return m_config.idleTimeoutMs;
}

bool PowerService::canEnterSleep() const
{
    // TODO: canEnterSleep
    // Check if system is ready for deep sleep

    // For now, always allow sleep
    // In future, could check:
    // - MQTT publish queue empty
    // - Attendance batch sent
    // - No ongoing OTA update
    // - PN532 ready for sleep

    return true;
}

std::uint32_t PowerService::getDurationForState(PowerState state) const
{
    switch (state)
    {
        case PowerState::LightSleep:
            return m_config.lightSleepDurationMs;
        case PowerState::ModemSleep:
            return m_config.modemSleepDurationMs;
        case PowerState::DeepSleep:
        case PowerState::Hibernating:
            return m_config.sleepIntervalMs;
        default:
            return m_config.lightSleepDurationMs;
    }
}

void PowerService::requestSleep(const PowerState state, std::uint32_t durationMs)
{
    if (state == PowerState::Active)
    {
        LOG_WARN(m_name, "Cannot request sleep to Active state");
        return;
    }

    if (durationMs == 0)
    {
        durationMs = getDurationForState(state);
    }

    LOG_INFO(m_name, "Sleep requested: state=%s, duration=%ums", toString(state), durationMs);
    publishSleepRequested(state, durationMs);

    m_flags.sleepPending = true;
    m_pendingSleepState = state;
    m_pendingSleepDurationMs = durationMs;
    m_sleepRequestedAtMs = millis();
}

void PowerService::cancelSleepRequest()
{
    if (m_flags.sleepPending)
    {
        LOG_INFO(m_name, "Sleep request cancelled");
        m_flags.sleepPending = false;
    }
}

void PowerService::executePendingSleep()
{
    switch (m_pendingSleepState)
    {
        case PowerState::LightSleep: {
            enterLightSleepAsync(m_pendingSleepDurationMs);
            break;
        }
        case PowerState::ModemSleep: {
            enterModemSleepAsync(m_pendingSleepDurationMs);
            break;
        }
        case PowerState::DeepSleep:
        case PowerState::Hibernating: {
            enterDeepSleepAsync(m_pendingSleepDurationMs);
            break;
        }
        default: {
            break;
        }
    }
}

void PowerService::enterLightSleepAsync(const std::uint32_t durationMs)
{
    LOG_INFO(m_name, ">>> ENTERING LIGHT SLEEP for %ums <<<", durationMs);
    LOG_INFO(m_name, "    (CPU active, WiFi connected, IRQ works)");

    const auto oldState = m_currentState;
    m_currentState = PowerState::LightSleep;
    m_metrics.lightSleepCycles++;

    publishStateChange(m_currentState, oldState);

    m_flags.sleepActive = true;
    m_flags.isModemSleep = false;
    m_sleepStartMs = millis();
    m_sleepDurationMs = durationMs;
}

void PowerService::enterModemSleepAsync(const std::uint32_t durationMs)
{
    LOG_INFO(m_name, ">>> ENTERING MODEM SLEEP for %ums <<<", durationMs);
    LOG_INFO(m_name, "    (CPU active, WiFi OFF, IRQ works)");

    const auto oldState = m_currentState;
    m_currentState = PowerState::ModemSleep;
    m_metrics.modemSleepCycles++;

    publishStateChange(m_currentState, oldState);

    m_flags.sleepActive = true;
    m_flags.isModemSleep = true;
    m_sleepStartMs = millis();
    m_sleepDurationMs = durationMs;
}

void PowerService::wakeFromSleep()
{
    if (!m_flags.sleepActive)
    {
        return;
    }

    const auto wasModem = m_flags.isModemSleep;
    const auto sleptMs = millis() - m_sleepStartMs;
    m_flags.sleepActive = false;

    const auto oldState = m_currentState;
    m_currentState = PowerState::Active;
    publishStateChange(m_currentState, oldState);

    recordActivity();

    if (m_flags.wifiReady && m_config.autoSleepEnabled)
    {
        setState(ServiceState::Running);
    }

    LOG_INFO(m_name, ">>> WOKE from %s sleep (slept %ums) <<<", wasModem ? "modem" : "light", sleptMs);
}

void PowerService::enterDeepSleepAsync(const std::uint32_t durationMs)
{
    // Clamp duration to platform limit, chain remaining
    std::uint32_t actualDuration = durationMs;
    std::uint32_t remaining = 0;

    if (durationMs > m_config.maxDeepSleepMs)
    {
        actualDuration = m_config.maxDeepSleepMs;
        remaining = durationMs - actualDuration;
        LOG_INFO(m_name, "Deep sleep chained: %ums now, %ums remaining", actualDuration, remaining);
    }

    LOG_INFO(m_name, ">>> ENTERING DEEP SLEEP for %ums <<<", actualDuration);
    LOG_INFO(m_name, "    (CPU OFF, WiFi OFF, device will RESET on wake)");

    m_metrics.deepSleepCycles++;

    rtcData_.lastRequestedState = PowerState::DeepSleep;
    rtcData_.remainingSleepMs = remaining;
    rtcData_.pendingNfcWakeup = m_config.enableNfcWakeup ? 1 : 0;
    saveToRtcMemory();

    prepareForSleep(PowerState::DeepSleep);

    if (m_config.enableNfcWakeup)
    {
        LOG_INFO(m_name, "NFC wakeup via RST (wire PN532 IRQ -> RST)");
    }
    else
    {
        LOG_INFO(m_name, "Timer-only wakeup (NFC wakeup disabled)");
    }

    const auto oldState = m_currentState;
    m_currentState = PowerState::DeepSleep;
    publishStateChange(m_currentState, oldState);

    LOG_INFO(m_name, "Going to sleep NOW... goodbye!");
    delay(50);

    // Enter deep sleep - device resets on wakeup, code below never executes
    platform::deepSleep(static_cast<uint64_t>(actualDuration) * 1000ULL);
}

void PowerService::checkIdleTimeout()
{
    const auto idleMs = getTimeSinceLastActivityMs();

    // Log idle progress every second (approximately)
    static std::uint32_t lastLoggedSec = 0;
    const auto idleSec = idleMs / 1000;
    if (idleSec != lastLoggedSec && idleSec > 0)
    {
        lastLoggedSec = idleSec;
        const auto remainingSec = (m_config.idleTimeoutMs - idleMs) / 1000;
        if (remainingSec <= 5 || idleSec % 2 == 0) // Log more frequently near timeout
        {
            LOG_INFO(m_name, "Idle: %us / %us (sleep in %us)",
                     idleSec, m_config.idleTimeoutMs / 1000, remainingSec);
        }
    }

    if (idleMs >= m_config.idleTimeoutMs)
    {
        lastLoggedSec = 0; // Reset for next cycle
        const auto sleepState = selectSmartSleepDepth();
        LOG_INFO(m_name, ">>> IDLE TIMEOUT - selecting %s <<<", toString(sleepState));
        requestSleep(sleepState, getDurationForState(sleepState));
    }
}

void PowerService::checkChainedSleep()
{
    if (rtcData_.remainingSleepMs > 0)
    {
        LOG_INFO(m_name, "Continuing chained deep sleep: %ums remaining", rtcData_.remainingSleepMs);

        // Continue the chained sleep immediately
        const auto remaining{rtcData_.remainingSleepMs};
        rtcData_.remainingSleepMs = 0;
        enterDeepSleepAsync(remaining);
    }
}

void PowerService::prepareForSleep(const PowerState state)
{
    LOG_DEBUG(m_name, "Preparing for %s", toString(state));

    // TODO: notify services to prepare for sleep via event

    if (m_config.enableNfcWakeup && m_config.nfcWakeGatePin != 0xFF)
    {
        const bool enableGate = (state == PowerState::DeepSleep || state == PowerState::Hibernating);
        setNfcWakeGate(enableGate);
        if (enableGate)
        {
            delay(2);
        }
    }

    // Flush any pending serial output
    Serial.flush();

    // Allow other services to prepare via event
    // (SleepRequested event already emitted in requestSleep)
    yield();
}

void PowerService::notifyWakeup()
{
    publishWakeupOccurred(m_wakeupReason);
}

void PowerService::saveToRtcMemory()
{
    rtcData_.crc32 = calculateCrc32(rtcData_);
    platform::rtcUserMemoryWrite(0, reinterpret_cast<uint32_t *>(&rtcData_), sizeof(rtcData_));
    LOG_DEBUG(m_name, "Saved RTC data");
}

bool PowerService::loadFromRtcMemory()
{
    RtcData loaded{};
    platform::rtcUserMemoryRead(0, reinterpret_cast<uint32_t *>(&loaded), sizeof(loaded));

    if (!loaded.isValid())
    {
        LOG_DEBUG(m_name, "RTC data not valid (magic mismatch)");
        return false;
    }

    if (loaded.crc32 != calculateCrc32(loaded))
    {
        LOG_WARN(m_name, "RTC data CRC mismatch");
        return false;
    }

    rtcData_ = loaded;
    return true;
}

uint32_t PowerService::calculateCrc32(const RtcData &data)
{
    // Simple CRC32 calculation (excluding the crc32 field itself)
    const auto *bytes{reinterpret_cast<const uint8_t *>(&data)};
    const auto len{offsetof(RtcData, crc32)}; // Exclude crc32 field

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++)
        {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

WakeupReason PowerService::detectWakeupReason()
{
    return platform::detectWakeupReason();
}

void PowerService::publishStateChange(const PowerState newState, const PowerState oldState)
{
    Event event(EventType::PowerStateChange, PowerEvent{
                                                     .durationMs = 0,
                                                     .targetState = newState,
                                                     .previousState = oldState,
                                             });
    event.timestampMs = millis();
    m_bus.publish(event);
}

void PowerService::publishSleepRequested(const PowerState state, const std::uint32_t durationMs)
{
    Event event(EventType::SleepRequested, PowerEvent{
                                                   .durationMs = durationMs,
                                                   .targetState = state,
                                                   .previousState = m_currentState,
                                           });
    event.timestampMs = millis();
    m_bus.publish(event);
}

void PowerService::publishWakeupOccurred(const WakeupReason reason)
{
    Event event(EventType::WakeupOccurred, PowerEvent{
                                                   .durationMs = 0,
                                                   .targetState = PowerState::Active,
                                                   .previousState = m_currentState,
                                                   .wakeupReason = reason,
                                           });
    event.timestampMs = millis();
    m_bus.publish(event);
}

void PowerService::recordActivityInternal(const ActivityType type)
{
    // Check if this activity type is enabled in config
    if (!isActivityTypeEnabled(type))
    {
        return;
    }

    m_lastActivityMs = millis();
    LOG_DEBUG(m_name, "Activity recorded: type=%d", static_cast<uint8_t>(type));
}

bool PowerService::isActivityTypeEnabled(const ActivityType type) const
{
    return (m_config.activityTypeMask & static_cast<uint8_t>(type)) != 0;
}

void PowerService::recordActivity()
{
    m_lastActivityMs = millis();
}

void PowerService::setNfcWakeGate(const bool enabled)
{
    if (m_config.nfcWakeGatePin == 0xFF)
    {
        return;
    }

    pinMode(m_config.nfcWakeGatePin, OUTPUT);
    digitalWrite(m_config.nfcWakeGatePin, enabled ? HIGH : LOW);
}
} // namespace isic
