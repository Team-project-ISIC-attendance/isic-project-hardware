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

    m_wakeupReason = detectWakeupReason();
    m_metrics.lastWakeupReason = m_wakeupReason;
    LOG_INFO(m_name, "Wakeup reason: %s", toString(m_wakeupReason));

    // Load RTC data if waking from deep sleep
    if (m_wakeupReason == WakeupReason::Timer || m_wakeupReason == WakeupReason::External)
    {
        if (loadFromRtcMemory())
        {
            LOG_INFO(m_name, "Restored RTC data: wakeups=%u, totalSleepMs=%u", rtcData_.wakeupCount, rtcData_.totalSleepMs);

            m_metrics.wakeupCount = rtcData_.wakeupCount;
            m_metrics.totalDeepSleepMs = rtcData_.totalSleepMs;

            checkChainedSleep();
        }
    }
    else
    {
        // Fresh boot - initialize RTC data
        rtcData_ = RtcData{};
        rtcData_.magic = RtcData::MAGIC;
    }

    rtcData_.wakeupCount++;
    m_metrics.wakeupCount = rtcData_.wakeupCount;

    m_lastActivityMs = millis();
    m_metrics.lastActivityMs = m_lastActivityMs;

    m_currentState = PowerState::Active;
    m_metrics.currentState = m_currentState;

    setState(ServiceState::Ready);
    publishWakeupOccurred(m_wakeupReason);

    LOG_INFO(m_name, "Ready (wakeup #%u, smart=%d, mqttSleep=%d)", m_metrics.wakeupCount, m_config.smartSleepEnabled, m_config.modemSleepOnMqttDisconnect);
    return Status::Ok();
}

void PowerService::loop()
{
    if (m_state != ServiceState::Ready && m_state != ServiceState::Running)
    {
        return;
    }

    m_metrics.uptimeMs = millis();

    if (m_sleepPending)
    {
        if (const auto elapsed = millis() - m_sleepRequestedAtMs; elapsed >= PowerConfig::Constants::kSleepDelayMs)
        {
            executePendingSleep();
            m_sleepPending = false;
        }
        return; // Don't process other operations while sleep is pending
    }

    // Handle active light sleep timer (non-blocking)
    if (m_lightSleepActive)
    {
        if (const auto elapsed = millis() - m_lightSleepStartMs; elapsed >= m_lightSleepDurationMs)
        {
            // Wake from light sleep
            m_lightSleepActive = false;
            m_metrics.totalLightSleepMs += elapsed;

            m_currentState = PowerState::Active;
            m_metrics.currentState = m_currentState;
            publishStateChange(m_currentState, PowerState::LightSleep);
            recordActivity();

            // Transition to Running if we have dependencies
            if (m_wifiReady && m_config.autoSleepEnabled)
            {
                setState(ServiceState::Running);
            }

            LOG_INFO(m_name, "Woke from light sleep (%ums)", elapsed);
        }
        return;
    }

    // Handle active modem sleep timer (non-blocking)
    if (m_modemSleepActive)
    {
        if (const auto elapsed = millis() - m_modemSleepStartMs; elapsed >= m_modemSleepDurationMs)
        {
            // Wake from modem sleep
            wakeFromModemSleep();
        }
        return;
    }

    // State-specific logic
    switch (m_state)
    {
        case ServiceState::Ready: {
            handleReadyState();
            break;
        }
        case ServiceState::Running: {
            handleRunningState();
            break;
        }
        default: {
            break;
        }
    }
}

void PowerService::end()
{
    setState(ServiceState::Stopping);
    LOG_INFO(m_name, "Shutting down...");

    // Cancel any pending sleep
    if (m_sleepPending)
    {
        cancelSleepRequest();
    }

    // Wake from any active sleep
    if (m_lightSleepActive)
    {
        m_lightSleepActive = false;
    }
    if (m_modemSleepActive)
    {
        wakeFromModemSleep();
    }

    // Save state before shutdown
    saveToRtcMemory();

    eventConnections_.clear();

    setState(ServiceState::Stopped);
    LOG_INFO(m_name, "Stopped");
}

