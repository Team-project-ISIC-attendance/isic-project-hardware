#include "services/ConfigService.hpp"

#include "common/Logger.hpp"

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <utility>

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

template<typename Type>
bool parseNumber(const JsonVariant &json, const char *key, Type &target)
{
    if (json[key].is<Type>())
    {
        target = json[key].as<Type>();
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

bool parseString(const JsonVariant &json, const char *key, std::string &target)
{
    if (json[key].is<const char *>())
    {
        target = json[key].as<const char *>();
        return true;
    }

    return false;
}


void serializeWifiConfig(const JsonObject &wifi, const WiFiConfig &wifiConfig)
{
    wifi["stationSsid"] = wifiConfig.stationSsid;
    wifi["stationPassword"] = wifiConfig.stationPassword;
    wifi["stationConnectRetryDelayMs"] = wifiConfig.stationConnectRetryDelayMs;
    wifi["stationConnectionTimeoutMs"] = wifiConfig.stationConnectionTimeoutMs;
    wifi["stationFastReconnectIntervalMs"] = wifiConfig.stationFastReconnectIntervalMs;
    wifi["stationSlowReconnectIntervalMs"] = wifiConfig.stationSlowReconnectIntervalMs;
    wifi["stationMaxFastConnectionAttempts"] = wifiConfig.stationMaxFastConnectionAttempts;
    wifi["stationPowerSaveEnabled"] = wifiConfig.stationPowerSaveEnabled;
    wifi["stationHasEverConnected"] = wifiConfig.stationHasEverConnected;
    wifi["accessPointSsidPrefix"] = wifiConfig.accessPointSsidPrefix;
    wifi["accessPointPassword"] = wifiConfig.accessPointPassword;
    wifi["accessPointModeTimeoutMs"] = wifiConfig.accessPointModeTimeoutMs;
}

void serializeMqttConfig(const JsonObject &mqtt, const MqttConfig &mqttConfig)
{
    mqtt["brokerAddress"] = mqttConfig.brokerAddress;
    mqtt["port"] = mqttConfig.port;
    mqtt["username"] = mqttConfig.username;
    mqtt["password"] = mqttConfig.password;
    mqtt["baseTopic"] = mqttConfig.baseTopic;
    mqtt["keepAliveIntervalSec"] = mqttConfig.keepAliveIntervalSec;
    mqtt["reconnectMinIntervalMs"] = mqttConfig.reconnectMinIntervalMs;
    mqtt["reconnectMaxIntervalMs"] = mqttConfig.reconnectMaxIntervalMs;
}

void serializeDeviceConfig(const JsonObject &device, const DeviceConfig &deviceConfig)
{
    device["deviceId"] = deviceConfig.deviceId;
    device["locationId"] = deviceConfig.locationId;
}

void serializePn532Config(const JsonObject &pn532, const Pn532Config &pn532Config)
{
    pn532["spiSckPin"] = pn532Config.spiSckPin;
    pn532["spiMisoPin"] = pn532Config.spiMisoPin;
    pn532["spiMosiPin"] = pn532Config.spiMosiPin;
    pn532["spiCsPin"] = pn532Config.spiCsPin;
    pn532["irqPin"] = pn532Config.irqPin;
    pn532["pollIntervalMs"] = pn532Config.pollIntervalMs;
    pn532["readTimeoutMs"] = pn532Config.readTimeoutMs;
    pn532["maxConsecutiveErrors"] = pn532Config.maxConsecutiveErrors;
    pn532["recoveryDelayMs"] = pn532Config.recoveryDelayMs;
}

void serializeAttendanceConfig(const JsonObject &attendance, const AttendanceConfig &attendanceConfig)
{
    attendance["debounceIntervalMs"] = attendanceConfig.debounceIntervalMs;
    attendance["batchMaxSize"] = attendanceConfig.batchMaxSize;
    attendance["batchFlushIntervalMs"] = attendanceConfig.batchFlushIntervalMs;
    attendance["offlineBufferSize"] = attendanceConfig.offlineBufferSize;
    attendance["offlineBufferFlushIntervalMs"] = attendanceConfig.offlineBufferFlushIntervalMs;
    attendance["batchingEnabled"] = attendanceConfig.batchingEnabled;
    attendance["offlineQueuePolicy"] = static_cast<uint8_t>(attendanceConfig.offlineQueuePolicy);
}

void serializeFeedbackConfig(const JsonObject &feedback, const FeedbackConfig &feedbackConfig)
{
    feedback["enabled"] = feedbackConfig.enabled;
    feedback["ledEnabled"] = feedbackConfig.ledEnabled;
    feedback["ledPin"] = feedbackConfig.ledPin;
    feedback["buzzerEnabled"] = feedbackConfig.buzzerEnabled;
    feedback["buzzerPin"] = feedbackConfig.buzzerPin;
    feedback["ledActiveHigh"] = feedbackConfig.ledActiveHigh;
    feedback["beepFrequencyHz"] = feedbackConfig.beepFrequencyHz;
    feedback["successBlinkDurationMs"] = feedbackConfig.successBlinkDurationMs;
    feedback["errorBlinkDurationMs"] = feedbackConfig.errorBlinkDurationMs;
}

void serializeHealthConfig(const JsonObject &health, const HealthConfig &healthConfig)
{
    health["healthCheckIntervalMs"] = healthConfig.healthCheckIntervalMs;
    health["statusUpdateIntervalMs"] = healthConfig.statusUpdateIntervalMs;
    health["metricsPublishIntervalMs"] = healthConfig.metricsPublishIntervalMs;
    health["publishToMqtt"] = healthConfig.publishToMqtt;
}

void serializeOtaConfig(const JsonObject &ota, const OtaConfig &otaConfig)
{
    ota["enabled"] = otaConfig.enabled;
    ota["serverUrl"] = otaConfig.serverUrl;
    ota["username"] = otaConfig.username;
    ota["password"] = otaConfig.password;
    ota["timeoutMs"] = otaConfig.timeoutMs;
    ota["checkOnConnect"] = otaConfig.checkOnConnect;
}

void serializePowerConfig(const JsonObject &power, const PowerConfig &powerConfig)
{
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
}

std::string serializeToJson(const Config &config)
{
    JsonDocument doc;

    // Version and magic for validation on load
    doc["magic"] = config.magic;
    doc["version"] = config.version;

    // WiFi
    const auto wifi{doc["wifi"].to<JsonObject>()};
    serializeWifiConfig(wifi, config.wifi);

    // MQTT
    const auto mqtt{doc["mqtt"].to<JsonObject>()};
    serializeMqttConfig(mqtt, config.mqtt);

    // Device
    const auto device{doc["device"].to<JsonObject>()};
    serializeDeviceConfig(device, config.device);

    // PN532
    const auto pn532{doc["pn532"].to<JsonObject>()};
    serializePn532Config(pn532, config.pn532);

    // Attendance
    const auto attendance{doc["attendance"].to<JsonObject>()};
    serializeAttendanceConfig(attendance, config.attendance);

    // Feedback
    const auto feedback{doc["feedback"].to<JsonObject>()};
    serializeFeedbackConfig(feedback, config.feedback);

    // Health
    const auto health{doc["health"].to<JsonObject>()};
    serializeHealthConfig(health, config.health);

    // OTA
    const auto ota{doc["ota"].to<JsonObject>()};
    serializeOtaConfig(ota, config.ota);

    // Power
    const auto power{doc["power"].to<JsonObject>()};
    serializePowerConfig(power, config.power);

    std::string result;
    result.reserve(measureJson(doc) + 1);
    serializeJson(doc, result);
    return result;
}


// Macros for parsing fields and setting 'changed' flag if updated, just to reduce code duplication
#define PARSE_STR(json, key, field)        \
    do                                     \
    {                                      \
        if (parseString(json, key, field)) \
            changed = true;                \
    }                                      \
    while (0)

#define PARSE_NUM(json, key, field)        \
    do                                     \
    {                                      \
        if (parseNumber(json, key, field)) \
            changed = true;                \
    }                                      \
    while (0)

#define PARSE_BOOL(json, key, field)     \
    do                                   \
    {                                    \
        if (parseBool(json, key, field)) \
            changed = true;              \
    }                                    \
    while (0)

bool deserializeWifiConfig(const JsonVariant &json, WiFiConfig &wifiConfig)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

    PARSE_STR(json, "stationSsid", wifiConfig.stationSsid);
    PARSE_STR(json, "stationPassword", wifiConfig.stationPassword);
    PARSE_NUM(json, "stationConnectRetryDelayMs", wifiConfig.stationConnectRetryDelayMs);
    PARSE_NUM(json, "stationConnectionTimeoutMs", wifiConfig.stationConnectionTimeoutMs);
    PARSE_NUM(json, "StationFastReconnectIntervalMs", wifiConfig.stationFastReconnectIntervalMs);
    PARSE_NUM(json, "stationSlowReconnectIntervalMs", wifiConfig.stationSlowReconnectIntervalMs);
    PARSE_NUM(json, "stationMaxFastConnectionAttempts", wifiConfig.stationMaxFastConnectionAttempts);
    PARSE_BOOL(json, "stationPowerSaveEnabled", wifiConfig.stationPowerSaveEnabled);
    PARSE_BOOL(json, "stationHasEverConnected", wifiConfig.stationHasEverConnected);
    PARSE_STR(json, "accessPointSsidPrefix", wifiConfig.accessPointSsidPrefix);
    PARSE_STR(json, "accessPointPassword", wifiConfig.accessPointPassword);
    PARSE_NUM(json, "accessPointModeTimeoutMs", wifiConfig.accessPointModeTimeoutMs);

    return changed;
}

bool deserializeMqttConfig(const JsonVariant &json, MqttConfig &mqttConfig)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

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

bool deserializeDeviceConfig(const JsonVariant &json, DeviceConfig &deviceConfig)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

    PARSE_STR(json, "deviceId", deviceConfig.deviceId);
    PARSE_STR(json, "locationId", deviceConfig.locationId);

    return changed;
}

