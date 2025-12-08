#include "services/ConfigService.hpp"

#include "core/Logger.hpp"

#include <memory>
#include <ArduinoJson.h>

namespace isic {
    namespace {
        constexpr auto *CONFIG_SERVICE_TAG{"ConfigService"};
        constexpr auto *PREF_NAMESPACE{"isic"};
        constexpr auto *PREF_KEY_CONFIG{"config"};
    }

    ConfigService::ConfigService(EventBus &bus) : m_bus(bus) {
        (void) m_bus.subscribe(this); // TODO: handle unsubscription on destruction if needed
    }

    Status ConfigService::begin() {
        if (!m_prefs.begin(PREF_NAMESPACE, false)) {
            LOG_ERROR(CONFIG_SERVICE_TAG, "Failed to open NVS namespace");
            m_config = AppConfig::makeDefault();
            return Status::Error(ErrorCode::StorageError, "NVS open failed");
        }

        if (const auto status = load(); !status.ok()) {
            LOG_WARNING(CONFIG_SERVICE_TAG, "Load failed, using defaults");
            m_config = AppConfig::makeDefault();
            (void) save(); // TODO: Attempt to save defaults
        }

        notifyUpdated();
        return Status::OK();
    }

    Status ConfigService::load() {
        const auto json{m_prefs.getString(PREF_KEY_CONFIG, "")};

        if (json.isEmpty()) {
            LOG_INFO(CONFIG_SERVICE_TAG, "No config stored in NVS, using defaults");
            m_config = AppConfig::makeDefault();
            return Status::OK();
        }

        JsonDocument doc{};
        if (const auto err = deserializeJson(doc, json); err) {
            LOG_ERROR(CONFIG_SERVICE_TAG, "JSON parse error: %s", err.c_str());
            return Status::Error(ErrorCode::JsonError, "Deserialize failed");
        }

        auto cfg = AppConfig::makeDefault();
        const auto root = doc.as<JsonObject>();

        // WiFi
        if (const auto wifi = root["wifi"].as<JsonObjectConst>()) {
            cfg.wifi.ssid = wifi["ssid"] | cfg.wifi.ssid;
            cfg.wifi.password = wifi["password"] | cfg.wifi.password;
            cfg.wifi.connectTimeoutMs = wifi["connectTimeoutMs"] | cfg.wifi.connectTimeoutMs;
            cfg.wifi.maxRetries = wifi["maxRetries"] | cfg.wifi.maxRetries;
        }

        // MQTT
        if (const auto mqtt = root["mqtt"].as<JsonObjectConst>()) {
            cfg.mqtt.broker = mqtt["broker"] | cfg.mqtt.broker;
            cfg.mqtt.port = mqtt["port"] | cfg.mqtt.port;
            cfg.mqtt.username = mqtt["username"] | cfg.mqtt.username;
            cfg.mqtt.password = mqtt["password"] | cfg.mqtt.password;
            cfg.mqtt.baseTopic = mqtt["baseTopic"] | cfg.mqtt.baseTopic;
            cfg.mqtt.tls = mqtt["tls"] | cfg.mqtt.tls;
            cfg.mqtt.keepAliveSeconds = mqtt["keepAliveSeconds"] | cfg.mqtt.keepAliveSeconds;
            cfg.mqtt.reconnectBackoffMinMs = mqtt["reconnectBackoffMinMs"] | cfg.mqtt.reconnectBackoffMinMs;
            cfg.mqtt.reconnectBackoffMaxMs = mqtt["reconnectBackoffMaxMs"] | cfg.mqtt.reconnectBackoffMaxMs;
            cfg.mqtt.outboundQueueSize = mqtt["outboundQueueSize"] | cfg.mqtt.outboundQueueSize;
        }

        // Device
        if (const auto device = root["device"].as<JsonObjectConst>()) {
            cfg.device.deviceId = device["deviceId"] | cfg.device.deviceId;
            cfg.device.locationId = device["locationId"] | cfg.device.locationId;
            cfg.device.firmwareVersion = device["firmwareVersion"] | cfg.device.firmwareVersion;
        }

        // Attendance
        if (const auto att = root["attendance"].as<JsonObjectConst>()) {
            cfg.attendance.debounceMs = att["debounceMs"] | cfg.attendance.debounceMs;
            cfg.attendance.offlineBufferSize = att["offlineBufferSize"] | cfg.attendance.offlineBufferSize;
            cfg.attendance.eventQueueSize = att["eventQueueSize"] | cfg.attendance.eventQueueSize;
            cfg.attendance.queueHighWatermark = att["queueHighWatermark"] | cfg.attendance.queueHighWatermark;
        }

        // OTA
        if (const auto ota = root["ota"].as<JsonObjectConst>()) {
            cfg.ota.autoCheck = ota["autoCheck"] | cfg.ota.autoCheck;
            cfg.ota.checkIntervalMs = ota["checkIntervalMs"] | cfg.ota.checkIntervalMs;
            cfg.ota.updateServerUrl = ota["updateServerUrl"] | cfg.ota.updateServerUrl;
            cfg.ota.requireHttps = ota["requireHttps"] | cfg.ota.requireHttps;
        }

        // PN532
        if (const auto pn532 = root["pn532"].as<JsonObjectConst>()) {
            cfg.pn532.irqPin = pn532["irqPin"] | cfg.pn532.irqPin;
            cfg.pn532.resetPin = pn532["resetPin"] | cfg.pn532.resetPin;
            cfg.pn532.pollIntervalMs = pn532["pollIntervalMs"] | cfg.pn532.pollIntervalMs;
            cfg.pn532.cardReadTimeoutMs = pn532["cardReadTimeoutMs"] | cfg.pn532.cardReadTimeoutMs;
            cfg.pn532.healthCheckIntervalMs = pn532["healthCheckIntervalMs"] | cfg.pn532.healthCheckIntervalMs;
            cfg.pn532.communicationTimeoutMs = pn532["communicationTimeoutMs"] | cfg.pn532.communicationTimeoutMs;
            cfg.pn532.maxConsecutiveErrors = pn532["maxConsecutiveErrors"] | cfg.pn532.maxConsecutiveErrors;
            cfg.pn532.recoveryDelayMs = pn532["recoveryDelayMs"] | cfg.pn532.recoveryDelayMs;
            cfg.pn532.maxRecoveryAttempts = pn532["maxRecoveryAttempts"] | cfg.pn532.maxRecoveryAttempts;
            cfg.pn532.wakeOnCardEnabled = pn532["wakeOnCardEnabled"] | cfg.pn532.wakeOnCardEnabled;
        }

        // Power
        if (const auto power = root["power"].as<JsonObjectConst>()) {
            cfg.power.sleepEnabled = power["sleepEnabled"] | cfg.power.sleepEnabled;
            cfg.power.idleTimeoutMs = power["idleTimeoutMs"] | cfg.power.idleTimeoutMs;
            cfg.power.wakeCheckIntervalMs = power["wakeCheckIntervalMs"] | cfg.power.wakeCheckIntervalMs;
            cfg.power.wakeSourcePn532Enabled = power["wakeSourcePn532Enabled"] | cfg.power.wakeSourcePn532Enabled;
            cfg.power.wakeSourceTimerEnabled = power["wakeSourceTimerEnabled"] | cfg.power.wakeSourceTimerEnabled;
            cfg.power.timerWakeIntervalMs = power["timerWakeIntervalMs"] | cfg.power.timerWakeIntervalMs;
            cfg.power.wifiPowerSaveEnabled = power["wifiPowerSaveEnabled"] | cfg.power.wifiPowerSaveEnabled;
            cfg.power.cpuFrequencyMhz = power["cpuFrequencyMhz"] | cfg.power.cpuFrequencyMhz;

            if (power["sleepType"].is<const char *>()) {
                if (const auto sleepType = power["sleepType"].as<const char *>(); sleepType == "light") {
                    cfg.power.sleepType = PowerConfig::SleepType::Light;
                } else if (sleepType == "deep") {
                    cfg.power.sleepType = PowerConfig::SleepType::Deep;
                } else {
                    cfg.power.sleepType = PowerConfig::SleepType::None;
                }
            }
        }

        // Health
        if (const auto health = root["health"].as<JsonObjectConst>()) {
            cfg.health.checkIntervalMs = health["checkIntervalMs"] | cfg.health.checkIntervalMs;
            cfg.health.reportIntervalMs = health["reportIntervalMs"] | cfg.health.reportIntervalMs;
            cfg.health.publishToMqtt = health["publishToMqtt"] | cfg.health.publishToMqtt;
            cfg.health.logToSerial = health["logToSerial"] | cfg.health.logToSerial;
            cfg.health.mqttUnhealthyAfterMs = health["mqttUnhealthyAfterMs"] | cfg.health.mqttUnhealthyAfterMs;
            cfg.health.wifiUnhealthyAfterMs = health["wifiUnhealthyAfterMs"] | cfg.health.wifiUnhealthyAfterMs;
        }

        // Log
        if (const auto log = root["log"].as<JsonObjectConst>()) {
            cfg.log.includeTimestamps = log["includeTimestamps"] | cfg.log.includeTimestamps;
            cfg.log.colorOutput = log["colorOutput"] | cfg.log.colorOutput;

            if (log["serialLevel"].is<uint8_t>()) {
                cfg.log.serialLevel = static_cast<LogConfig::Level>(log["serialLevel"].as<uint8_t>());
            }
            if (log["mqttLevel"].is<uint8_t>()) {
                cfg.log.mqttLevel = static_cast<LogConfig::Level>(log["mqttLevel"].as<uint8_t>());
            }
        }

        if (!cfg.validate()) {
            LOG_WARNING(CONFIG_SERVICE_TAG, "Loaded config fails validation, using defaults");
            cfg = AppConfig::makeDefault();
        }

        m_config = cfg;
        LOG_INFO(CONFIG_SERVICE_TAG, "Config loaded from NVS");
        return Status::OK();
    }

