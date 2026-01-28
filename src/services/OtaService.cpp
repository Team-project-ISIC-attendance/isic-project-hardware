#include "services/OtaService.hpp"

#if ISIC_ENABLE_OTA

#include "common/Logger.hpp"

#include <ArduinoJson.h>
#include <Stream.h>
#include <algorithm>

namespace isic
{
namespace
{
int compareVersions(const char *v1, const char *v2)
{
    auto major1 = 0, minor1 = 0, patch1 = 0;
    auto major2 = 0, minor2 = 0, patch2 = 0;

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
        const bool firstConnect{!m_mqttConnected};
        m_mqttConnected = true;

        m_bus.publish({EventType::MqttSubscribeRequest, MqttEvent{.topic = "ota/start"}});

        if (firstConnect && m_config.checkOnConnect && m_config.isConfigured())
        {
            LOG_INFO(m_name, "First MQTT connect, scheduling OTA check");
            m_pendingCheck = true;
        }
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttDisconnected, [this](const Event &) {
        m_mqttConnected = false;

        if (m_otaState == OtaState::Downloading)
        {
            LOG_WARN(m_name, "MQTT disconnected, aborting OTA");
            failDownload("Connection lost");
        }
    }));
    m_eventConnections.push_back(m_bus.subscribeScoped(EventType::MqttMessage, [this](const Event &event) {
        if (const auto *mqtt = event.get<MqttEvent>(); mqtt && mqtt->topic.find("/ota/start") != std::string::npos)
        {
            m_pendingCheck = true;
        }
    }));
}

Status OtaService::begin()
{
    setState(ServiceState::Initializing);
    LOG_INFO(m_name, "Initializing...");

    if (!m_config.enabled)
    {
        LOG_INFO(m_name, "Disabled by config");
        setState(ServiceState::Running);
        return Status::Ok();
    }

    LOG_INFO(m_name, "Server configured: %s", m_config.isConfigured() ? "yes" : "no");
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
        LOG_INFO(m_name, "OTA check requested");
        checkForUpdate();
    }

    if (m_otaState == OtaState::Downloading)
    {
        LOG_INFO(m_name, "Processing OTA download...");
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
        LOG_WARN(m_name, "Server not configured");
        return;
    }

    if (m_otaState == OtaState::Downloading)
    {
        LOG_WARN(m_name, "Update already in progress");
        return;
    }

    if (!m_mqttConnected)
    {
        LOG_WARN(m_name, "Not connected");
        return;
    }

    LOG_INFO(m_name, "Checking for updates...");
    m_otaState = OtaState::Checking;

    std::uint32_t serverSize{0};
    std::string serverVersion{};
    std::string serverMd5{};

    serverVersion.reserve(12);
    serverMd5.reserve(32);

    if (!fetchManifest(serverVersion, serverMd5, serverSize))
    {
        LOG_ERROR(m_name, "Failed to fetch manifest");
        m_otaState = OtaState::Idle;
        return;
    }

    if (isNewerVersion(serverVersion))
    {
        LOG_INFO(m_name, "Update: %s -> %s", DeviceConfig::Constants::kFirmwareVersion, serverVersion.c_str());
        m_bus.publish({EventType::MqttPublishRequest, MqttEvent{.topic = "ota/update_available", .payload = serverVersion}});
        if (!beginDownload(serverMd5, serverSize))
        {
            failDownload("Failed to start download");
        }
    }
    else
    {
        LOG_INFO(m_name, "Up to date (v%s)", DeviceConfig::Constants::kFirmwareVersion);
        m_otaState = OtaState::Idle;
    }
}

bool OtaService::fetchManifest(std::string &outVersion, std::string &outMd5, std::uint32_t &outSize)
{
    const std::string url{m_config.serverUrl + "/manifest.json"};
    m_updateHttp.setTimeout(m_config.timeoutMs);

    if (!m_updateHttp.begin(m_updateClient, url.c_str()))
    {
        LOG_ERROR(m_name, "HTTP begin failed");
        return false;
    }

    if (!m_config.username.empty())
    {
        m_updateHttp.setAuthorization(m_config.username.c_str(), m_config.password.c_str());
    }

    if (const auto code = m_updateHttp.GET(); code != HTTP_CODE_OK)
    {
        LOG_ERROR(m_name, "HTTP %d", code);
        m_updateHttp.end();
        return false;
    }

    String payload{m_updateHttp.getString()};
    m_updateHttp.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload))
    {
        LOG_ERROR(m_name, "JSON parse failed");
        return false;
    }

    if (!doc["version"].is<const char *>())
    {
        LOG_ERROR(m_name, "Missing 'version'");
        return false;
    }

    if(!doc["board"].is<const char *>())
    {
        LOG_ERROR(m_name, "Missing 'board'");
        return false;
    }

    if (strcmp(doc["board"].as<const char *>(), platform::kBoardName) != 0)
    {
        LOG_ERROR(m_name, "Manifest board mismatch");
        return false;
    }

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