bool deserializePn532Config(const JsonVariant &json, Pn532Config &pn532Config)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

    PARSE_NUM(json, "spiSckPin", pn532Config.spiSckPin);
    PARSE_NUM(json, "spiMisoPin", pn532Config.spiMisoPin);
    PARSE_NUM(json, "spiMosiPin", pn532Config.spiMosiPin);
    PARSE_NUM(json, "spiCsPin", pn532Config.spiCsPin);
    PARSE_NUM(json, "irqPin", pn532Config.irqPin);
    PARSE_NUM(json, "pollIntervalMs", pn532Config.pollIntervalMs);
    PARSE_NUM(json, "readTimeoutMs", pn532Config.readTimeoutMs);
    PARSE_NUM(json, "maxConsecutiveErrors", pn532Config.maxConsecutiveErrors);
    PARSE_NUM(json, "recoveryDelayMs", pn532Config.recoveryDelayMs);

    return changed;
}

bool deserializeAttendanceConfig(const JsonVariant &json, AttendanceConfig &attendanceConfig)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

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

bool deserializeFeedbackConfig(const JsonVariant &json, FeedbackConfig &feedbackConfig)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

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

bool deserializeHealthConfig(const JsonVariant &json, HealthConfig &healthConfig)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

    PARSE_NUM(json, "healthCheckIntervalMs", healthConfig.healthCheckIntervalMs);
    PARSE_NUM(json, "statusUpdateIntervalMs", healthConfig.statusUpdateIntervalMs);
    PARSE_NUM(json, "metricsPublishIntervalMs", healthConfig.metricsPublishIntervalMs);
    PARSE_BOOL(json, "publishToMqtt", healthConfig.publishToMqtt);

    return changed;
}

