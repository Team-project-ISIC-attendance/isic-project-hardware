#include "services/ConfigService.hpp"

#include "common/Logger.hpp"

namespace isic
{
namespace
{
bool endsWith(const std::string &str, const char *suffix) noexcept
{
    const auto suffixLen{strlen(suffix)};

    if (str.length() < suffixLen)
    {
        return false;
    }

    return str.compare(str.length() - suffixLen, suffixLen, suffix) == 0;
}

bool parseString(const JsonVariant &json, const char *key, std::string &target)
{
    if (json[key].is<const char *>())
    {
        target = json[key].as<const char *>();
        return true;
    }

    return false;
}

template<typename T>
bool parseNumber(const JsonVariant &json, const char *key, T &target)
{
    if (json[key].is<T>())
    {
        target = json[key].as<T>();
        return true;
    }

    return false;
}

bool parseBool(const JsonVariant &json, const char *key, bool &target)
{
    if (json[key].is<bool>())
    {
        target = json[key].as<bool>();
        return true;
    }

    return false;
}

std::string serializeToJson(const Config &config)
{
    JsonDocument doc;

    // Version and magic for validation
    doc["magic"] = config.magic;
    doc["version"] = config.version;

    // WiFi
    const auto wifiConfig{config.wifi};
    const auto wifi{doc["wifi"].to<JsonObject>()};
    wifi["stationSsid"] = wifiConfig.stationSsid;
    wifi["stationPassword"] = wifiConfig.stationPassword;
    wifi["stationConnectRetryDelayMs"] = wifiConfig.stationConnectRetryDelayMs;
    wifi["stationConnectionTimeoutMs"] = wifiConfig.stationConnectionTimeoutMs;
    wifi["stationMaxConnectionAttempts"] = wifiConfig.stationMaxConnectionAttempts;
    wifi["stationPowerSaveEnabled"] = wifiConfig.stationPowerSaveEnabled;
    wifi["accessPointSsidPrefix"] = wifiConfig.accessPointSsidPrefix;
    wifi["accessPointPassword"] = wifiConfig.accessPointPassword;
    wifi["accessPointModeTimeoutMs"] = wifiConfig.accessPointModeTimeoutMs;

    // MQTT
    const auto mqttConfig{config.mqtt};
    const auto mqtt{doc["mqtt"].to<JsonObject>()};
    mqtt["brokerAddress"] = mqttConfig.brokerAddress;
    mqtt["port"] = mqttConfig.port;
    mqtt["username"] = mqttConfig.username;
    mqtt["password"] = mqttConfig.password;
    mqtt["baseTopic"] = mqttConfig.baseTopic;
    mqtt["keepAliveIntervalSec"] = mqttConfig.keepAliveIntervalSec;
    mqtt["reconnectMinIntervalMs"] = mqttConfig.reconnectMinIntervalMs;
    mqtt["reconnectMaxIntervalMs"] = mqttConfig.reconnectMaxIntervalMs;

    // Device
    const auto deviceConfig{config.device};
    const auto device{doc["device"].to<JsonObject>()};
    device["deviceId"] = deviceConfig.deviceId;
    device["locationId"] = deviceConfig.locationId;

    // PN532
    const auto pn532Config{config.pn532};
    const auto pn532{doc["pn532"].to<JsonObject>()};
    pn532["spiSckPin"] = pn532Config.spiSckPin;
    pn532["spiMisoPin"] = pn532Config.spiMisoPin;
    pn532["spiMosiPin"] = pn532Config.spiMosiPin;
    pn532["spiCsPin"] = pn532Config.spiCsPin;
    pn532["irqPin"] = pn532Config.irqPin;
    pn532["resetPin"] = pn532Config.resetPin;
    pn532["pollIntervalMs"] = pn532Config.pollIntervalMs;
    pn532["readTimeoutMs"] = pn532Config.readTimeoutMs;
    pn532["maxConsecutiveErrors"] = pn532Config.maxConsecutiveErrors;
    pn532["recoveryDelayMs"] = pn532Config.recoveryDelayMs;

    // Attendance
    const auto attendanceConfig{config.attendance};
    const auto attendance{doc["attendance"].to<JsonObject>()};
    attendance["debounceIntervalMs"] = attendanceConfig.debounceIntervalMs;
    attendance["batchMaxSize"] = attendanceConfig.batchMaxSize;
    attendance["batchFlushIntervalMs"] = attendanceConfig.batchFlushIntervalMs;
    attendance["offlineBufferSize"] = attendanceConfig.offlineBufferSize;
    attendance["offlineBufferFlushIntervalMs"] = attendanceConfig.offlineBufferFlushIntervalMs;
    attendance["batchingEnabled"] = attendanceConfig.batchingEnabled;
    attendance["offlineQueuePolicy"] = static_cast<uint8_t>(attendanceConfig.offlineQueuePolicy);

    // Feedback
    const auto feedbackConfig{config.feedback};
    const auto feedback{doc["feedback"].to<JsonObject>()};
    feedback["enabled"] = feedbackConfig.enabled;
    feedback["ledEnabled"] = feedbackConfig.ledEnabled;
    feedback["ledPin"] = feedbackConfig.ledPin;
    feedback["buzzerEnabled"] = feedbackConfig.buzzerEnabled;
    feedback["buzzerPin"] = feedbackConfig.buzzerPin;
    feedback["ledActiveHigh"] = feedbackConfig.ledActiveHigh;
    feedback["beepFrequencyHz"] = feedbackConfig.beepFrequencyHz;
    feedback["successBlinkDurationMs"] = feedbackConfig.successBlinkDurationMs;
    feedback["errorBlinkDurationMs"] = feedbackConfig.errorBlinkDurationMs;

    // Health
    const auto healthConfig{config.health};
    const auto health{doc["health"].to<JsonObject>()};
    health["healthCheckIntervalMs"] = healthConfig.healthCheckIntervalMs;
    health["statusUpdateIntervalMs"] = healthConfig.statusUpdateIntervalMs;
    health["enabled"] = healthConfig.enabled;
    health["publishToMqtt"] = healthConfig.publishToMqtt;
    health["publishToLog"] = healthConfig.publishToLog;

    // OTA
    const auto otaConfig{config.ota};
    const auto ota{doc["ota"].to<JsonObject>()};
    ota["enabled"] = otaConfig.enabled;
    ota["updateServerUrl"] = otaConfig.updateServerUrl;
    ota["username"] = otaConfig.username;
    ota["password"] = otaConfig.password;

    // Power
    const auto powerConfig{config.power};
    const auto power{doc["power"].to<JsonObject>()};
    power["sleepIntervalMs"] = powerConfig.sleepIntervalMs;
    power["maxDeepSleepMs"] = powerConfig.maxDeepSleepMs;
    power["lightSleepDurationMs"] = powerConfig.lightSleepDurationMs;
    power["idleTimeoutMs"] = powerConfig.idleTimeoutMs;
    power["enableTimerWakeup"] = powerConfig.enableTimerWakeup;
    power["enableNfcWakeup"] = powerConfig.enableNfcWakeup;
    power["nfcWakeupPin"] = powerConfig.nfcWakeupPin;
    power["autoSleepEnabled"] = powerConfig.autoSleepEnabled;
    power["disableWiFiDuringSleep"] = powerConfig.disableWiFiDuringSleep;
    power["pn532SleepBetweenScans"] = powerConfig.pn532SleepBetweenScans;
    power["smartSleepEnabled"] = powerConfig.smartSleepEnabled;
    power["modemSleepOnMqttDisconnect"] = powerConfig.modemSleepOnMqttDisconnect;
    power["modemSleepDurationMs"] = powerConfig.modemSleepDurationMs;
    power["smartSleepShortThresholdMs"] = powerConfig.smartSleepShortThresholdMs;
    power["smartSleepMediumThresholdMs"] = powerConfig.smartSleepMediumThresholdMs;
    power["activityTypeMask"] = powerConfig.activityTypeMask;

    std::string result;
    result.reserve(1024); // Pre-allocate for typical config size
    serializeJson(doc, result);
    return result;
}

// Macros for parsing fields and setting 'changed' flag if updated, just to reduce code duplication
// Parse string field: PARSE_STR(json, "fieldName", config.field)
#define PARSE_STR(json, key, field)        \
    do                                     \
    {                                      \
        if (parseString(json, key, field)) \
            changed = true;                \
    }                                      \
    while (0)

// Parse numeric field: PARSE_NUM(json, "fieldName", config.field)
#define PARSE_NUM(json, key, field)        \
    do                                     \
    {                                      \
        if (parseNumber(json, key, field)) \
            changed = true;                \
    }                                      \
    while (0)

// Parse bool field: PARSE_BOOL(json, "fieldName", config.field)
#define PARSE_BOOL(json, key, field)     \
    do                                   \
    {                                    \
        if (parseBool(json, key, field)) \
            changed = true;              \
    }                                    \
    while (0)
} // namespace

ConfigService::ConfigService(EventBus &bus)
    : ServiceBase("ConfigService")
    , m_bus(bus)
{

    m_eventConnections.reserve(3);
    m_eventConnections.push_back(
            m_bus.subscribeScoped(EventType::MqttConnected, [this](const Event &) {
                m_bus.publish(Event{EventType::MqttSubscribeRequest, MqttEvent{.topic = "config/set/#"}});
            }));
    m_eventConnections.push_back(
            m_bus.subscribeScoped(EventType::MqttMessage, [this](const Event &event) {
                if (const auto *mqtt = event.get<MqttEvent>(); mqtt && mqtt->topic.find("/config/set") != std::string::npos)
                {
                    handleConfigMessage(mqtt->topic, mqtt->payload);
                }
            }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::ConfigChanged, [this](const Event &) {
        // Mark config as dirty so it gets saved in next loop() iteration
        LOG_INFO(m_name, "Configuration changed, marking as dirty");
        m_dirty = true;
    }));
}

ConfigService::~ConfigService()
{
    if (m_dirty)
    {
        (void) save(); // TODO: handle failure? i think in destructor we can't do much about it so just ignore
    }
}

Status ConfigService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing (version=%u, magic=0x%08X)...", Config::kVersion, Config::kMagicNumber);

