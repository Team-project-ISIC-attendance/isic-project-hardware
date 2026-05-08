#include "services/WiFiService.hpp"

#include "common/Logger.hpp"
#include "platform/PlatformESP.hpp"
#include "platform/PlatformWiFi.hpp"
#include "services/ConfigService.hpp"

#include <ArduinoJson.h>

namespace isic
{
namespace
{
// Lifehack when use username field - injected via compile-time string literal concatenation, see below is just not safe but works
#ifdef ISIC_WIFI_EDUROAM
#define EDUROAM_USERNAME_FIELD "<input name=username placeholder=Username>"
#else
#define EDUROAM_USERNAME_FIELD ""
#endif

// Stripped HTML: no doctype/html/head/body (optional in HTML5), MQTT collapsed in <details>
// Element IDs become implicit globals so no getElementById needed
constexpr char CONFIG_HTML[] PROGMEM =
"<meta name=viewport content='width=device-width'>"
"<style>input,button{display:block;width:100%;margin:5px 0;padding:8px;box-sizing:border-box}"
"button{background:#4cf;border:0;font-size:1em;cursor:pointer}"
"#m{padding:5px;display:none}.s{color:#4f4}.e{color:#f88}</style>"
"<h2>Setup</h2>"
"<form id=f>"
"<input name=ssid placeholder=SSID required>"
EDUROAM_USERNAME_FIELD
"<input type=password name=password placeholder=Password>"
"<details><summary>MQTT (optional)</summary>"
"<input name=mqtt_broker placeholder=Broker>"
"<input name=mqtt_port value=1883 placeholder=Port>"
"<input name=mqtt_username placeholder=User>"
"<input type=password name=mqtt_password placeholder=Pass>"
"<input name=mqtt_base_topic placeholder='Topic (device)'>"
"</details>"
"<button>Save &amp; Reboot</button>"
"</form>"
"<div id=m></div>"
"<script>f.onsubmit=async e=>{"
"e.preventDefault();"
"try{"
"const r=await fetch('/save',{method:'POST',body:new FormData(f)});"
"m.className=r.ok?'s':'e';"
"m.textContent=r.ok?'Saved! Rebooting...':(await r.json()).error||'Error';"
"}catch(x){m.className='e';m.textContent='Error';}"
"m.style.display='block';"
"};</script>";
} // namespace

WiFiService::WiFiService(EventBus &bus, ConfigService &config, AsyncWebServer &webServer)
    : ServiceBase("WiFiService")
    , m_bus(bus)
    , m_config(config.getWifiConfig())
    , m_configService(config)
    , m_webServer(webServer)
    , m_hasEverConnected(m_config.stationHasEverConnected)
{
    m_eventConnections.reserve(2);
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::PowerStateChange, [this](const Event &e) {
        handlePowerStateChange(e);
    }));
}

Status WiFiService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing WiFiService...");

    WiFi.persistent(false); // non use static :persistent not works in esp32
    WiFi.mode(WIFI_OFF);
    delay(100); // TODO: need fix this is brief delay for WiFi hardware reset - unavoidable during initialization

    if (!m_config.isConfigured())
    {
        LOG_INFO(m_name, "WiFi not configured, starting AP mode");
        startApMode();
        // AP mode is operational - transition to Running
        setState(ServiceState::Running);
        return Status::Ok();
    }

    LOG_INFO(m_name, "Connecting to %s...", m_config.stationSsid.c_str());
    connectToStation();

    setState(ServiceState::Ready);
    LOG_INFO(m_name, "Ready (waiting for WiFi connection)");
    return Status::Ok();
}

void WiFiService::loop()
{
    if (m_state != ServiceState::Ready && m_state != ServiceState::Running)
    {
        return;
    }

    switch (m_wifiState)
    {
        case WiFiState::Connecting: {
            handleConnecting();
            break;
        }
        case WiFiState::Connected: {
            handleConnected();
            break;
        }
        case WiFiState::ApMode: {
            m_dnsServer.processNextRequest();
            // Enforce AP timeout: only reset m_apStartMs in startApMode(), not here
            if (m_config.accessPointModeTimeoutMs > 0 &&
                (millis() - m_apStartMs) >= m_config.accessPointModeTimeoutMs)
            {
                LOG_INFO(m_name, "AP timeout — switching back to station mode");
                stopApMode();
                m_wifiState = WiFiState::Disconnected;
                m_lastReconnectAttemptMs = 0;
                connectToStation();
            }
            break;
        }
        case WiFiState::Disconnected: {
            handleDisconnected();
            break;
        }
        case WiFiState::WaitingRetry: {
            // Non-blocking retry delay
            if (!m_powerSleepActive && millis() - m_lastDisconnectMs >= 100)
            {
                connectToStation();
            }
            break;
        }
        default: {
            break;
        }
    }
}

