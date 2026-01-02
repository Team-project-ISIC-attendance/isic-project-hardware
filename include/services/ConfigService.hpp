#ifndef ISIC_SERVICES_CONFIGSERVICE_HPP
#define ISIC_SERVICES_CONFIGSERVICE_HPP

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>

namespace isic
{
class ConfigService : public ServiceBase
{
public:
    static constexpr auto *CONFIG_FILE{"/config.json"};

    explicit ConfigService(EventBus &bus);
    ~ConfigService() override;

    ConfigService(const ConfigService &) = delete;
    ConfigService &operator=(const ConfigService &) = delete;
    ConfigService(ConfigService &&) = delete;
    ConfigService &operator=(ConfigService &&) = delete;

    // IService implementation
    [[nodiscard]] Status begin() override;
    void loop() override;
    void end() override;

    [[nodiscard]] const Config &get() const
    {
        return m_config;
    }
    [[nodiscard]] Config &getMutable()
    {
        return m_config;
    }
    [[nodiscard]] const WiFiConfig &getWifiConfig() const noexcept
    {
        return m_config.wifi;
    }
    [[nodiscard]] const MqttConfig &getMqttConfig() const noexcept
    {
        return m_config.mqtt;
    }
    [[nodiscard]] const DeviceConfig &getDeviceConfig() const noexcept
    {
        return m_config.device;
    }
    [[nodiscard]] const Pn532Config &getPn532Config() const noexcept
    {
        return m_config.pn532;
    }
    [[nodiscard]] const AttendanceConfig &getAttendanceConfig() const noexcept
    {
        return m_config.attendance;
    }
    [[nodiscard]] const FeedbackConfig &getFeedbackConfig() const noexcept
    {
        return m_config.feedback;
    }
    [[nodiscard]] const HealthConfig &getHealthConfig() const noexcept
    {
        return m_config.health;
    }
    [[nodiscard]] const OtaConfig &getOtaConfig() const noexcept
    {
        return m_config.ota;
    }
    [[nodiscard]] const PowerConfig &getPowerConfig() const noexcept
    {
        return m_config.power;
    }

    [[nodiscard]] Status save();
    [[nodiscard]] Status saveNow();
    [[nodiscard]] Status load();
    [[nodiscard]] Status reset();
    [[nodiscard]] Status updateFromJson(const char *json);

    [[nodiscard]] bool isConfigured() const noexcept
    {
        return m_config.isConfigured();
    }
    [[nodiscard]] bool isDirty() const noexcept
    {
        return m_dirty;
    }

private:
    void handleConfigMessage(const std::string& topic, const std::string& payload);

    bool parseJson(const char *json);
    bool parseWifiConfig(const JsonVariant &json);
    bool parseMqttConfig(const JsonVariant &json);
    bool parseDeviceConfig(const JsonVariant &json);
    bool parsePn532Config(const JsonVariant &json);
    bool parseAttendanceConfig(const JsonVariant &json);
    bool parseFeedbackConfig(const JsonVariant &json);
    bool parseHealthConfig(const JsonVariant &json);
    bool parseOtaConfig(const JsonVariant &json);
    bool parsePowerConfig(const JsonVariant &json);

    EventBus &m_bus;
    Config m_config{};

    std::vector<EventBus::ScopedConnection> m_eventConnections{};

    bool m_dirty{false};
};
} // namespace isic

#endif // ISIC_SERVICES_CONFIGSERVICE_HPP
