#ifndef ISIC_SERVICES_WIFISERVICE_HPP
#define ISIC_SERVICES_WIFISERVICE_HPP

#include "common/Config.hpp"
#include "core/EventBus.hpp"
#include "core/IService.hpp"
#include "platform/PlatformWiFi.hpp"

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <vector>

namespace isic
{
class WiFiService : public ServiceBase
{
public:
    WiFiService(EventBus &bus, const WiFiConfig &config, Config& systemConfig);
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

    [[nodiscard]] String getConfigPageHtml() const;

    void enterPowerSleep();
    void wakeFromPowerSleep();
    void handlePowerStateChange(const Event &event);

    EventBus &m_bus;
    const WiFiConfig &m_config;
    Config& m_systemConfig;

    AsyncWebServer m_webServer{80};
    DNSServer m_dnsServer;

    WiFiState m_wifiState{WiFiState::Disconnected};
    std::uint32_t m_lastCheckMs{0};
    std::uint32_t m_connectStartMs{0};

    std::uint32_t m_lastReconnectAttemptMs{0};
    std::uint32_t m_connectAttempts{0};
    std::uint32_t m_apStartMs{0};
    std::uint8_t m_connectRetries{0};

    WiFiMetrics m_metrics{};

    bool m_webServerStarted{false};
    bool m_apActive{false};

    std::uint32_t m_lastDisconnectMs{0};
    bool m_waitingForRetry{false};

    bool m_timeSyncStarted{false};

    bool m_rebootPending{false};
    std::uint32_t m_rebootRequestedMs{0};

    std::vector<EventBus::ScopedConnection> m_eventConnections;
};
} // namespace isic

#endif // ISIC_SERVICES_WIFISERVICE_HPP