void PowerService::handleReadyState()
{
    // Transition to Running if auto-sleep is enabled and we're ready
    if (m_config.autoSleepEnabled && m_wifiReady)
    {
        setState(ServiceState::Running);
        LOG_INFO(m_name, "Transitioning to Running - auto-sleep active");
    }
}

void PowerService::handleRunningState()
{
    // Network-aware automatic modem sleep
    if (m_config.modemSleepOnMqttDisconnect && !m_mqttReady && !m_modemSleepActive && !m_sleepPending)
    {
        LOG_INFO(m_name, "MQTT disconnected, entering modem sleep");
        enterModemSleepAsync(m_config.modemSleepDurationMs);
        m_metrics.networkAwareSleeps++;
        return;
    }

    // Check idle timeout if auto-sleep is enabled
    if (m_config.autoSleepEnabled && !m_sleepPending && !m_lightSleepActive && !m_modemSleepActive)
    {
        checkIdleTimeout();
    }
}

void PowerService::handleWifiConnected(const Event & /* event */)
{
    LOG_DEBUG(m_name, "WiFi connected");
    m_wifiReady = true;
    recordActivityInternal(ActivityType::WifiConnected);

    // Transition to Running if auto-sleep is enabled
    if (m_config.autoSleepEnabled && m_state == ServiceState::Ready)
    {
        setState(ServiceState::Running);
    }
}

void PowerService::handleWifiDisconnected(const Event & /* event */)
{
    LOG_DEBUG(m_name, "WiFi disconnected");
    m_wifiReady = false;

    // Back to Ready state if we were Running
    if (m_state == ServiceState::Running)
    {
        setState(ServiceState::Ready);
    }
}

void PowerService::handleMqttConnected(const Event & /* event */)
{
    LOG_DEBUG(m_name, "MQTT connected");
    m_mqttReady = true;
    recordActivityInternal(ActivityType::MqttConnected);

    // If in modem sleep due to MQTT disconnect, wake up
    if (m_modemSleepActive)
    {
        LOG_INFO(m_name, "MQTT reconnected, waking from modem sleep");
        wakeFromModemSleep();
    }
}

void PowerService::handleMqttDisconnected(const Event & /* event */)
{
    LOG_DEBUG(m_name, "MQTT disconnected");
    m_mqttReady = false;
}

void PowerService::handleCardScanned(const Event & /* event */)
{
    recordActivityInternal(ActivityType::CardScanned);

    // Cancel pending sleep on card scan
    if (m_sleepPending)
    {
        LOG_DEBUG(m_name, "Card scan cancelled pending sleep");
        cancelSleepRequest();
    }
}

void PowerService::handleMqttMessage(const Event & /* event */)
{
    recordActivityInternal(ActivityType::MqttMessage);
}

void PowerService::handleNfcReady(const Event & /* event */)
{
    recordActivityInternal(ActivityType::NfcReady);
}