    // Initialize LittleFS
    if (!LittleFS.begin())
    {
        LOG_ERROR(m_name, "LittleFS mount failed, formatting...");
        if (!LittleFS.format() || !LittleFS.begin())
        {
            setState(ServiceState::Error);
            return Status::Error("LittleFS init failed");
        }
    }

    // Load configuration
    if (load().failed())
    {
        LOG_WARN(m_name, "Load failed or version mismatch, resetting to defaults");
        m_config.restoreDefaults();

        // Delete old config file to clean up incompatible data
        if (LittleFS.exists(CONFIG_FILE))
        {
            LOG_INFO(m_name, "Removing old config file");
            LittleFS.remove(CONFIG_FILE);
        }

        (void) save(); // TODO: handle failure?
    }

    setState(ServiceState::Running);
    LOG_INFO(m_name, "Ready, device=%s, fw=%s", m_config.device.deviceId.c_str(), DeviceConfig::Constants::kFirmwareVersion);
    return Status::Ok();
}

void ConfigService::loop()
{
    if (m_dirty)
    {
        (void) save(); // TODO: handle failure?
        m_dirty = false;
    }
}

void ConfigService::end()
{
    if (m_dirty)
    {
        (void) save(); // TODO: handle failure?
    }

    m_eventConnections.clear();
    setState(ServiceState::Stopped);
}