    Status ConfigService::save() {
        JsonDocument doc{};
        const auto root{doc.to<JsonObject>()};

        // WiFi
        {
            const auto wifi{root["wifi"].to<JsonObject>()};
            wifi["ssid"] = m_config.wifi.ssid;
            wifi["password"] = m_config.wifi.password;
            wifi["connectTimeoutMs"] = m_config.wifi.connectTimeoutMs;
            wifi["maxRetries"] = m_config.wifi.maxRetries;
        }

        // MQTT
        {
            const auto mqtt{root["mqtt"].to<JsonObject>()};
            mqtt["broker"] = m_config.mqtt.broker;
            mqtt["port"] = m_config.mqtt.port;
            mqtt["username"] = m_config.mqtt.username;
            mqtt["password"] = m_config.mqtt.password;
            mqtt["baseTopic"] = m_config.mqtt.baseTopic;
            mqtt["tls"] = m_config.mqtt.tls;
            mqtt["keepAliveSeconds"] = m_config.mqtt.keepAliveSeconds;
            mqtt["reconnectBackoffMinMs"] = m_config.mqtt.reconnectBackoffMinMs;
            mqtt["reconnectBackoffMaxMs"] = m_config.mqtt.reconnectBackoffMaxMs;
            mqtt["outboundQueueSize"] = m_config.mqtt.outboundQueueSize;
        }

        // Device
        {
            const auto device{root["device"].to<JsonObject>()};
            device["deviceId"] = m_config.device.deviceId;
            device["locationId"] = m_config.device.locationId;
            device["firmwareVersion"] = m_config.device.firmwareVersion;
        }

        // Attendance
        {
            const auto att{root["attendance"].to<JsonObject>()};
            att["debounceMs"] = m_config.attendance.debounceMs;
            att["offlineBufferSize"] = m_config.attendance.offlineBufferSize;
            att["eventQueueSize"] = m_config.attendance.eventQueueSize;
            att["queueHighWatermark"] = m_config.attendance.queueHighWatermark;
        }

        // OTA
        {
            const auto ota{root["ota"].to<JsonObject>()};
            ota["autoCheck"] = m_config.ota.autoCheck;
            ota["checkIntervalMs"] = m_config.ota.checkIntervalMs;
            ota["updateServerUrl"] = m_config.ota.updateServerUrl;
            ota["requireHttps"] = m_config.ota.requireHttps;
        }

        // PN532
        {
            const auto pn532{root["pn532"].to<JsonObject>()};
            pn532["irqPin"] = m_config.pn532.irqPin;
            pn532["resetPin"] = m_config.pn532.resetPin;
            pn532["pollIntervalMs"] = m_config.pn532.pollIntervalMs;
            pn532["cardReadTimeoutMs"] = m_config.pn532.cardReadTimeoutMs;
            pn532["healthCheckIntervalMs"] = m_config.pn532.healthCheckIntervalMs;
            pn532["communicationTimeoutMs"] = m_config.pn532.communicationTimeoutMs;
            pn532["maxConsecutiveErrors"] = m_config.pn532.maxConsecutiveErrors;
            pn532["recoveryDelayMs"] = m_config.pn532.recoveryDelayMs;
            pn532["maxRecoveryAttempts"] = m_config.pn532.maxRecoveryAttempts;
            pn532["wakeOnCardEnabled"] = m_config.pn532.wakeOnCardEnabled;
        }

        // Power
        {
            const auto power{root["power"].to<JsonObject>()};
            power["sleepEnabled"] = m_config.power.sleepEnabled;
            power["idleTimeoutMs"] = m_config.power.idleTimeoutMs;
            power["wakeCheckIntervalMs"] = m_config.power.wakeCheckIntervalMs;
            power["wakeSourcePn532Enabled"] = m_config.power.wakeSourcePn532Enabled;
            power["wakeSourceTimerEnabled"] = m_config.power.wakeSourceTimerEnabled;
            power["timerWakeIntervalMs"] = m_config.power.timerWakeIntervalMs;
            power["wifiPowerSaveEnabled"] = m_config.power.wifiPowerSaveEnabled;
            power["cpuFrequencyMhz"] = m_config.power.cpuFrequencyMhz;

            const auto *sleepType{"none"};
            switch (m_config.power.sleepType) {
                case PowerConfig::SleepType::Light: {
                    sleepType = "light";
                    break;
                }
                case PowerConfig::SleepType::Deep: {
                    sleepType = "deep";
                    break;
                }
                default: {
                    break;
                }
            }
            power["sleepType"] = sleepType;
        }

        // Health
        {
            const auto health{root["health"].to<JsonObject>()};
            health["checkIntervalMs"] = m_config.health.checkIntervalMs;
            health["reportIntervalMs"] = m_config.health.reportIntervalMs;
            health["publishToMqtt"] = m_config.health.publishToMqtt;
            health["logToSerial"] = m_config.health.logToSerial;
            health["mqttUnhealthyAfterMs"] = m_config.health.mqttUnhealthyAfterMs;
            health["wifiUnhealthyAfterMs"] = m_config.health.wifiUnhealthyAfterMs;
        }

        // Log
        {
            const auto log{root["log"].to<JsonObject>()};
            log["serialLevel"] = static_cast<uint8_t>(m_config.log.serialLevel);
            log["mqttLevel"] = static_cast<uint8_t>(m_config.log.mqttLevel);
            log["includeTimestamps"] = m_config.log.includeTimestamps;
            log["colorOutput"] = m_config.log.colorOutput;
        }

        String json{};
        serializeJson(doc, json);

        if (!m_prefs.putString(PREF_KEY_CONFIG, json)) {
            LOG_ERROR(CONFIG_SERVICE_TAG, "Failed to save config to NVS");
            return Status::Error(ErrorCode::StorageError, "NVS putString failed");
        }

        LOG_INFO(CONFIG_SERVICE_TAG, "Config saved to NVS");
        return Status::OK();
    }