bool deserializeOtaConfig(const JsonVariant &json, OtaConfig &otaConfig)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

    PARSE_BOOL(json, "enabled", otaConfig.enabled);
    PARSE_STR(json, "serverUrl", otaConfig.serverUrl);
    PARSE_STR(json, "username", otaConfig.username);
    PARSE_STR(json, "password", otaConfig.password);
    PARSE_NUM(json, "timeoutMs", otaConfig.timeoutMs);
    PARSE_BOOL(json, "checkOnConnect", otaConfig.checkOnConnect);

    return changed;
}

bool deserializePowerConfig(const JsonVariant &json, PowerConfig &powerConfig)
{
    if (!json.is<JsonObject>())
    {
        return false;
    }

    auto changed{false};

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

bool deserializeJson(const char *serviceName, const char *json, Config &config)
{
    JsonDocument doc;

    if (const auto error = deserializeJson(doc, json); error)
    {
        LOG_ERROR(serviceName, "Parse error: %s", error.c_str());
        return false;
    }

    // Validate magic number and version
    if (doc["magic"].is<std::uint32_t>())
    {
        if (const auto magic{doc["magic"].as<std::uint32_t>()}; magic != Config::kMagicNumber)
        {
            LOG_ERROR(serviceName, "Invalid magic number: 0x%08X (expected 0x%08X)", magic, Config::kMagicNumber);
            return false;
        }
    }
    else
    {
        LOG_WARN(serviceName, "No magic number in config, may be old version");
        return false;
    }

    if (doc["version"].is<std::uint16_t>())
    {
        if (const auto version{doc["version"].as<std::uint16_t>()}; version != Config::kVersion)
        {
            LOG_ERROR(serviceName, "Config version mismatch: %u (expected %u)", version, Config::kVersion);
            return false;
        }
    }
    else
    {
        LOG_WARN(serviceName, "No version in config, may be old version");
        return false;
    }

    auto changed{false};

    if (doc["wifi"].is<JsonObject>())
    {
        changed |= deserializeWifiConfig(doc["wifi"], config.wifi);
    }
    if (doc["mqtt"].is<JsonObject>())
    {
        changed |= deserializeMqttConfig(doc["mqtt"], config.mqtt);
    }
    if (doc["device"].is<JsonObject>())
    {
        changed |= deserializeDeviceConfig(doc["device"], config.device);
    }
    if (doc["pn532"].is<JsonObject>())
    {
        changed |= deserializePn532Config(doc["pn532"], config.pn532);
    }
    if (doc["attendance"].is<JsonObject>())
    {
        changed |= deserializeAttendanceConfig(doc["attendance"], config.attendance);
    }
    if (doc["feedback"].is<JsonObject>())
    {
        changed |= deserializeFeedbackConfig(doc["feedback"], config.feedback);
    }
    if (doc["health"].is<JsonObject>())
    {
        changed |= deserializeHealthConfig(doc["health"], config.health);
    }
    if (doc["ota"].is<JsonObject>())
    {
        changed |= deserializeOtaConfig(doc["ota"], config.ota);
    }
    if (doc["power"].is<JsonObject>())
    {
        changed |= deserializePowerConfig(doc["power"], config.power);
    }

    return changed;
}


// Cleanup macros after use
#undef PARSE_STR
#undef PARSE_NUM
#undef PARSE_BOOL

constexpr auto *kConfigSetTopicSuffix{"config/set"};
constexpr auto *kConfigGetTopicSuffix{"config/get"};
constexpr auto *kConfigSetTopic{"config/set/#"};
constexpr auto *kConfigGetTopic{"config/get/#"};
} // namespace