Status ConfigService::save()
{
    LOG_DEBUG(m_name, "Saving to %s", CONFIG_FILE);

    auto file = LittleFS.open(CONFIG_FILE, "w");
    if (!file)
    {
        LOG_ERROR(m_name, "Failed to open for write");
        return Status::Error("File open failed");
    }

    const auto json{serializeToJson(m_config)};
    const auto written{file.print(json.c_str())};
    file.close();

    if (written != json.length())
    {
        LOG_ERROR(m_name, "Write incomplete: %u/%u", written, json.length());
        return Status::Error("Write failed");
    }

    LOG_INFO(m_name, "Saved (%u bytes)", written);
    m_dirty = false;
    return Status::Ok();
}

Status ConfigService::saveNow()
{
    return save();
}

Status ConfigService::load()
{
    LOG_DEBUG(m_name, "Loading from %s", CONFIG_FILE);

    if (!LittleFS.exists(CONFIG_FILE))
    {
        LOG_INFO(m_name, "File not found");
        return Status::Error("Not found");
    }

    auto file = LittleFS.open(CONFIG_FILE, "r");
    if (!file)
    {
        LOG_ERROR(m_name, "Failed to open for read");
        return Status::Error("Open failed");
    }

    const String json{file.readString()};
    file.close();

    if (json.isEmpty())
    {
        LOG_ERROR(m_name, "Empty file");
        return Status::Error("Empty file");
    }

    if (!parseJson(json.c_str()))
    {
        LOG_ERROR(m_name, "Parse failed");
        return Status::Error("Parse failed");
    }

    LOG_INFO(m_name, "Loaded");
    return Status::Ok();
}