PowerState PowerService::selectSmartSleepDepth()
{
    // If smart sleep is disabled, use default light sleep
    if (!m_config.smartSleepEnabled)
    {
        return PowerState::LightSleep;
    }

    const auto estimatedIdleMs{estimateIdleDuration()};

    // Decision tree for sleep depth
    PowerState selectedState;
    if (estimatedIdleMs < m_config.smartSleepShortThresholdMs)
    {
        // Short idle: use light sleep (WiFi stays connected)
        selectedState = PowerState::LightSleep;
        LOG_DEBUG(m_name, "Smart sleep: light (estimated %ums idle)", estimatedIdleMs);
    }
    else if (estimatedIdleMs < m_config.smartSleepMediumThresholdMs)
    {
        // Medium idle: use modem sleep if MQTT down, otherwise light sleep
        if (!m_mqttReady)
        {
            selectedState = PowerState::ModemSleep;
            LOG_DEBUG(m_name, "Smart sleep: modem (MQTT down, %ums idle)", estimatedIdleMs);
        }
        else
        {
            selectedState = PowerState::LightSleep;
            LOG_DEBUG(m_name, "Smart sleep: light (MQTT up, %ums idle)", estimatedIdleMs);
        }
    }
    else
    {
        // Long idle: use deep sleep (check if operations are complete)
        if (canEnterSleep())
        {
            selectedState = PowerState::DeepSleep;
            LOG_DEBUG(m_name, "Smart sleep: deep (%ums idle)", estimatedIdleMs);
        }
        else
        {
            // Pending operations - use modem sleep
            selectedState = PowerState::ModemSleep;
            LOG_DEBUG(m_name, "Smart sleep: modem (pending ops, %ums idle)", estimatedIdleMs);
        }
    }

    m_metrics.smartSleepDecisions++;
    return selectedState;
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

void PowerService::requestSleep(const PowerState state, std::uint32_t durationMs)
{
    if (state == PowerState::Active)
    {
        LOG_WARN(m_name, "Cannot request sleep to Active state");
        return;
    }

    if (durationMs == 0)
    {
        switch (state)
        {
            case PowerState::LightSleep: {
                durationMs = m_config.lightSleepDurationMs;
                break;
            }
            case PowerState::ModemSleep: {
                durationMs = m_config.modemSleepDurationMs;
                break;
            }
            case PowerState::DeepSleep:
            case PowerState::Hibernating: {
                durationMs = m_config.sleepIntervalMs;
                break;
            }
            default: {
                break;
            }
        }
    }

    LOG_INFO(m_name, "Sleep requested: state=%s, duration=%ums", toString(state), durationMs);
    publishSleepRequested(state, durationMs);

    // Queue the sleep request (allow time for event handlers)
    m_sleepPending = true;
    m_pendingSleepState = state;
    m_pendingSleepDurationMs = durationMs;
    m_sleepRequestedAtMs = millis();
}

void PowerService::cancelSleepRequest()
{
    if (m_sleepPending)
    {
        LOG_INFO(m_name, "Sleep request cancelled");
        m_sleepPending = false;
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
    LOG_INFO(m_name, "Entering light sleep for %ums (async)", durationMs);

    const auto oldState{m_currentState};
    m_currentState = PowerState::LightSleep;
    m_metrics.currentState = m_currentState;
    m_metrics.lightSleepCycles++;

    publishStateChange(m_currentState, oldState);

    // Start non-blocking light sleep timer
    m_lightSleepActive = true;
    m_lightSleepStartMs = millis();
    m_lightSleepDurationMs = durationMs;

    // Services handle their own power state via PowerStateChange event
    // WiFiService will configure light sleep mode
    // Pn532Service will enter low-power mode

    LOG_DEBUG(m_name, "Light sleep timer started");
}

void PowerService::enterModemSleepAsync(const std::uint32_t durationMs)
{
    LOG_INFO(m_name, "Entering modem sleep for %ums", durationMs);

    const auto oldState{m_currentState};
    m_currentState = PowerState::ModemSleep;
    m_metrics.currentState = m_currentState;
    m_metrics.modemSleepCycles++;

    publishStateChange(m_currentState, oldState);

    // Start non-blocking modem sleep timer
    m_modemSleepActive = true;
    m_modemSleepStartMs = millis();
    m_modemSleepDurationMs = durationMs;

    // WiFiService subscribes to PowerStateChange and will turn off RF
    // Pn532Service subscribes and will enter low-power mode

    LOG_INFO(m_name, "Modem sleep active");
}

void PowerService::wakeFromModemSleep()
{
    if (!m_modemSleepActive)
    {
        return;
    }

    LOG_INFO(m_name, "Waking from modem sleep");

    m_modemSleepActive = false;
    m_metrics.totalModemSleepMs += (millis() - m_modemSleepStartMs);

    const auto oldState{m_currentState};
    m_currentState = PowerState::Active;
    m_metrics.currentState = m_currentState;
    publishStateChange(m_currentState, oldState);

    recordActivity();
}

void PowerService::enterDeepSleepAsync(const std::uint32_t durationMs)
{
    // Deep sleep is inherently blocking (device resets on wakeup)
    // This "async" just means we delayed before calling it

    // Clamp duration to ESP8266 limit
    std::uint32_t actualDuration{durationMs};
    std::uint32_t remaining{0};

    if (durationMs > m_config.maxDeepSleepMs)
    {
        actualDuration = m_config.maxDeepSleepMs;
        remaining = durationMs - actualDuration;
        LOG_INFO(m_name, "Deep sleep chained: sleeping %ums, remaining %ums", actualDuration, remaining);
    }

    LOG_INFO(m_name, "Entering deep sleep for %ums", actualDuration);

    m_metrics.deepSleepCycles++;
    m_metrics.totalDeepSleepMs += actualDuration;

    rtcData_.lastRequestedState = PowerState::DeepSleep;
    rtcData_.remainingSleepMs = remaining;
    rtcData_.totalSleepMs = m_metrics.totalDeepSleepMs;
    saveToRtcMemory();

    prepareForSleep(PowerState::DeepSleep);

    const auto oldState{m_currentState};
    m_currentState = PowerState::DeepSleep;
    m_metrics.currentState = m_currentState;
    publishStateChange(m_currentState, oldState);

    // TODO: give services time to prepare for deep sleep
    delay(100); // is blocks so no ok rewrite in the meantime

    // Enter deep sleep - execution stops here
    const auto sleepUs{static_cast<uint64_t>(actualDuration) * 1000ULL};
    platform::deepSleep(sleepUs);

    // TODO: give services time to prepare for deep sleep Note: Execution stops here. Device resets on wakeup.
    delay(100); // is blocks so no ok rewrite in the meantime
}

void PowerService::checkIdleTimeout()
{
    if (const auto idleMs = getTimeSinceLastActivityMs(); idleMs >= m_config.idleTimeoutMs)
    {
        LOG_INFO(m_name, "Idle timeout reached (%ums)", idleMs);
        m_metrics.idleTimeoutsTriggered++;

        const auto sleepState{selectSmartSleepDepth()};

        std::uint32_t duration;
        switch (sleepState)
        {
            case PowerState::LightSleep: {
                duration = m_config.lightSleepDurationMs;
                break;
            }
            case PowerState::ModemSleep: {
                duration = m_config.modemSleepDurationMs;
                break;
            }
            case PowerState::DeepSleep: {
                duration = m_config.sleepIntervalMs;
                break;
            }
            default: {
                duration = m_config.lightSleepDurationMs;
                break;
            }
        }

        requestSleep(sleepState, duration);
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
                                                     .targetState = newState,
                                                     .previousState = oldState,
                                             });
    event.timestampMs = millis();
    m_bus.publish(event);
}

void PowerService::publishSleepRequested(const PowerState state, const std::uint32_t durationMs)
{
    Event event(EventType::SleepRequested, PowerEvent{
                                                   .targetState = state,
                                                   .previousState = m_currentState,
                                                   .durationMs = durationMs,
                                           });
    event.timestampMs = millis();
    m_bus.publish(event);
}

void PowerService::publishWakeupOccurred(const WakeupReason reason)
{
    Event event(EventType::WakeupOccurred, PowerEvent{
                                                   .targetState = PowerState::Active,
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
    m_metrics.lastActivityMs = m_lastActivityMs;
    LOG_DEBUG(m_name, "Activity recorded: type=%d", static_cast<uint8_t>(type));
}

bool PowerService::isActivityTypeEnabled(const ActivityType type) const
{
    return (m_config.activityTypeMask & static_cast<uint8_t>(type)) != 0;
}

void PowerService::recordActivity()
{
    m_lastActivityMs = millis();
    m_metrics.lastActivityMs = m_lastActivityMs;
}
} // namespace isic