void WiFiService::end()
{
    setState(ServiceState::Stopping);
    LOG_INFO(m_name, "Shutting down...");

    if (WiFi.status() == WL_CONNECTED)
    {
        WiFi.disconnect();
    }
    if (m_apActive)
    {
        stopApMode();
    }

    WiFi.mode(WIFI_OFF);
    m_wifiState = WiFiState::Disconnected;

    setState(ServiceState::Stopped);
    LOG_INFO(m_name, "Stopped");
}

int8_t WiFiService::getRssi() const
{
    return isConnected() ? static_cast<int8_t>(WiFi.RSSI()) : 0;
}

void WiFiService::startApMode()
{
    auto apSsid{m_config.accessPointSsidPrefix};
    apSsid += platform::getChipIdHex().c_str();

    LOG_INFO(m_name, "Starting AP: %s", apSsid.c_str());

    // Configure and start AP
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));

    if (!m_config.accessPointPassword.empty())
    {
        WiFi.softAP(apSsid.c_str(), m_config.accessPointPassword.c_str());
    }
    else
    {
        WiFi.softAP(apSsid.c_str());
    }

    // Start DNS server for captive portal
    m_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    m_dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

    // Setup web server endpoints
    setupWebServer();

    m_wifiState = WiFiState::ApMode;
    m_apActive = true;
    m_apStartMs = millis(); // reset timeout only when AP actually starts

    LOG_INFO(m_name, "AP started, IP: %s", WiFi.softAPIP().toString().c_str());
    m_bus.publish(EventType::WifiApStarted);
}

void WiFiService::stopApMode()
{
    if (!m_apActive)
    {
        return;
    }

    LOG_INFO(m_name, "Stopping AP mode");

    m_dnsServer.stop();
    WiFi.softAPdisconnect(true);
    m_apActive = false;

    m_bus.publish(EventType::WifiApStopped);
}

void WiFiService::setupWebServer()
{
    // Configuration page
    m_webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", CONFIG_HTML);
    });

    // Captive portal detection endpoints
    m_webServer.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");
    });
    m_webServer.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");
    });
    m_webServer.on("/fwlink", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");
    });

    // WiFi scan endpoint
    m_webServer.on("/scan", HTTP_GET, [this](AsyncWebServerRequest *request) {
        handleScanNetworks(request);
    });

    // Save configuration endpoint
    m_webServer.on("/save", HTTP_POST, [this](AsyncWebServerRequest *request) {
        handleSaveConfig(request);
    });

    // Status endpoint
    m_webServer.on("/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        handleStatus(request);
    });
}

void WiFiService::connectToStation()
{
    if (!m_config.isConfigured())
    {
        return;
    }

    WiFi.mode(WIFI_STA);
    platform::setWiFiNormalPower();
    if (m_config.stationPowerSaveEnabled)
    {
        platform::setWiFiLightSleep();
    }

#ifdef ISIC_WIFI_EDUROAM
    platform::connectEduroam(m_config.stationSsid.c_str(), m_config.stationUsername.c_str(), m_config.stationPassword.c_str());
#else
    WiFi.begin(m_config.stationSsid.c_str(), m_config.stationPassword.c_str());
#endif

    m_wifiState = WiFiState::Connecting;
    m_connectStartMs = millis();
    ++m_connectAttempts;

    if (m_inSlowRetryMode)
    {
        LOG_INFO(m_name, "Slow retry attempt #%d to %s...", m_connectAttempts, m_config.stationSsid.c_str());
    }
    else
    {
        LOG_INFO(m_name, "Connecting to %s (attempt %d/%d)...", m_config.stationSsid.c_str(), m_connectAttempts, m_config.stationMaxFastConnectionAttempts);
    }
}

void WiFiService::handleConnecting()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        onConnected();
        return;
    }

    // Check timeout
    if (millis() - m_connectStartMs >= m_config.stationConnectionTimeoutMs)
    {
        if (!m_inSlowRetryMode && m_connectAttempts >= m_config.stationMaxFastConnectionAttempts)
        {
            if (!m_hasEverConnected) // If never connected before, start AP mode
            {
                LOG_ERROR(m_name, "Max fast retries (%d) reached and never connected, starting AP mode", m_config.stationMaxFastConnectionAttempts);
                WiFi.disconnect();
                m_wifiState = WiFiState::Disconnected;
                startApMode();
                return;
            }

            m_inSlowRetryMode = true; // If was connected before, switch to slow retry mode (WiFi temporarily down)
            LOG_WARN(m_name, "Max fast retries (%d) reached, switching to slow retry mode (every 10 min) - WiFi may be temporarily down", m_config.stationMaxFastConnectionAttempts);
        }

        WiFi.disconnect();
        m_wifiState = WiFiState::Disconnected;
        m_lastReconnectAttemptMs = millis();

        if (m_inSlowRetryMode)
        {
            LOG_DEBUG(m_name, "Will retry in 10 minutes...");
        }
        else
        {
            LOG_WARN(m_name, "Connect timeout (attempt %d/%d), will retry in 5 seconds", m_connectAttempts, m_config.stationMaxFastConnectionAttempts);
        }
    }
}