Status ConfigService::reset()
{
    LOG_INFO(m_name, "Resetting to defaults");
    m_config.restoreDefaults();
    const auto status{save()}; // TODO: handle failure? first time handle it becouse we are resetting to defaults
    m_bus.publish(Event{EventType::ConfigChanged});
    return status;
}

Status ConfigService::updateFromJson(const char *json)
{
    if (!json || !parseJson(json))
    {
        return Status::Error("Invalid JSON");
    }

    m_dirty = true;
    m_bus.publish(Event{EventType::ConfigChanged});
    return Status::Ok();
}

void ConfigService::handleConfigMessage(const std::string &topic, const std::string &payload)
{
    JsonDocument doc;
    if (const auto error = deserializeJson(doc, payload); error)
    {
        LOG_ERROR(m_name, "JSON error: %s", error.c_str());
        return;
    }

    bool updated{false};
    const auto json{doc.as<JsonVariant>()};

    // Route based on topic suffix
    if (endsWith(topic, "/wifi"))
    {
        LOG_INFO(m_name, "Updating WiFi");
        updated = parseWifiConfig(json);
    }
    else if (endsWith(topic, "/mqtt"))
    {
        LOG_INFO(m_name, "Updating MQTT");
        updated = parseMqttConfig(json);
    }
    else if (endsWith(topic, "/device"))
    {
        LOG_INFO(m_name, "Updating Device");
        updated = parseDeviceConfig(json);
    }
    else if (endsWith(topic, "/pn532"))
    {
        LOG_INFO(m_name, "Updating PN532");
        updated = parsePn532Config(json);
    }
    else if (endsWith(topic, "/attendance"))
    {
        LOG_INFO(m_name, "Updating Attendance");
        updated = parseAttendanceConfig(json);
    }
    else if (endsWith(topic, "/feedback"))
    {
        LOG_INFO(m_name, "Updating Feedback");
        updated = parseFeedbackConfig(json);
    }
    else if (endsWith(topic, "/health"))
    {
        LOG_INFO(m_name, "Updating Health");
        updated = parseHealthConfig(json);
    }
    else if (endsWith(topic, "/ota"))
    {
        LOG_INFO(m_name, "Updating OTA");
        updated = parseOtaConfig(json);
    }
    else if (endsWith(topic, "/power"))
    {
        LOG_INFO(m_name, "Updating Power");
        updated = parsePowerConfig(json);
    }
    else
    {
        // Full config update
        LOG_INFO(m_name, "Full update");
        updated = parseJson(payload.c_str());
    }

    if (updated)
    {
        m_dirty = true;
        m_bus.publish(Event{EventType::ConfigChanged});
    }
}

bool ConfigService::parseJson(const char *json)
{
    JsonDocument doc;
    if (const auto error = deserializeJson(doc, json); error)
    {
        LOG_ERROR(m_name, "Parse error: %s", error.c_str());
        return false;
    }

    // Validate magic number and version
    if (doc["magic"].is<std::uint32_t>())
    {
        if (const auto magic{doc["magic"].as<std::uint32_t>()}; magic != Config::kMagicNumber)
        {
            LOG_ERROR(m_name, "Invalid magic number: 0x%08X (expected 0x%08X)", magic, Config::kMagicNumber);
            return false;
        }
    }
    else
    {
        LOG_WARN(m_name, "No magic number in config, may be old version");
        return false;
    }

    if (doc["version"].is<std::uint16_t>())
    {
        if (const auto version{doc["version"].as<std::uint16_t>()}; version != Config::kVersion)
        {
            LOG_ERROR(m_name, "Config version mismatch: %u (expected %u)", version, Config::kVersion);
            return false;
        }
    }
    else
    {
        LOG_WARN(m_name, "No version in config, may be old version");
        return false;
    }

    if (doc["wifi"].is<JsonObject>())
    {
        parseWifiConfig(doc["wifi"]);
    }
    if (doc["mqtt"].is<JsonObject>())
    {
        parseMqttConfig(doc["mqtt"]);
    }
    if (doc["device"].is<JsonObject>())
    {
        parseDeviceConfig(doc["device"]);
    }
    if (doc["pn532"].is<JsonObject>())
    {
        parsePn532Config(doc["pn532"]);
    }
    if (doc["attendance"].is<JsonObject>())
    {
        parseAttendanceConfig(doc["attendance"]);
    }
    if (doc["feedback"].is<JsonObject>())
    {
        parseFeedbackConfig(doc["feedback"]);
    }
    if (doc["health"].is<JsonObject>())
    {
        parseHealthConfig(doc["health"]);
    }
    if (doc["ota"].is<JsonObject>())
    {
        parseOtaConfig(doc["ota"]);
    }
    if (doc["power"].is<JsonObject>())
    {
        parsePowerConfig(doc["power"]);
    }

    return true;
}

