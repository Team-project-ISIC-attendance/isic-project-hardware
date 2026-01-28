#ifndef ISIC_SERVICES_OTASERVICE_HPP
#define ISIC_SERVICES_OTASERVICE_HPP

#include "core/EventBus.hpp"
#include "core/IService.hpp"
#include "common/Config.hpp"
#include "platform/PlatformOta.hpp"

#include <array>
#include <vector>

///< Forward declaration of Stream class
class Stream;

namespace isic
{
class OtaService : public ServiceBase
{
public:
    OtaService(EventBus &bus, const OtaConfig &config);
    ~OtaService() override = default;

    OtaService(const OtaService &) = delete;
    OtaService &operator=(const OtaService &) = delete;
    OtaService(OtaService &&) = delete;
    OtaService &operator=(OtaService &&) = delete;

    Status begin() override;
    void loop() override;
    void end() override;

    void checkForUpdate();

    [[nodiscard]] OtaState getOtaState() const { return m_otaState; }
    [[nodiscard]] bool isUpdating() const { return m_otaState == OtaState::Downloading; }
    [[nodiscard]] std::uint8_t getProgress() const { return m_progress; }

    void serializeMetrics(JsonObject &obj) const override
    {
        obj["state"] = toString(getState());
        obj["otaState"] = static_cast<int>(m_otaState);
        obj["progress"] = m_progress;
        obj["serverConfigured"] = m_config.isConfigured();
    }

private:
    bool fetchManifest(std::string &outVersion, std::string &outMd5, std::uint32_t &outSize);
    bool isNewerVersion(const std::string &serverVersion) const;
    bool beginDownload(const std::string &expectedMd5, std::uint32_t expectedSize);
    void processDownload();
    void completeDownload();
    void failDownload(const char *reason);
    void cleanupDownload();

    EventBus &m_bus;
    const OtaConfig &m_config;

    OtaState m_otaState{OtaState::Idle};
    std::uint8_t m_progress{0};
    bool m_pendingCheck{false};
    bool m_mqttConnected{false};
    bool m_downloadActive{false};

    std::uint32_t m_updateTotalSize{0};
    std::uint32_t m_updateDownloaded{0};
    std::uint32_t m_lastDownloadActivityMs{0};
    std::uint32_t m_lastProgressPublishMs{0};

    HTTPClient m_updateHttp;
    WiFiClient m_updateClient;
    Stream *m_updateStream{nullptr};
    std::string m_updateMd5{};

    static constexpr std::size_t kDownloadBufferSize{1024};
    std::array<std::uint8_t, kDownloadBufferSize> m_downloadBuffer{};

    std::vector<EventBus::ScopedConnection> m_eventConnections{};
};

} // namespace isic

#endif // ISIC_SERVICES_OTASERVICE_HPP
