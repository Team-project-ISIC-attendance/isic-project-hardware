#include "services/PowerService.hpp"

#include "common/Logger.hpp"
#include "platform/PlatformPower.hpp"
#include "services/AttendanceService.hpp"

namespace isic
{

PowerService::PowerService(EventBus &bus, const PowerConfig &config, const AttendanceService &attendanceService)
    : ServiceBase("PowerService")
    , m_bus(bus)
    , m_config(config)
    , m_attendanceService(attendanceService)
{
    eventConnections_.reserve(12);
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
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::WifiApStarted, [this](const Event &e) {
        handleWifiApStarted(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::WifiApStopped, [this](const Event &e) {
        handleWifiApStopped(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::OtaStarted, [this](const Event &e) {
        handleOtaStarted(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::OtaCompleted, [this](const Event &e) {
        handleOtaCompleted(e);
    }));
    eventConnections_.push_back(m_bus.subscribeScoped(EventType::OtaError, [this](const Event &e) {
        handleOtaError(e);
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
    m_lastActivityMs = millis();
    m_lastCardActivityMs = m_lastActivityMs;
    m_currentState = PowerState::Active;
    m_pn532TargetMode = Pn532PowerMode::ActiveScan;
    m_burstMode = false;
    m_burstSleepSuppressionActive = false;
    m_recentCardScanMs.fill(0);
    m_recentCardScanIndex = 0;

    setState(ServiceState::Running);
    publishWakeupOccurred(m_wakeupReason);

    LOG_INFO(m_name,
             "Ready (readerIdle=%ums, modemAfter=%ums, mqttSleep=%d)",
             m_config.readerIdleTimeoutMs,
             m_config.modemSleepAfterMs,
             m_config.modemSleepOnMqttDisconnect);
    return Status::Ok();
}

void PowerService::loop()
{
    if (m_state != ServiceState::Running)
    {
        return;
    }

    const auto now{millis()};
    updateBurstState(now);
    updatePn532Target(computeDesiredPn532Mode(now));

    if (m_sleepPending)
    {
        if (const auto elapsed = now - m_sleepRequestedAtMs; elapsed >= PowerConfig::Constants::kSleepDelayMs)
        {
            executePendingSleep();
        }
        return;
    }

    const auto desiredState{computeDesiredEspState(now)};
    if (desiredState == PowerState::Active)
    {
        if (m_currentState != PowerState::Active)
        {
            wakeToActive();
        }
        return;
    }

    if (m_currentState == PowerState::ModemSleep && desiredState == PowerState::ModemSleep)
    {
        return;
    }

    if (desiredState == PowerState::LightSleep && m_currentState == PowerState::Active)
    {
        requestSleep(PowerState::LightSleep);
        return;
    }

    if (desiredState == PowerState::ModemSleep && m_currentState != PowerState::ModemSleep)
    {
        requestSleep(PowerState::ModemSleep);
    }
}

void PowerService::end()
{
    if (m_state == ServiceState::Stopped)
    {
        return;
    }

    setState(ServiceState::Stopping);
    LOG_INFO(m_name, "Shutting down...");

    cancelSleepRequest();
    m_currentState = PowerState::Active;
    eventConnections_.clear();

    setState(ServiceState::Stopped);
    LOG_INFO(m_name, "Stopped");
}

void PowerService::handleWifiConnected(const Event & /* event */)
{
    LOG_DEBUG(m_name, "WiFi connected");
    m_wifiReady = true;
    recordActivityInternal(ActivityType::WifiConnected);
}

void PowerService::handleWifiDisconnected(const Event & /* event */)
{
    LOG_DEBUG(m_name, "WiFi disconnected");
    m_wifiReady = false;
}

void PowerService::handleMqttConnected(const Event & /* event */)
{
    LOG_DEBUG(m_name, "MQTT connected");
    m_mqttReady = true;
    recordActivityInternal(ActivityType::MqttConnected);
}

void PowerService::handleMqttDisconnected(const Event & /* event */)
{
    LOG_DEBUG(m_name, "MQTT disconnected");
    m_mqttReady = false;

    const auto now{millis()};
    if (m_config.modemSleepOnMqttDisconnect && m_currentState != PowerState::ModemSleep)
    {
        if (m_burstMode || isReaderReadyHoldActive(now))
        {
            if (m_burstMode)
            {
                ++m_metrics.sleepSuppressedByBurst;
            }
            LOG_INFO(m_name, "Skipping MQTT-driven modem sleep while reader traffic is active");
            return;
        }

        ++m_metrics.networkAwareSleeps;
        requestSleep(PowerState::ModemSleep);
    }
}

void PowerService::handleCardScanned(const Event &event)
{
    const auto *card{event.get<CardEvent>()};
    const auto now{card != nullptr ? card->timestampMs : millis()};

    recordCardActivity(now);
    recordActivityInternal(ActivityType::CardScanned);
    wakeToActive();
}

void PowerService::handleMqttMessage(const Event & /* event */)
{
    recordActivityInternal(ActivityType::MqttMessage);
}

void PowerService::handleNfcReady(const Event & /* event */)
{
    recordActivityInternal(ActivityType::NfcReady);
}

void PowerService::handleWifiApStarted(const Event & /* event */)
{
    m_apModeActive = true;
    recordActivity();
    wakeToActive();
}

void PowerService::handleWifiApStopped(const Event & /* event */)
{
    m_apModeActive = false;
    recordActivity();
}

void PowerService::handleOtaStarted(const Event & /* event */)
{
    m_otaUpdateActive = true;
    recordActivity();
    wakeToActive();
}

void PowerService::handleOtaCompleted(const Event & /* event */)
{
    m_otaUpdateActive = false;
    recordActivity();
}

void PowerService::handleOtaError(const Event & /* event */)
{
    m_otaUpdateActive = false;
    recordActivity();
}

bool PowerService::canEnterSleep(const PowerState state, SleepBlockReason *reason) const
{
    if (reason != nullptr)
    {
        *reason = SleepBlockReason::None;
    }

    if (state == PowerState::Active)
    {
        return false;
    }

    if (m_apModeActive)
    {
        if (reason != nullptr)
        {
            *reason = SleepBlockReason::AccessPoint;
        }
        return false;
    }

    if (m_otaUpdateActive)
    {
        if (reason != nullptr)
        {
            *reason = SleepBlockReason::OtaUpdate;
        }
        return false;
    }

    // Don't sleep while MQTT is up and there are queued offline records to flush
    if (m_mqttReady && m_attendanceService.getOfflineBufferSize() > 0)
    {
        if (reason != nullptr)
        {
            *reason = SleepBlockReason::OfflinePending;
        }
        return false;
    }

    return true;
}

void PowerService::noteSleepBlocked(const SleepBlockReason reason)
{
    ++m_metrics.sleepBlocked;

    switch (reason)
    {
        case SleepBlockReason::AccessPoint:
            ++m_metrics.sleepBlockedByAp;
            LOG_DEBUG(m_name, "Sleep blocked: AP mode active");
            break;
        case SleepBlockReason::OtaUpdate:
            ++m_metrics.sleepBlockedByOta;
            LOG_DEBUG(m_name, "Sleep blocked: OTA update in progress");
            break;
        case SleepBlockReason::OfflinePending:
            LOG_DEBUG(m_name, "Sleep blocked: offline records pending flush");
            break;
        case SleepBlockReason::None:
        default:
            LOG_DEBUG(m_name, "Sleep blocked");
            break;
    }
}

void PowerService::evaluateIdleState()
{
    if (!m_config.autoSleepEnabled)
    {
        return;
    }

    const auto idleMs = getTimeSinceLastActivityMs();
    if (idleMs >= m_config.modemSleepAfterMs)
    {
        if (m_currentState != PowerState::ModemSleep)
        {
            requestSleep(PowerState::ModemSleep);
        }
        return;
    }

    if (idleMs >= m_config.readerIdleTimeoutMs && m_currentState == PowerState::Active)
    {
        requestSleep(PowerState::LightSleep);
    }
}

void PowerService::updateBurstState(const std::uint32_t nowMs)
{
    if (m_burstMode && (nowMs - m_lastCardActivityMs) >= m_config.burstHoldMs)
    {
        m_burstMode = false;
        ++m_metrics.burstExits;
        LOG_INFO(m_name, "Reader burst mode ended after quiet period");
    }

    const bool burstSuppressingSleep = m_burstMode && getTimeSinceLastActivityMs() >= m_config.readerIdleTimeoutMs;
    if (burstSuppressingSleep)
    {
        if (!m_burstSleepSuppressionActive)
        {
            ++m_metrics.sleepSuppressedByBurst;
            m_burstSleepSuppressionActive = true;
            LOG_INFO(m_name, "Reader burst traffic is holding ESP awake");
        }
    }
    else
    {
        m_burstSleepSuppressionActive = false;
    }
}

void PowerService::recordCardActivity(const std::uint32_t timestampMs)
{
    m_lastCardActivityMs = timestampMs;
    m_recentCardScanMs[m_recentCardScanIndex] = timestampMs;
    m_recentCardScanIndex = (m_recentCardScanIndex + 1) % m_recentCardScanMs.size();

    const auto burstScanThreshold = std::max<std::uint8_t>(1, m_config.burstScanCount);
    if (!m_burstMode && countRecentCardScans(timestampMs) >= burstScanThreshold)
    {
        m_burstMode = true;
        m_burstSleepSuppressionActive = false;
        ++m_metrics.burstEntries;
        LOG_INFO(m_name, "Reader burst mode enabled");
    }
}

std::uint8_t PowerService::countRecentCardScans(const std::uint32_t nowMs) const
{
    std::uint8_t count{0};
    for (const auto scanMs: m_recentCardScanMs)
    {
        if (scanMs != 0 && (nowMs - scanMs) <= m_config.burstWindowMs)
        {
            ++count;
        }
    }
    return count;
}

bool PowerService::isReaderReadyHoldActive(const std::uint32_t nowMs) const
{
    return m_lastCardActivityMs != 0 && (nowMs - m_lastCardActivityMs) < m_config.readerReadyHoldMs;
}

bool PowerService::shouldKeepEspAwakeForTraffic(const std::uint32_t nowMs) const
{
    return m_burstMode || isReaderReadyHoldActive(nowMs);
}

PowerState PowerService::computeDesiredEspState(const std::uint32_t nowMs) const
{
    if (!m_config.autoSleepEnabled)
    {
        return m_currentState;
    }

    if (shouldKeepEspAwakeForTraffic(nowMs))
    {
        return PowerState::Active;
    }

    const auto idleMs = nowMs - m_lastActivityMs;
    if (idleMs >= m_config.modemSleepAfterMs)
    {
        return PowerState::ModemSleep;
    }

    if (idleMs >= m_config.readerIdleTimeoutMs)
    {
        return PowerState::LightSleep;
    }

    return PowerState::Active;
}

Pn532PowerMode PowerService::computeDesiredPn532Mode(const std::uint32_t nowMs) const
{
    if (!m_config.pn532SleepBetweenScans)
    {
        return Pn532PowerMode::ActiveScan;
    }

    if (m_currentState == PowerState::ModemSleep)
    {
        return Pn532PowerMode::PowerDown;
    }

    if (!m_config.autoSleepEnabled)
    {
        return Pn532PowerMode::ActiveScan;
    }

    if (m_burstMode || isReaderReadyHoldActive(nowMs))
    {
        return Pn532PowerMode::ActiveScan;
    }

    if ((nowMs - m_lastCardActivityMs) < m_config.pn532SleepAfterMs)
    {
        return Pn532PowerMode::ActiveScan;
    }

    return Pn532PowerMode::PowerDown;
}

void PowerService::updatePn532Target(const Pn532PowerMode targetMode)
{
    if (targetMode == m_pn532TargetMode)
    {
        return;
    }

    m_pn532TargetMode = targetMode;
    publishStateChange(m_currentState, m_currentState, m_pn532TargetMode);
    LOG_INFO(m_name, "PN532 target mode -> %s", toString(m_pn532TargetMode));
}

void PowerService::requestSleep(const PowerState state, const std::uint32_t durationMs)
{
    if (state == PowerState::Active)
    {
        LOG_WARN(m_name, "Cannot request sleep to Active state");
        return;
    }

    if (durationMs != 0)
    {
        LOG_DEBUG(m_name, "Ignoring legacy sleep duration for %s", toString(state));
    }

    if (m_currentState == state && !m_sleepPending)
    {
        return;
    }

    LOG_INFO(m_name, "Sleep requested: state=%s", toString(state));
    publishSleepRequested(state, 0);

    m_sleepPending = true;
    m_pendingSleepState = state;
    m_pendingSleepDurationMs = 0;
    m_sleepRequestedAtMs = millis();
}

void PowerService::cancelSleepRequest()
{
    if (m_sleepPending)
    {
        LOG_INFO(m_name, "Sleep request cancelled");
        m_sleepPending = false;
        m_pendingSleepState = PowerState::Active;
        m_pendingSleepDurationMs = 0;
    }
}

void PowerService::executePendingSleep()
{
    const auto pendingState = m_pendingSleepState;
    m_sleepPending = false;
    m_pendingSleepState = PowerState::Active;
    m_pendingSleepDurationMs = 0;

    if (pendingState == PowerState::Active || pendingState == m_currentState)
    {
        return;
    }

    SleepBlockReason reason{SleepBlockReason::None};
    if (!canEnterSleep(pendingState, &reason))
    {
        noteSleepBlocked(reason);
        return;
    }

    switch (pendingState)
    {
        case PowerState::LightSleep:
            enterLightSleep();
            break;
        case PowerState::ModemSleep:
            enterModemSleep();
            break;
        case PowerState::Active:
        default:
            break;
    }
}

void PowerService::enterLightSleep()
{
    if (m_currentState == PowerState::LightSleep)
    {
        return;
    }

    prepareForSleep(PowerState::LightSleep);

    const auto oldState = m_currentState;
    m_currentState = PowerState::LightSleep;
    ++m_metrics.lightSleepEntries;

    publishStateChange(m_currentState, oldState, m_pn532TargetMode);
    LOG_INFO(m_name, "Reader entered light sleep");
}

void PowerService::enterModemSleep()
{
    if (m_currentState == PowerState::ModemSleep)
    {
        return;
    }

    prepareForSleep(PowerState::ModemSleep);

    const auto oldState = m_currentState;
    m_currentState = PowerState::ModemSleep;
    m_pn532TargetMode = Pn532PowerMode::PowerDown;
    ++m_metrics.modemSleepEntries;

    publishStateChange(m_currentState, oldState, m_pn532TargetMode);
    LOG_INFO(m_name, "Reader entered modem sleep");
}

void PowerService::wakeToActive()
{
    if (m_sleepPending)
    {
        cancelSleepRequest();
    }

    const auto oldState = m_currentState;
    if (oldState == PowerState::Active)
    {
        recordActivity();
        updatePn532Target(computeDesiredPn532Mode(millis()));
        return;
    }

    m_currentState = PowerState::Active;
    if (oldState == PowerState::LightSleep)
    {
        ++m_metrics.lightSleepWakeups;
    }
    else if (oldState == PowerState::ModemSleep)
    {
        ++m_metrics.modemSleepWakeups;
    }

    m_pn532TargetMode = computeDesiredPn532Mode(millis());
    publishStateChange(m_currentState, oldState, m_pn532TargetMode);
    recordActivity();

    LOG_INFO(m_name, "Reader woke from %s", toString(oldState));
}

void PowerService::prepareForSleep(const PowerState state)
{
    LOG_DEBUG(m_name, "Preparing for %s", toString(state));
    Serial.flush();
    yield();
}

WakeupReason PowerService::detectWakeupReason()
{
    return platform::detectWakeupReason();
}

void PowerService::publishStateChange(const PowerState newState, const PowerState oldState, const Pn532PowerMode pn532TargetMode)
{
    m_bus.publish({EventType::PowerStateChange,
                   PowerEvent{
                           .durationMs = 0,
                           .targetState = newState,
                           .previousState = oldState,
                           .pn532TargetMode = pn532TargetMode,
                   }});
}

void PowerService::publishSleepRequested(const PowerState state, const std::uint32_t durationMs)
{
    m_bus.publish({EventType::SleepRequested,
                   PowerEvent{
                           .durationMs = durationMs,
                           .targetState = state,
                           .previousState = m_currentState,
                           .pn532TargetMode = state == PowerState::Active ? m_pn532TargetMode : Pn532PowerMode::PowerDown,
                   }});
}

void PowerService::publishWakeupOccurred(const WakeupReason reason)
{
    m_bus.publish({EventType::WakeupOccurred,
                   PowerEvent{
                           .durationMs = 0,
                           .targetState = PowerState::Active,
                           .previousState = m_currentState,
                           .pn532TargetMode = m_pn532TargetMode,
                           .wakeupReason = reason,
                   }});
}

void PowerService::recordActivityInternal(const ActivityType type)
{
    if (!isActivityTypeEnabled(type))
    {
        return;
    }

    m_lastActivityMs = millis();
    LOG_DEBUG(m_name, "Activity recorded: type=%d", static_cast<std::uint8_t>(type));
}

bool PowerService::isActivityTypeEnabled(const ActivityType type) const
{
    return (m_config.activityTypeMask & static_cast<std::uint8_t>(type)) != 0;
}

void PowerService::recordActivity()
{
    m_lastActivityMs = millis();
}
} // namespace isic