void WiFiService::handleConnected()
{
    // Monitor connection status - transition to disconnected if connection lost
    if (WiFi.status() != WL_CONNECTED)
    {
        onDisconnected();
        return;
    }

    // Ensure we're in Running state when connected
    if (m_state != ServiceState::Running)
    {
        setState(ServiceState::Running);
    }
}

void WiFiService::handleDisconnected()
{
    if (m_powerSleepActive || !m_config.isConfigured())
    {
        return;
    }

    const auto currentMs{millis()};
    const auto timeSinceLastAttempt{currentMs - m_lastReconnectAttemptMs};
    const auto retryInterval{m_inSlowRetryMode ? m_config.stationSlowReconnectIntervalMs : m_config.stationFastReconnectIntervalMs};

    if (timeSinceLastAttempt >= retryInterval)
    {
        m_lastReconnectAttemptMs = currentMs;
        connectToStation();
    }
}

void WiFiService::onConnected()
{
    m_wifiState = WiFiState::Connected;

    const auto wasFirstConnection{!m_hasEverConnected};
    m_hasEverConnected = true;

    if (wasFirstConnection)
    {
        m_configService.update([](Config &cfg) {
            cfg.wifi.stationHasEverConnected = true;
        });
        LOG_INFO(m_name, "First successful WiFi connection - flag persisted to config");
    }

    m_connectAttempts = 0;
    m_inSlowRetryMode = false;

    if (!m_timeSyncStarted)
    {
        configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
        m_timeSyncStarted = true;
        LOG_INFO(m_name, "NTP sync requested");
    }

    // Service is now fully operational - transition to Running
    setState(ServiceState::Running);
    LOG_INFO(m_name, "WiFi connected - service now Running, IP: %s, RSSI: %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());

    // Stop AP mode if it was running
    if (m_apActive)
    {
        stopApMode();
    }

    m_bus.publish(EventType::WifiConnected);
}

void WiFiService::onDisconnected()
{
    m_wifiState = WiFiState::Disconnected;
    ++m_metrics.disconnectCount;

    setState(ServiceState::Ready);
    LOG_WARN(m_name, "WiFi disconnected - service now Ready (will reconnect)");

    m_bus.publish(EventType::WifiDisconnected);
}

void WiFiService::handleScanNetworks(AsyncWebServerRequest *request)
{
    const auto result{WiFi.scanComplete()};

    if (result == WIFI_SCAN_FAILED)
    {
        WiFi.scanNetworks(true); // Async scan
        request->send(202, "application/json", R"({"status":"scanning"})");
        return;
    }

    if (result == WIFI_SCAN_RUNNING)
    {
        request->send(202, "application/json", R"({"status":"scanning"})");
        return;
    }

    JsonDocument doc;
    const auto networks{doc["networks"].to<JsonArray>()};

    for (auto i = 0; i < result; i++)
    {
        auto net{networks.add<JsonObject>()};
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["secure"] = platform::isNetworkSecure(i);
    }

    WiFi.scanDelete();
    WiFi.scanNetworks(true); // Start new scan for next request

    String json;
    json.reserve(measureJson(doc) + 1);
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void WiFiService::handleSaveConfig(AsyncWebServerRequest *request)
{
    const auto ssid{request->hasParam("ssid", true) ? request->getParam("ssid", true)->value() : ""};

    if (ssid.isEmpty())
    {
        request->send(400, "application/json", R"({"error":"SSID required"})");
        return;
    }

    // Update configuration using generic update method
    m_configService.update([&](Config &cfg) {
        // Update WiFi configuration
        cfg.wifi.stationSsid = ssid.c_str();

        if (request->hasParam("password", true))
        {
            cfg.wifi.stationPassword = request->getParam("password", true)->value().c_str();
        }

#ifdef ISIC_WIFI_EDUROAM
        if (request->hasParam("username", true))
        {
            cfg.wifi.stationUsername = request->getParam("username", true)->value().c_str();
        }
#endif

        // Update MQTT configuration (optional)
        if (request->hasParam("mqtt_broker", true))
        {
            if (const auto broker{request->getParam("mqtt_broker", true)->value()}; !broker.isEmpty())
            {
                cfg.mqtt.brokerAddress = broker.c_str();
                LOG_INFO(m_name, "MQTT broker updated: %s", broker.c_str());
            }
        }
        if (request->hasParam("mqtt_port", true))
        {
            cfg.mqtt.port = request->getParam("mqtt_port", true)->value().toInt();
        }
        if (request->hasParam("mqtt_username", true))
        {
            cfg.mqtt.username = request->getParam("mqtt_username", true)->value().c_str();
        }
        if (request->hasParam("mqtt_password", true))
        {
            cfg.mqtt.password = request->getParam("mqtt_password", true)->value().c_str();
        }
        if (request->hasParam("mqtt_base_topic", true))
        {
            if (const auto topic = request->getParam("mqtt_base_topic", true)->value(); !topic.isEmpty())
            {
                cfg.mqtt.baseTopic = topic.c_str();
            }
        }
    });

    request->send(200, "application/json", R"({"status":"saved","message":"Connecting to WiFi..."})");

    LOG_INFO(m_name, "Config saved (WiFi: %s, MQTT: %s), transitioning from AP mode to station mode",
             m_configService.getWifiConfig().stationSsid.c_str(),
             m_configService.getMqttConfig().brokerAddress.empty() ? "not configured" : m_configService.getMqttConfig().brokerAddress.c_str());

    // Stop AP mode and connect to the configured network
    stopApMode();
    connectToStation();
}

void WiFiService::handleStatus(AsyncWebServerRequest *request)
{
    JsonDocument doc;

    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    doc["ap_active"] = m_apActive;

    if (WiFi.status() == WL_CONNECTED)
    {
        doc["ssid"] = WiFi.SSID();
        doc["ip"] = WiFi.localIP().toString();
        doc["rssi"] = WiFi.RSSI();
    }

    if (m_apActive)
    {
        doc["ap_ip"] = WiFi.softAPIP().toString();
        doc["ap_clients"] = WiFi.softAPgetStationNum();
    }

    String json;
    json.reserve(measureJson(doc) + 1);
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void WiFiService::enterPowerSleep()
{
    LOG_INFO(m_name, "WiFi entering power sleep");

    m_lightSleepConfigured = false;

    if (m_wifiState == WiFiState::Connected || m_wifiState == WiFiState::Connecting)
    {
        WiFi.disconnect(true);
    }

    if (m_apActive)
    {
        stopApMode();
    }

    WiFi.mode(WIFI_OFF);
    platform::wiFiPowerDown();
    m_powerSleepActive = true;

    LOG_INFO(m_name, "WiFi powered down");
}

void WiFiService::wakeFromPowerSleep()
{
    LOG_INFO(m_name, "WiFi waking from power sleep");

    platform::wiFiPowerUp();
    m_powerSleepActive = false;
    m_lightSleepConfigured = false;

    if (m_config.isConfigured())
    {
        // Reconnect to configured network
        connectToStation();
        LOG_INFO(m_name, "WiFi reconnecting to %s", m_config.stationSsid.c_str());
    }
    else
    {
        // Start AP mode if not configured
        startApMode();
    }
}

void WiFiService::handlePowerStateChange(const Event &event)
{
    const auto *power{event.get<PowerEvent>()};
    if (!power)
    {
        return;
    }

    LOG_DEBUG(m_name, "Power state change: %s -> %s", toString(power->previousState), toString(power->targetState));

    switch (power->targetState)
    {
        case PowerState::LightSleep: {
            if (!m_powerSleepActive && m_wifiState == WiFiState::Connected)
            {
                platform::setWiFiLightSleep();
                m_lightSleepConfigured = true;
                LOG_INFO(m_name, "WiFi configured for reader light sleep");
            }
            break;
        }
        case PowerState::ModemSleep: {
            // Full sleep: disconnect and power down WiFi
            if (!m_powerSleepActive)
            {
                enterPowerSleep();
            }
            break;
        }
        case PowerState::Active: {
            if (power->previousState == PowerState::LightSleep && m_lightSleepConfigured)
            {
                platform::setWiFiNormalPower();
                if (m_config.stationPowerSaveEnabled)
                {
                    platform::setWiFiLightSleep();
                }
                m_lightSleepConfigured = false;
                LOG_INFO(m_name, "WiFi restored after reader light sleep");
            }

            if (power->previousState == PowerState::ModemSleep && m_powerSleepActive)
            {
                wakeFromPowerSleep();
            }
            break;
        }
        default:
            break;
    }
}
} // namespace isic