bool ConfigService::parseWifiConfig(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &wifiConfig{m_config.wifi};

    PARSE_STR(json, "stationSsid", wifiConfig.stationSsid);
    PARSE_STR(json, "stationPassword", wifiConfig.stationPassword);
    PARSE_NUM(json, "stationConnectRetryDelayMs", wifiConfig.stationConnectRetryDelayMs);
    PARSE_NUM(json, "stationConnectionTimeoutMs", wifiConfig.stationConnectionTimeoutMs);
    PARSE_NUM(json, "stationMaxConnectionAttempts", wifiConfig.stationMaxConnectionAttempts);
    PARSE_BOOL(json, "stationPowerSaveEnabled", wifiConfig.stationPowerSaveEnabled);
    PARSE_STR(json, "accessPointSsidPrefix", wifiConfig.accessPointSsidPrefix);
    PARSE_STR(json, "accessPointPassword", wifiConfig.accessPointPassword);
    PARSE_NUM(json, "accessPointModeTimeoutMs", wifiConfig.accessPointModeTimeoutMs);

    return changed;
}

bool ConfigService::parseMqttConfig(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &mqttConfig{m_config.mqtt};

    PARSE_STR(json, "brokerAddress", mqttConfig.brokerAddress);
    PARSE_NUM(json, "port", mqttConfig.port);
    PARSE_STR(json, "username", mqttConfig.username);
    PARSE_STR(json, "password", mqttConfig.password);
    PARSE_STR(json, "baseTopic", mqttConfig.baseTopic);
    PARSE_NUM(json, "keepAliveIntervalSec", mqttConfig.keepAliveIntervalSec);
    PARSE_NUM(json, "reconnectMinIntervalMs", mqttConfig.reconnectMinIntervalMs);
    PARSE_NUM(json, "reconnectMaxIntervalMs", mqttConfig.reconnectMaxIntervalMs);

    return changed;
}

bool ConfigService::parseDeviceConfig(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &deviceConfig{m_config.device};

    PARSE_STR(json, "deviceId", deviceConfig.deviceId);
    PARSE_STR(json, "locationId", deviceConfig.locationId);

    return changed;
}

bool ConfigService::parsePn532Config(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &pn532Config{m_config.pn532};

    PARSE_NUM(json, "spiSckPin", pn532Config.spiSckPin);
    PARSE_NUM(json, "spiMisoPin", pn532Config.spiMisoPin);
    PARSE_NUM(json, "spiMosiPin", pn532Config.spiMosiPin);
    PARSE_NUM(json, "spiCsPin", pn532Config.spiCsPin);
    PARSE_NUM(json, "irqPin", pn532Config.irqPin);
    PARSE_NUM(json, "resetPin", pn532Config.resetPin);
    PARSE_NUM(json, "pollIntervalMs", pn532Config.pollIntervalMs);
    PARSE_NUM(json, "readTimeoutMs", pn532Config.readTimeoutMs);
    PARSE_NUM(json, "maxConsecutiveErrors", pn532Config.maxConsecutiveErrors);
    PARSE_NUM(json, "recoveryDelayMs", pn532Config.recoveryDelayMs);

    return changed;
}

bool ConfigService::parseAttendanceConfig(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &attendanceConfig{m_config.attendance};

    PARSE_NUM(json, "debounceIntervalMs", attendanceConfig.debounceIntervalMs);
    PARSE_NUM(json, "batchMaxSize", attendanceConfig.batchMaxSize);
    PARSE_NUM(json, "batchFlushIntervalMs", attendanceConfig.batchFlushIntervalMs);
    PARSE_NUM(json, "offlineBufferSize", attendanceConfig.offlineBufferSize);
    PARSE_NUM(json, "offlineBufferFlushIntervalMs", attendanceConfig.offlineBufferFlushIntervalMs);
    PARSE_BOOL(json, "batchingEnabled", attendanceConfig.batchingEnabled);

    // Parse enum separately, with validation. offlineQueuePolicy is uint8_t in JSON so 0 - DropOldest, 1 - DropNewest, 2 - DropAll
    if (json["offlineQueuePolicy"].is<uint8_t>())
    {
        if (const auto policy{json["offlineQueuePolicy"].as<uint8_t>()}; policy <= static_cast<uint8_t>(AttendanceConfig::OfflineQueuePolicy::DropAll))
        {
            attendanceConfig.offlineQueuePolicy = static_cast<AttendanceConfig::OfflineQueuePolicy>(policy);
            changed = true;
        }
    }

    return changed;
}

