#ifndef ISIC_SERVICES_OTASERVICE_HPP
#define ISIC_SERVICES_OTASERVICE_HPP

#include "core/EventBus.hpp"
#include "core/IService.hpp"
#include "common/Config.hpp"

#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <vector>

namespace isic
{
class OtaService : public ServiceBase
{
public:
    OtaService(EventBus &bus, const OtaConfig &config, AsyncWebServer &webServer);
    ~OtaService() override = default;

    OtaService(const OtaService &) = delete;
    OtaService &operator=(const OtaService &) = delete;
    OtaService(OtaService &&) = delete;
    OtaService &operator=(OtaService &&) = delete;

    // IService implementation
    Status begin() override;
    void loop() override;
    void end() override;

    [[nodiscard]] OtaState getOtaState() const
    {
        return m_otaState;
    }
    [[nodiscard]] bool isUpdating() const
    {
        return m_otaState == OtaState::Downloading;
    }
    [[nodiscard]] std::uint8_t getProgress() const
    {
        return m_progress;
    }

private:
    void onOtaStart();
    void onOtaEnd(bool success);
    void onOtaProgress(std::size_t current, std::size_t total);

    EventBus &m_bus;
    const OtaConfig &m_config;
    AsyncWebServer &m_webServer;  // Reference to shared web server

    OtaState m_otaState{OtaState::Idle};
    std::uint8_t m_progress{0};

    std::vector<EventBus::ScopedConnection> m_eventConnections;
};
} // namespace isic

#endif // ISIC_SERVICES_OTASERVICE_HPP