    Status ConfigService::updateFromJson(const std::string &json) {
        JsonDocument doc{};
        if (const auto err = deserializeJson(doc, json); err) {
            LOG_ERROR(CONFIG_SERVICE_TAG, "JSON parse error in update: %s", err.c_str());
            return Status::Error(ErrorCode::JsonError, "Deserialize failed");
        }

        auto newCfg{m_config};
        const auto root{doc.as<JsonObject>()};

        // Partial update - only override fields that are present
        if (const auto wifi = root["wifi"].as<JsonObjectConst>()) {
            if (wifi["ssid"].is<const char *>()) {
                newCfg.wifi.ssid = wifi["ssid"].as<const char *>();
            }
            if (wifi["password"].is<const char *>()) {
                newCfg.wifi.password = wifi["password"].as<const char *>();
            }
            if (wifi["connectTimeoutMs"].is<uint32_t>()) {
                newCfg.wifi.connectTimeoutMs = wifi["connectTimeoutMs"].as<uint32_t>();
            }
            if (wifi["maxRetries"].is<uint8_t>()) {
                newCfg.wifi.maxRetries = wifi["maxRetries"].as<uint8_t>();
            }
        }

        if (const auto mqtt = root["mqtt"].as<JsonObjectConst>()) {
            if (mqtt["broker"].is<const char *>()) {
                newCfg.mqtt.broker = mqtt["broker"].as<const char *>();
            }
            if (mqtt["port"].is<uint16_t>()) {
                newCfg.mqtt.port = mqtt["port"].as<uint16_t>();
            }
            if (mqtt["username"].is<const char *>()) {
                newCfg.mqtt.username = mqtt["username"].as<const char *>();
            }
            if (mqtt["password"].is<const char *>()) {
                newCfg.mqtt.password = mqtt["password"].as<const char *>();
            }
            if (mqtt["baseTopic"].is<const char *>()) {
                newCfg.mqtt.baseTopic = mqtt["baseTopic"].as<const char *>();
            }
            if (mqtt["tls"].is<bool>()) {
                newCfg.mqtt.tls = mqtt["tls"].as<bool>();
            }
            if (mqtt["keepAliveSeconds"].is<uint32_t>()) {
                newCfg.mqtt.keepAliveSeconds = mqtt["keepAliveSeconds"].as<uint32_t>();
            }
            if (mqtt["outboundQueueSize"].is<size_t>()) {
                newCfg.mqtt.outboundQueueSize = mqtt["outboundQueueSize"].as<size_t>();
            }
        }

        if (const auto dev = root["device"].as<JsonObjectConst>()) {
            if (dev["deviceId"].is<const char *>()) {
                newCfg.device.deviceId = dev["deviceId"].as<const char *>();
            }
            if (dev["locationId"].is<const char *>()) {
                newCfg.device.locationId = dev["locationId"].as<const char *>();
            }
        }

        if (const auto att = root["attendance"].as<JsonObjectConst>()) {
            if (att["debounceMs"].is<uint32_t>()) {
                newCfg.attendance.debounceMs = att["debounceMs"].as<uint32_t>();
            }
            if (att["offlineBufferSize"].is<size_t>()) {
                newCfg.attendance.offlineBufferSize = att["offlineBufferSize"].as<size_t>();
            }
            if (att["queueHighWatermark"].is<size_t>()) {
                newCfg.attendance.queueHighWatermark = att["queueHighWatermark"].as<size_t>();
            }
        }

        if (const auto pn532 = root["pn532"].as<JsonObjectConst>()) {
            if (pn532["pollIntervalMs"].is<uint32_t>()) {
                newCfg.pn532.pollIntervalMs = pn532["pollIntervalMs"].as<uint32_t>();
            }
            if (pn532["healthCheckIntervalMs"].is<uint32_t>()) {
                newCfg.pn532.healthCheckIntervalMs = pn532["healthCheckIntervalMs"].as<uint32_t>();
            }
            if (pn532["maxConsecutiveErrors"].is<uint8_t>()) {
                newCfg.pn532.maxConsecutiveErrors = pn532["maxConsecutiveErrors"].as<uint8_t>();
            }
        }

        if (const auto power = root["power"].as<JsonObjectConst>()) {
            if (power["sleepEnabled"].is<bool>()) {
                newCfg.power.sleepEnabled = power["sleepEnabled"].as<bool>();
            }
            if (power["idleTimeoutMs"].is<uint32_t>()) {
                newCfg.power.idleTimeoutMs = power["idleTimeoutMs"].as<uint32_t>();
            }
            if (power["sleepType"].is<const char *>()) {
                if (const auto sleepType = power["sleepType"].as<const char *>(); sleepType == "light") {
                    newCfg.power.sleepType = PowerConfig::SleepType::Light;
                }
                else if (sleepType == "deep") {
                    newCfg.power.sleepType = PowerConfig::SleepType::Deep;
                }
                else {
                    newCfg.power.sleepType = PowerConfig::SleepType::None;
                }
            }
        }

        if (const auto health = root["health"].as<JsonObjectConst>()) {
            if (health["checkIntervalMs"].is<uint32_t>()) {
                newCfg.health.checkIntervalMs = health["checkIntervalMs"].as<uint32_t>();
            }
            if (health["reportIntervalMs"].is<uint32_t>()) {
                newCfg.health.reportIntervalMs = health["reportIntervalMs"].as<uint32_t>();
            }
        }

        if (!newCfg.validate()) {
            LOG_WARNING(CONFIG_SERVICE_TAG, "Rejected invalid config update");
            return Status::Error(ErrorCode::InvalidArgument, "Config validation failed");
        }

        m_config = newCfg;
        (void) save(); // TODO: handle save error?
        notifyUpdated();
        return Status::OK();
    }

    void ConfigService::notifyUpdated() const {
        auto evt = std::make_unique<Event>(Event{
            .type = EventType::ConfigUpdated,
            .payload = ConfigUpdatedEvent{&m_config},
            .timestampMs = static_cast<std::uint64_t>(millis())
        });
        (void) m_bus.publish(std::move(evt)); // TODO: check publish result
    }

    void ConfigService::onEvent(const Event &event) {
        // Handle MQTT config update messages
        if (event.type == EventType::MqttMessageReceived) {
            if (const auto *msg = std::get_if<MqttMessageEvent>(&event.payload)) {
                // Check if it's a config update message
                if (msg->topic.find("/config/set") != std::string::npos) {
                    LOG_INFO(CONFIG_SERVICE_TAG, "Received config update via MQTT");
                    (void) updateFromJson(msg->payload); // TODO: handle update error?
                }
            }
        }
    }
}