ConfigService::ConfigService(EventBus &bus)
    : ServiceBase("ConfigService")
    , m_bus(bus)
{

    m_eventConnections.reserve(2);
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttConnected, [this](const Event &) {
        m_bus.publish({EventType::MqttSubscribeRequest, MqttEvent{.topic = kConfigSetTopic}});
        m_bus.publish({EventType::MqttSubscribeRequest, MqttEvent{.topic = kConfigGetTopic}});
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttMessage, [this](const Event &event) {
        if (const auto *mqtt = event.get<MqttEvent>())
        {
            if (mqtt->topic.find(kConfigSetTopicSuffix) != std::string::npos)
            {
                handleSetConfigMessage(mqtt->topic, mqtt->payload);
            }
            else if (mqtt->topic.find(kConfigGetTopicSuffix) != std::string::npos)
            {
                handleGetConfigMessage(mqtt->topic);
            }
        }
    }));
}

ConfigService::~ConfigService()
{
    if (m_dirty)
    {
        (void) saveNow(); // TODO: handle failure? i think in destructor we can't do much about it so just ignore
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

        if (LittleFS.exists(kConfigFile))
        {
            LOG_INFO(m_name, "Removing old config file");
            LittleFS.remove(kConfigFile);
        }

        (void) saveNow(); // TODO: handle failure?
    }

    setState(ServiceState::Running);
    LOG_INFO(m_name, "Ready, device=%s, fw=%s", m_config.device.deviceId.c_str(), DeviceConfig::Constants::kFirmwareVersion);
    return Status::Ok();
}

