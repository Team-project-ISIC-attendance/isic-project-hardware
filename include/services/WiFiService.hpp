#ifndef ISIC_SERVICES_WIFISERVICE_HPP
#define ISIC_SERVICES_WIFISERVICE_HPP

#include "core/EventBus.hpp"
#include "core/IService.hpp"

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

#include <vector>

namespace isic
{
class ConfigService;
class WiFiConfig;

class WiFiService : public ServiceBase
{
public:
    WiFiService(EventBus &bus, ConfigService &configService, AsyncWebServer &webServer);
    ~WiFiService() override = default;

    WiFiService(const WiFiService&) = delete;
    WiFiService& operator=(const WiFiService&) = delete;
    WiFiService(WiFiService&&) = delete;
    WiFiService& operator=(WiFiService&&) = delete;

    // IService implementation
    Status begin() override;
    void loop() override;
    void end() override;

    [[nodiscard]] WiFiState getWiFiState() const
    {
        return m_wifiState;
    }
    [[nodiscard]] WiFiMetrics getWiFiMetrics() const
    {
        return m_metrics;
    }
    [[nodiscard]] bool isConnected() const
    {
        return m_wifiState == WiFiState::Connected;
    }
    [[nodiscard]] bool isApMode() const
    {
        return m_wifiState == WiFiState::ApMode;
    }

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
        obj["disconnect_count"] = m_metrics.disconnectCount;
    }

private:
    void startApMode();
    void stopApMode();

    void setupWebServer();

    void connectToStation();

    void handleConnecting();
    void handleConnected();
    void handleDisconnected();

    void onConnected();
    void onDisconnected();

    void handleScanNetworks(AsyncWebServerRequest *request);
    void handleSaveConfig(AsyncWebServerRequest *request);
    void handleStatus(AsyncWebServerRequest *request);

    void enterPowerSleep();
    void wakeFromPowerSleep();
    void handlePowerStateChange(const Event &event);

    EventBus &m_bus;
    const WiFiConfig &m_config; // cashed reference to WiFi config
    ConfigService &m_configService;

    AsyncWebServer& m_webServer;
    DNSServer m_dnsServer;

    WiFiState m_wifiState{WiFiState::Disconnected};
    std::uint32_t m_lastCheckMs{0};
    std::uint32_t m_connectStartMs{0};

    std::uint32_t m_lastReconnectAttemptMs{0};
    std::uint32_t m_connectAttempts{0};
    std::uint32_t m_lastDisconnectMs{0};
    std::uint32_t m_apStartMs{0};
    std::uint8_t m_connectRetries{0};
    bool m_inSlowRetryMode{false};
    bool m_hasEverConnected{false};
    bool m_apActive{false};
    bool m_timeSyncStarted{false};

    WiFiMetrics m_metrics{};

    std::vector<EventBus::Subscription> m_eventConnections;
};
} // namespace isic

#endif // ISIC_SERVICES_WIFISERVICE_HPP
