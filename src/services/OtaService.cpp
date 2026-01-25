#include "services/OtaService.hpp"

#if ISIC_ENABLE_OTA

#include "common/Logger.hpp"

#ifdef ISIC_PLATFORM_ESP8266
#include <ESP8266HTTPClient.h>
#elif defined(ISIC_PLATFORM_ESP32)
#include <HTTPClient.h>
#endif

#include <ArduinoJson.h>
#include <Stream.h>
#include <Update.h>
#include <WiFiClient.h>
#include <algorithm>

namespace isic
{
namespace
{
constexpr auto *TAG{"OtaService"};

int compareVersions(const char *v1, const char *v2)
{
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;

    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);

    if (major1 != major2) return (major1 > major2) ? 1 : -1;
    if (minor1 != minor2) return (minor1 > minor2) ? 1 : -1;
    if (patch1 != patch2) return (patch1 > patch2) ? 1 : -1;
    return 0;
}
} // namespace

OtaService::OtaService(EventBus &bus, const OtaConfig &config)
    : ServiceBase("OtaService")
    , m_bus(bus)
    , m_config(config)
{
    m_eventConnections.reserve(3);
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttConnected, [this](const Event &) {
        m_bus.publish({EventType::MqttSubscribeRequest, MqttEvent{.topic = "ota/start"}});
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttMessage, [this](const Event &event) {
        if (const auto *mqtt = event.get<MqttEvent>())
        {
            if (mqtt->topic.find("/ota/start") != std::string::npos)
            {
                m_pendingCheck = true;
            }
        }
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::WifiDisconnected, [this](const Event &) {
        if (m_otaState == OtaState::Downloading)
        {
            LOG_WARN(TAG, "WiFi disconnected, aborting OTA");
            failDownload("WiFi disconnected");
        }
    }));
}

Status OtaService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(TAG, "Initializing...");

    if (!m_config.enabled)
    {
        LOG_INFO(TAG, "Disabled by config");
        setState(ServiceState::Running);
        return Status::Ok();
    }

    // Auto-check on WiFi connect
    if (m_config.checkOnConnect && m_config.isConfigured())
    {
        m_eventConnections.push_back(m_bus.subscribeScoped(EventType::WifiConnected, [this](const Event &) {
            LOG_INFO(TAG, "WiFi connected, scheduling check");
            m_pendingCheck = true;
        }));
    }

    if (m_config.isConfigured())
    {
        LOG_INFO(TAG, "Server: %s", m_config.serverUrl.c_str());
    }
    else
    {
        LOG_INFO(TAG, "Server not configured");
    }

    setState(ServiceState::Running);
    return Status::Ok();
}

void OtaService::loop()
{
     if (!m_config.enabled || !m_config.isConfigured())
    {
        return;
    }

    if (m_pendingCheck && m_otaState != OtaState::Downloading)
    {
        m_pendingCheck = false;
        LOG_INFO(TAG, "OTA check requested");
        checkForUpdate();
    }

    if (m_otaState == OtaState::Downloading)
    {
        LOG_INFO(TAG, "Processing OTA download...");
        processDownload();
    }
}

void OtaService::end()
{
    m_eventConnections.clear();
    setState(ServiceState::Stopped);
}

void OtaService::checkForUpdate()
{
    if (!m_config.isConfigured())
    {
        LOG_WARN(TAG, "Server not configured");
        return;
    }

    if (m_otaState == OtaState::Downloading)
    {
        LOG_WARN(TAG, "Update already in progress");
        return;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        LOG_WARN(TAG, "WiFi not connected");
        return;
    }

    LOG_INFO(TAG, "Checking for updates...");
    m_otaState = OtaState::Checking;

    std::string serverVersion, serverMd5;
    std::uint32_t serverSize{0};
    if (!fetchManifest(serverVersion, serverMd5, serverSize))
    {
        LOG_ERROR(TAG, "Failed to fetch manifest");
        m_otaState = OtaState::Idle;
        return;
    }

    if (isNewerVersion(serverVersion))
    {
        LOG_INFO(TAG, "Update: %s -> %s", DeviceConfig::Constants::kFirmwareVersion, serverVersion.c_str());
        m_bus.publish(EventType::OtaStarted);
        if (!beginDownload(serverMd5, serverSize))
        {
            failDownload("Failed to start download");
        }
    }
    else
    {
        LOG_INFO(TAG, "Up to date (v%s)", DeviceConfig::Constants::kFirmwareVersion);
        m_otaState = OtaState::Idle;
    }
}