void ConfigService::loop()
{
    if (m_dirty)
    {
        (void) saveNow(); // TODO: handle failure?
        m_dirty = false;
    }
}

void ConfigService::end()
{
    if (m_dirty)
    {
        (void) saveNow(); // TODO: handle failure?
    }

    m_eventConnections.clear();
    setState(ServiceState::Stopped);
}

Status ConfigService::save()
{
    m_dirty = true;
    return Status::Ok();
}

Status ConfigService::saveNow()
{
    LOG_DEBUG(m_name, "Saving to %s", kConfigFile);

    auto file = LittleFS.open(kConfigFile, "w");
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

Status ConfigService::load()
{
    LOG_DEBUG(m_name, "Loading from %s", kConfigFile);

    if (!LittleFS.exists(kConfigFile))
    {
        LOG_INFO(m_name, "File not found");
        return Status::Error("Not found");
    }

    auto file{LittleFS.open(kConfigFile, "r")};
    if (!file)
    {
        LOG_ERROR(m_name, "Failed to open for read");
        return Status::Error("Open failed");
    }

    const auto json{file.readString()}; // returns Arduino String, but we use c++ types only, use const char*
    file.close();

    if (json.isEmpty())
    {
        LOG_ERROR(m_name, "Empty file");
        return Status::Error("Empty file");
    }

    if (!deserializeJson(m_name, json.c_str(), m_config))
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

    const auto status{saveNow()}; // TODO: handle failure?
    m_bus.publish(EventType::ConfigChanged);

    return status;
}

void ConfigService::handleSetConfigMessage(const std::string &topic, const std::string &payload)
{
    JsonDocument doc;

    if (const auto error = deserializeJson(doc, payload); error)
    {
        LOG_ERROR(m_name, "JSON error: %s", error.c_str());
        return;
    }

    auto updated{false};
    const auto json{doc.as<JsonVariant>()};

    if (endsWith(topic, "/wifi"))
    {
        LOG_INFO(m_name, "Updating WiFi");
        updated = deserializeWifiConfig(json, m_config.wifi);
    }
    else if (endsWith(topic, "/mqtt"))
    {
        LOG_INFO(m_name, "Updating MQTT");
        updated = deserializeMqttConfig(json, m_config.mqtt);
    }
    else if (endsWith(topic, "/device"))
    {
        LOG_INFO(m_name, "Updating Device");
        updated = deserializeDeviceConfig(json, m_config.device);
    }
    else if (endsWith(topic, "/pn532"))
    {
        LOG_INFO(m_name, "Updating PN532");
        updated = deserializePn532Config(json, m_config.pn532);
    }
    else if (endsWith(topic, "/attendance"))
    {
        LOG_INFO(m_name, "Updating Attendance");
        updated = deserializeAttendanceConfig(json, m_config.attendance);
    }
    else if (endsWith(topic, "/feedback"))
    {
        LOG_INFO(m_name, "Updating Feedback");
        updated = deserializeFeedbackConfig(json, m_config.feedback);
    }
    else if (endsWith(topic, "/health"))
    {
        LOG_INFO(m_name, "Updating Health");
        updated = deserializeHealthConfig(json, m_config.health);
    }
    else if (endsWith(topic, "/ota"))
    {
        LOG_INFO(m_name, "Updating OTA");
        updated = deserializeOtaConfig(json, m_config.ota);
    }
    else if (endsWith(topic, "/power"))
    {
        LOG_INFO(m_name, "Updating Power");
        updated = deserializePowerConfig(json, m_config.power);
    }
    else
    {
        LOG_INFO(m_name, "Full update");
        updated = deserializeJson(m_name, payload.c_str(), m_config);
    }

    if (updated)
    {
        m_dirty = true;
        m_bus.publish(EventType::ConfigChanged);
    }
}

void ConfigService::handleGetConfigMessage(const std::string &topic)
{
    JsonDocument doc;
    std::string payload{};
    std::string responseTopic{"config"};

    if (endsWith(topic, "/wifi"))
    {
        LOG_INFO(m_name, "Getting WiFi config");
        const auto obj{doc.to<JsonObject>()};
        serializeWifiConfig(obj, m_config.wifi);
        responseTopic = "config/wifi";
    }
    else if (endsWith(topic, "/mqtt"))
    {
        LOG_INFO(m_name, "Getting MQTT config");
        const auto obj{doc.to<JsonObject>()};
        serializeMqttConfig(obj, m_config.mqtt);
        responseTopic = "config/mqtt";
    }
    else if (endsWith(topic, "/device"))
    {
        LOG_INFO(m_name, "Getting Device config");
        const auto obj{doc.to<JsonObject>()};
        serializeDeviceConfig(obj, m_config.device);
        responseTopic = "config/device";
    }
    else if (endsWith(topic, "/pn532"))
    {
        LOG_INFO(m_name, "Getting PN532 config");
        const auto obj{doc.to<JsonObject>()};
        serializePn532Config(obj, m_config.pn532);
        responseTopic = "config/pn532";
    }
    else if (endsWith(topic, "/attendance"))
    {
        LOG_INFO(m_name, "Getting Attendance config");
        const auto obj{doc.to<JsonObject>()};
        serializeAttendanceConfig(obj, m_config.attendance);
        responseTopic = "config/attendance";
    }
    else if (endsWith(topic, "/feedback"))
    {
        LOG_INFO(m_name, "Getting Feedback config");
        const auto obj{doc.to<JsonObject>()};
        serializeFeedbackConfig(obj, m_config.feedback);
        responseTopic = "config/feedback";
    }
    else if (endsWith(topic, "/health"))
    {
        LOG_INFO(m_name, "Getting Health config");
        const auto obj{doc.to<JsonObject>()};
        serializeHealthConfig(obj, m_config.health);
        responseTopic = "config/health";
    }
    else if (endsWith(topic, "/ota"))
    {
        LOG_INFO(m_name, "Getting OTA config");
        const auto obj{doc.to<JsonObject>()};
        serializeOtaConfig(obj, m_config.ota);
        responseTopic = "config/ota";
    }
    else if (endsWith(topic, "/power"))
    {
        LOG_INFO(m_name, "Getting Power config");
        const auto obj{doc.to<JsonObject>()};
        serializePowerConfig(obj, m_config.power);
        responseTopic = "config/power";
    }
    else
    {
        LOG_INFO(m_name, "Getting full config");
        payload = serializeToJson(m_config);
    }

    if (payload.empty())
    {
        const auto jsonSize = measureJson(doc);
        payload.reserve(jsonSize + 1);
        serializeJson(doc, payload);
    }

    m_bus.publish({EventType::MqttPublishRequest, MqttEvent{.topic = std::move(responseTopic), .payload = std::move(payload)}});
}
} // namespace isic
