#ifndef ISIC_SERVICES_CONFIGSERVICE_HPP
#define ISIC_SERVICES_CONFIGSERVICE_HPP

/**
 * @file ConfigService.hpp
 * @brief Configuration storage and access service
 *
 * Loads, persists, and publishes configuration updates across services.
 */

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <vector>

namespace isic
{
class ConfigService : public ServiceBase
{
    static constexpr auto *kConfigFile{"/config.json"};

public:
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

    template<typename UpdateFunc>
    void update(UpdateFunc&& func)
    {
        func(m_config);
        (void)save(); // must be always successful
        m_bus.publish(EventType::ConfigChanged);
    }

    [[nodiscard]] bool isConfigured() const noexcept
    {
        return m_config.isConfigured();
    }
    [[nodiscard]] bool isDirty() const noexcept
    {
        return m_dirty;
    }

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
    }

private:
    void handleSetConfigMessage(const std::string &topic, const std::string &payload);
    void handleGetConfigMessage(const std::string &topic);

    EventBus &m_bus;
    Config m_config{};

    // Event connections
    std::vector<EventBus::ScopedConnection> m_eventConnections{};

    // Dirty flag to indicate unsaved changes
    bool m_dirty{false};
};
} // namespace isic

#endif // ISIC_SERVICES_CONFIGSERVICE_HPP