bool OtaService::fetchManifest(std::string &outVersion, std::string &outMd5, std::uint32_t &outSize)
{
    WiFiClient client;
    HTTPClient http;

    std::string url = m_config.serverUrl + "/manifest.json";
    http.setTimeout(m_config.timeoutMs);

    if (!http.begin(client, url.c_str()))
    {
        LOG_ERROR(TAG, "HTTP begin failed");
        return false;
    }

    if (!m_config.username.empty())
    {
        http.setAuthorization(m_config.username.c_str(), m_config.password.c_str());
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        LOG_ERROR(TAG, "HTTP %d", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload))
    {
        LOG_ERROR(TAG, "JSON parse failed");
        return false;
    }

    if (!doc["version"].is<const char *>())
    {
        LOG_ERROR(TAG, "Missing 'version'");
        return false;
    }

    if(!doc["board"].is<const char *>())
    {
        LOG_ERROR(TAG, "Missing 'board'");
        return false;
    }

    #ifdef ISIC_PLATFORM_ESP8266
    if(strcmp(doc["board"].as<const char *>(), "esp8266") != 0)
    {
        LOG_ERROR(TAG, "Manifest board mismatch");
        return false;
    }
    #elif defined(ISIC_PLATFORM_ESP32)
    if(strcmp(doc["board"].as<const char *>(), "esp32dev") != 0)
    {
        LOG_ERROR(TAG, "Manifest board mismatch");
        return false;
    }
    #endif

    outVersion = doc["version"].as<const char *>();
    if (doc["md5"].is<const char *>())
    {
        outMd5 = doc["md5"].as<const char *>();
    }
    if (doc["size"].is<std::uint32_t>())
    {
        outSize = doc["size"].as<std::uint32_t>();
    }
    else if (doc["size"].is<int>())
    {
        outSize = static_cast<std::uint32_t>(doc["size"].as<int>());
    }
    else
    {
        outSize = 0;
    }

    return true;
}

bool OtaService::isNewerVersion(const std::string &serverVersion) const
{
    return compareVersions(serverVersion.c_str(), DeviceConfig::Constants::kFirmwareVersion) > 0;
}

bool OtaService::beginDownload(const std::string &expectedMd5, std::uint32_t expectedSize)
{
    std::string url = m_config.serverUrl + "/firmware.bin";
    LOG_INFO(TAG, "Starting download: %s", url.c_str());

    m_otaState = OtaState::Downloading;
    m_progress = 0;
    m_updateMd5 = expectedMd5;
    m_updateTotalSize = expectedSize;
    m_updateDownloaded = 0;
    m_lastDownloadActivityMs = millis();
    m_lastProgressPublishMs = 0;

    m_updateHttp.setTimeout(m_config.timeoutMs);
    if (!m_updateHttp.begin(m_updateClient, url.c_str()))
    {
        LOG_ERROR(TAG, "HTTP begin failed");
        return false;
    }

    if (!m_config.username.empty())
    {
        m_updateHttp.setAuthorization(m_config.username.c_str(), m_config.password.c_str());
    }

    const int code = m_updateHttp.GET();
    if (code != HTTP_CODE_OK)
    {
        LOG_ERROR(TAG, "HTTP %d", code);
        m_updateHttp.end();
        return false;
    }

    if (m_updateTotalSize == 0)
    {
        const int reportedSize = m_updateHttp.getSize();
        if (reportedSize > 0)
        {
            m_updateTotalSize = static_cast<std::uint32_t>(reportedSize);
        }
    }

    if (m_updateTotalSize > 0)
    {
        if (!Update.begin(m_updateTotalSize))
        {
            LOG_ERROR(TAG, "Update begin failed: %u", Update.getError());
            m_updateHttp.end();
            return false;
        }
    }
    else
    {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            LOG_ERROR(TAG, "Update begin failed: %u", Update.getError());
            m_updateHttp.end();
            return false;
        }
    }

    if (!m_updateMd5.empty())
    {
        Update.setMD5(m_updateMd5.c_str());
    }

    m_updateStream = m_updateHttp.getStreamPtr();
    if (m_updateStream == nullptr)
    {
        LOG_ERROR(TAG, "Stream not available");
        Update.abort();
        m_updateHttp.end();
        return false;
    }

    m_downloadActive = true;
    return true;
}