bool OtaService::beginDownload(const std::string &expectedMd5, const std::uint32_t expectedSize)
{
    std::string url{m_config.serverUrl + "/manifest.json"};
    LOG_INFO(m_name, "Starting download: %s", url.c_str());

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
        LOG_ERROR(m_name, "HTTP begin failed");
        return false;
    }

    if (!m_config.username.empty())
    {
        m_updateHttp.setAuthorization(m_config.username.c_str(), m_config.password.c_str());
    }

    if (const auto code = m_updateHttp.GET(); code != HTTP_CODE_OK)
    {
        LOG_ERROR(m_name, "HTTP %d", code);
        m_updateHttp.end();
        return false;
    }

    if (m_updateTotalSize == 0)
    {
        if (const auto reportedSize = m_updateHttp.getSize(); reportedSize > 0)
        {
            m_updateTotalSize = static_cast<std::uint32_t>(reportedSize);
        }
    }

    const auto updateSize = m_updateTotalSize > 0 ? m_updateTotalSize : platform::kUpdateSizeUnknown;
    if (!Update.begin(updateSize))
    {
        LOG_ERROR(m_name, "Update begin failed: %u", Update.getError());
        m_updateHttp.end();
        return false;
    }

    if (!m_updateMd5.empty())
    {
        Update.setMD5(m_updateMd5.c_str());
    }

    m_updateStream = m_updateHttp.getStreamPtr();
    if (m_updateStream == nullptr)
    {
        LOG_ERROR(m_name, "Stream not available");
        Update.end(false);
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

        if (!m_mqttConnected)
        {
            failDownload("Connection lost");
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

        const auto available = m_updateStream->available();
        if (available <= 0)
        {
            yield();
            break;
        }

        const auto toRead{static_cast<std::size_t>(std::min(available, static_cast<int>(m_downloadBuffer.size())))};
        if (toRead == 0)
        {
            yield();
            break;
        }

        const auto bytesRead{m_updateStream->readBytes(m_downloadBuffer.data(), toRead)};
        if (bytesRead == 0)
        {
            yield();
            break;
        }

        const auto bytesWritten{Update.write(m_downloadBuffer.data(), bytesRead)};
        if (bytesWritten != bytesRead)
        {
            failDownload("Update write failed");
            return;
        }

        m_updateDownloaded += bytesWritten;
        m_lastDownloadActivityMs = now;

        if (m_updateTotalSize > 0)
        {
            const auto progress{static_cast<std::uint8_t>(std::min<std::uint32_t>(100, (m_updateDownloaded * 100U) / m_updateTotalSize))};
            if ((progress != m_progress) && (now - m_lastProgressPublishMs >= OtaConfig::Constants::kProgressPublishIntervalMs))
            {
                m_progress = progress;
                m_lastProgressPublishMs = now;
                m_bus.publish({EventType::MqttPublishRequest, MqttEvent{.topic = "ota/progress", .payload = std::to_string(m_progress)}});
            }
        }
    }
}

void OtaService::completeDownload()
{
    if (!Update.end())
    {
        LOG_ERROR(m_name, "Update failed: %u", Update.getError());
        failDownload("Update end failed");
        return;
    }

    if (m_updateTotalSize > 0 && m_updateDownloaded < m_updateTotalSize)
    {
        LOG_ERROR(m_name, "Incomplete update: %u/%u bytes", m_updateDownloaded, m_updateTotalSize);
        failDownload("Incomplete update");
        return;
    }

    LOG_INFO(m_name, "Success, rebooting...");
    m_bus.publish({EventType::MqttPublishRequest, MqttEvent{.topic = "ota/completed", .payload = "success"}});
    m_otaState = OtaState::Completed;
    m_progress = 100;
    cleanupDownload();
    delay(100);
    ESP.restart();
}

void OtaService::failDownload(const char *reason)
{
    LOG_ERROR(m_name, "%s", reason);
    Update.end(false);
    m_otaState = OtaState::Error;
    m_progress = 0;
    m_bus.publish({EventType::MqttPublishRequest, MqttEvent{.topic = "ota/error", .payload = std::string("error: ") + reason}});
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