bool ConfigService::parseFeedbackConfig(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &feedbackConfig{m_config.feedback};

    PARSE_BOOL(json, "enabled", feedbackConfig.enabled);
    PARSE_BOOL(json, "ledEnabled", feedbackConfig.ledEnabled);
    PARSE_NUM(json, "ledPin", feedbackConfig.ledPin);
    PARSE_BOOL(json, "buzzerEnabled", feedbackConfig.buzzerEnabled);
    PARSE_NUM(json, "buzzerPin", feedbackConfig.buzzerPin);
    PARSE_BOOL(json, "ledActiveHigh", feedbackConfig.ledActiveHigh);
    PARSE_NUM(json, "beepFrequencyHz", feedbackConfig.beepFrequencyHz);
    PARSE_NUM(json, "successBlinkDurationMs", feedbackConfig.successBlinkDurationMs);
    PARSE_NUM(json, "errorBlinkDurationMs", feedbackConfig.errorBlinkDurationMs);

    return changed;
}

bool ConfigService::parseHealthConfig(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &healthConfig{m_config.health};

    PARSE_NUM(json, "healthCheckIntervalMs", healthConfig.healthCheckIntervalMs);
    PARSE_NUM(json, "statusUpdateIntervalMs", healthConfig.statusUpdateIntervalMs);
    PARSE_BOOL(json, "enabled", healthConfig.enabled);
    PARSE_BOOL(json, "publishToMqtt", healthConfig.publishToMqtt);
    PARSE_BOOL(json, "publishToLog", healthConfig.publishToLog);

    return changed;
}

bool ConfigService::parseOtaConfig(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &otaConfig{m_config.ota};

    PARSE_BOOL(json, "enabled", otaConfig.enabled);
    PARSE_STR(json, "updateServerUrl", otaConfig.updateServerUrl);
    PARSE_STR(json, "username", otaConfig.username);
    PARSE_STR(json, "password", otaConfig.password);

    return changed;
}

bool ConfigService::parsePowerConfig(const JsonVariant &json)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    bool changed{false};
    auto &powerConfig{m_config.power};

    PARSE_NUM(json, "sleepIntervalMs", powerConfig.sleepIntervalMs);
    PARSE_NUM(json, "maxDeepSleepMs", powerConfig.maxDeepSleepMs);
    PARSE_NUM(json, "lightSleepDurationMs", powerConfig.lightSleepDurationMs);
    PARSE_NUM(json, "idleTimeoutMs", powerConfig.idleTimeoutMs);
    PARSE_BOOL(json, "enableTimerWakeup", powerConfig.enableTimerWakeup);
    PARSE_BOOL(json, "enableNfcWakeup", powerConfig.enableNfcWakeup);
    PARSE_NUM(json, "nfcWakeupPin", powerConfig.nfcWakeupPin);
    PARSE_BOOL(json, "autoSleepEnabled", powerConfig.autoSleepEnabled);
    PARSE_BOOL(json, "disableWiFiDuringSleep", powerConfig.disableWiFiDuringSleep);
    PARSE_BOOL(json, "pn532SleepBetweenScans", powerConfig.pn532SleepBetweenScans);
    PARSE_BOOL(json, "smartSleepEnabled", powerConfig.smartSleepEnabled);
    PARSE_BOOL(json, "modemSleepOnMqttDisconnect", powerConfig.modemSleepOnMqttDisconnect);
    PARSE_NUM(json, "modemSleepDurationMs", powerConfig.modemSleepDurationMs);
    PARSE_NUM(json, "smartSleepShortThresholdMs", powerConfig.smartSleepShortThresholdMs);
    PARSE_NUM(json, "smartSleepMediumThresholdMs", powerConfig.smartSleepMediumThresholdMs);
    PARSE_NUM(json, "activityTypeMask", powerConfig.activityTypeMask);

    return changed;
}
} // namespace isic

// Cleanup macros
#undef PARSE_STR
#undef PARSE_NUM
#undef PARSE_BOOL