void OtaService::processDownload()
{
    if (!m_downloadActive)
    {
        m_otaState = OtaState::Idle; return;
    }
    if (!m_updateStream)
    { 
        failDownload("Missing stream"); return;
    }

    const auto start = millis();

    while (millis() - start < OtaConfig::Constants::kDefaultIntervalTimeDownload)
    {
        const auto now = millis();

        if (WiFi.status() != WL_CONNECTED)
        {
            failDownload("WiFi disconnected");
            return;
        }

        if (m_updateTotalSize > 0 && m_updateDownloaded >= m_updateTotalSize)
        {
            completeDownload();
            return;
        }
        if (!m_updateHttp.connected() && m_updateStream->available() == 0)
        {
            completeDownload();
            return;
        }

        if (now - m_lastDownloadActivityMs >= OtaConfig::Constants::kDefaultCheckStuckTimeMs)
        {
            failDownload("Download stalled");
            return;
        }

        const int available = m_updateStream->available();
        if (available <= 0)
        {
            yield();
            break;
        }

        const auto toRead = static_cast<std::size_t>(
            std::min<int>(available, static_cast<int>(m_downloadBuffer.size())));
        if (toRead == 0)
        {
            yield();
            break;
        }

        const auto bytesRead = m_updateStream->readBytes(m_downloadBuffer.data(), toRead);
        if (bytesRead == 0)
        {
            yield();
            break;
        }

        const auto bytesWritten = Update.write(m_downloadBuffer.data(), bytesRead);
        if (bytesWritten != bytesRead)
        {
            failDownload("Update write failed");
            return;
        }

        m_updateDownloaded += bytesWritten;
        m_lastDownloadActivityMs = now;

        if (m_updateTotalSize > 0)
        {
            const auto progress = static_cast<std::uint8_t>(
                std::min<std::uint32_t>(100, (m_updateDownloaded * 100U) / m_updateTotalSize));

            if (progress != m_progress && now - m_lastProgressPublishMs >= OtaConfig::Constants::kProgressPublishIntervalMs)
            {
                m_progress = progress;
                m_lastProgressPublishMs = now;
                m_bus.publish(EventType::OtaProgress);
            }
        }
    }
}

void OtaService::completeDownload()
{
    if (!Update.end())
    {
        LOG_ERROR(TAG, "Update failed: %u", Update.getError());
        failDownload("Update end failed");
        return;
    }

    if (m_updateTotalSize > 0 && m_updateDownloaded < m_updateTotalSize)
    {
        LOG_ERROR(TAG, "Incomplete update: %u/%u bytes", m_updateDownloaded, m_updateTotalSize);
        failDownload("Incomplete update");
        return;
    }

    LOG_INFO(TAG, "Success, rebooting...");
    m_otaState = OtaState::Completed;
    m_progress = 100;
    m_bus.publish(EventType::OtaCompleted);
    cleanupDownload();
    delay(100);
    ESP.restart();
}

void OtaService::failDownload(const char *reason)
{
    LOG_ERROR(TAG, "%s", reason);
    Update.abort();
    m_otaState = OtaState::Error;
    m_progress = 0;
    m_bus.publish(EventType::OtaError);
    cleanupDownload();
}

void OtaService::cleanupDownload()
{
    m_downloadActive = false;
    m_updateStream = nullptr;
    m_updateHttp.end();
    m_updateMd5.clear();
    m_updateTotalSize = 0;
    m_updateDownloaded = 0;
}

} // namespace isic

#endif // ISIC_ENABLE_OTA
