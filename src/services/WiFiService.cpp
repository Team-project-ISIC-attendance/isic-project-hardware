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
// Store HTML in flash memory to save RAM
constexpr char CONFIG_HTML[] PROGMEM = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ISIC Device Setup</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            min-height: 100vh;
            padding: 20px;
            color: #e0e0e0;
        }
        .container {
            max-width: 400px;
            margin: 0 auto;
            background: rgba(255,255,255,0.05);
            border-radius: 16px;
            padding: 30px;
            backdrop-filter: blur(10px);
            border: 1px solid rgba(255,255,255,0.1);
        }
        h1 {
            text-align: center;
            color: #4cc9f0;
            margin-bottom: 30px;
            font-size: 24px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            font-weight: 500;
            color: #b0b0b0;
        }
        input, select {
            width: 100%;
            padding: 12px 15px;
            border: 1px solid rgba(255,255,255,0.1);
            border-radius: 8px;
            background: rgba(0,0,0,0.2);
            color: #fff;
            font-size: 16px;
            transition: border-color 0.3s;
        }
        input:focus, select:focus {
            outline: none;
            border-color: #4cc9f0;
        }
        button {
            width: 100%;
            padding: 14px;
            border: none;
            border-radius: 8px;
            background: linear-gradient(135deg, #4cc9f0 0%, #4361ee 100%);
            color: #fff;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s, box-shadow 0.2s;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(76, 201, 240, 0.3);
        }
        button:disabled {
            opacity: 0.5;
            cursor: not-allowed;
            transform: none;
        }
        .networks {
            margin-bottom: 20px;
        }
        .network {
            padding: 12px;
            border: 1px solid rgba(255,255,255,0.1);
            border-radius: 8px;
            margin-bottom: 8px;
            cursor: pointer;
            display: flex;
            justify-content: space-between;
            align-items: center;
            transition: background 0.2s;
        }
        .network:hover {
            background: rgba(255,255,255,0.05);
        }
        .network.selected {
            border-color: #4cc9f0;
            background: rgba(76, 201, 240, 0.1);
        }
        .signal { color: #888; font-size: 12px; }
        .lock { margin-left: 8px; }
        .status {
            text-align: center;
            padding: 15px;
            border-radius: 8px;
            margin-top: 20px;
        }
        .status.success { background: rgba(0,200,100,0.2); color: #4ade80; }
        .status.error { background: rgba(200,0,0,0.2); color: #f87171; }
        .divider {
            border-top: 1px solid rgba(255,255,255,0.1);
            margin: 25px 0;
        }
        h3 { color: #b0b0b0; font-size: 14px; margin-bottom: 15px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>ISIC Device Setup</h1>

        <div class="networks" id="networks">
            <p style="text-align:center;color:#888">Scanning networks...</p>
        </div>

        <form id="configForm">
            <div class="form-group">
                <label>WiFi Network</label>
                <input type="text" id="ssid" name="ssid" required placeholder="Enter SSID">
            </div>
            <div class="form-group">
                <label>WiFi Password</label>
                <input type="password" id="password" name="password" placeholder="Enter password">
            </div>

            <div class="divider"></div>
            <h3>MQTT Settings (Optional)</h3>

            <div class="form-group">
                <label>MQTT Broker</label>
                <input type="text" id="mqtt_broker" name="mqtt_broker" placeholder="e.g., mqtt.example.com or 192.168.1.100">
            </div>
            <div class="form-group">
                <label>MQTT Port</label>
                <input type="number" id="mqtt_port" name="mqtt_port" value="1883" placeholder="1883">
            </div>
            <div class="form-group">
                <label>MQTT Username (optional)</label>
                <input type="text" id="mqtt_username" name="mqtt_username" placeholder="Leave empty if not needed">
            </div>
            <div class="form-group">
                <label>MQTT Password (optional)</label>
                <input type="password" id="mqtt_password" name="mqtt_password" placeholder="Leave empty if not needed">
            </div>
            <div class="form-group">
                <label>MQTT Base Topic (optional)</label>
                <input type="text" id="mqtt_base_topic" name="mqtt_base_topic" placeholder="Default: device">
            </div>

            <button type="submit" id="saveBtn">Save & Reboot</button>
        </form>

        <div id="status"></div>
    </div>

    <script>
        async function scanNetworks() {
            try {
                const resp = await fetch('/scan');
                const data = await resp.json();

                if (data.status === 'scanning') {
                    setTimeout(scanNetworks, 2000);
                    return;
                }

                const container = document.getElementById('networks');
                if (data.networks && data.networks.length > 0) {
                    container.innerHTML = data.networks.map(n => `
                        <div class="network" data-ssid="${n.ssid}">
                            <span>${n.ssid} ${n.secure ? '🔒' : ''}</span>
                            <span class="signal">${n.rssi} dBm</span>
                        </div>
                    `).join('');

                    document.querySelectorAll('.network').forEach(el => {
                        el.onclick = () => {
                            document.querySelectorAll('.network').forEach(e => e.classList.remove('selected'));
                            el.classList.add('selected');
                            document.getElementById('ssid').value = el.dataset.ssid;
                        };
                    });
                } else {
                    container.innerHTML = '<p style="text-align:center;color:#888">No networks found</p>';
                }
            } catch (e) {
                console.error(e);
            }
        }

        document.getElementById('configForm').onsubmit = async (e) => {
            e.preventDefault();
            const btn = document.getElementById('saveBtn');
            const status = document.getElementById('status');

            btn.disabled = true;
            btn.textContent = 'Saving...';

            try {
                const formData = new FormData(e.target);
                const resp = await fetch('/save', {
                    method: 'POST',
                    body: formData
                });
                const data = await resp.json();

                if (resp.ok) {
                    status.className = 'status success';
                    status.textContent = 'Configuration saved! Device is rebooting...';
                } else {
                    throw new Error(data.error || 'Save failed');
                }
            } catch (err) {
                status.className = 'status error';
                status.textContent = err.message;
                btn.disabled = false;
                btn.textContent = 'Save & Connect';
            }
        };

        scanNetworks();
    </script>
</body>
</html>
)";
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
            // AP mode handling - DNS server is non-blocking
            m_dnsServer.processNextRequest();
            break;
        }
        case WiFiState::Disconnected: {
            handleDisconnected();
            break;
        }
        case WiFiState::WaitingRetry: {
            // Non-blocking retry delay
            if (millis() - m_lastDisconnectMs >= 100)
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
        request->send_P(200, "text/html", CONFIG_HTML);
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
    WiFi.begin(m_config.stationSsid.c_str(), m_config.stationPassword.c_str());

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
    if (!m_config.isConfigured())
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
    LOG_INFO(m_name, "WiFi connected - service now Running, IP: %s, RSSI: %d", WiFi.localIP().toString().c_str(), m_metrics.rssi);

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
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void WiFiService::handleSaveConfig(AsyncWebServerRequest *request)
{
    const auto ssid{request->hasParam("ssid", true) ? request->getParam("ssid", true)->value() : ""};
    const auto password{request->hasParam("password", true) ? request->getParam("password", true)->value() : ""};

    if (ssid.isEmpty())
    {
        request->send(400, "application/json", R"({"error":"SSID required"})");
        return;
    }

    // Update configuration using generic update method
    m_configService.update([&](Config &cfg) {
        // Update WiFi configuration
        cfg.wifi.stationSsid = ssid.c_str();
        cfg.wifi.stationPassword = password.c_str();

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
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void WiFiService::enterPowerSleep()
{
    LOG_INFO(m_name, "WiFi entering power sleep");

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

    LOG_INFO(m_name, "WiFi powered down");
}

void WiFiService::wakeFromPowerSleep()
{
    LOG_INFO(m_name, "WiFi waking from power sleep");

    platform::wiFiPowerUp();

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
            // Light sleep: configure WiFi for light sleep mode
            platform::setWiFiLightSleep();
            LOG_INFO(m_name, "WiFi configured for light sleep");
            break;
        }
        case PowerState::ModemSleep:
        case PowerState::DeepSleep:
        case PowerState::Hibernating: {
            // Full sleep: disconnect and power down WiFi
            enterPowerSleep();
            break;
        }
        case PowerState::Active: {
            // Waking up: restore WiFi if was in modem sleep
            if (power->previousState == PowerState::ModemSleep)
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
