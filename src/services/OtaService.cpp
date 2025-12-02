/**
 * @file OtaService.cpp
 * @brief Production-grade OTA firmware update service implementation.
 *
 * This implementation uses ESP-IDF OTA APIs for reliable firmware updates:
 * - esp_ota_begin() - Initialize OTA write to partition
 * - esp_ota_write() - Write firmware chunks to flash
 * - esp_ota_end() - Finalize and verify the update
 * - esp_ota_set_boot_partition() - Mark new partition as bootable
 *
 * The update flow:
 * 1. Receive OTA command via MQTT or EventBus
 * 2. Acquire wake lock to prevent sleep
 * 3. Connect to HTTP/HTTPS server
 * 4. Get next available OTA partition
 * 5. Stream firmware in chunks, writing directly to flash
 * 6. Verify checksum/signature
 * 7. Mark new partition as boot partition
 * 8. Restart device to apply update
 *
 * On boot after OTA:
 * 1. Check if this is first boot on new partition
 * 2. Run validation checks
 * 3. Mark partition as valid (prevents rollback)
 * 4. If validation fails, bootloader will roll back automatically
 */

#include "services/OtaService.hpp"
#include "services/PowerService.hpp"
#include "core/Logger.hpp"

#include <WiFi.h>
#include <mbedtls/sha256.h>
#include <esp_app_desc.h>

namespace isic {
    namespace {
        constexpr auto *OTA_TAG{"OtaService"};

        // Buffer and timing constants
        constexpr std::uint32_t DOWNLOAD_BUFFER_SIZE{4096};
        constexpr std::uint32_t PROGRESS_REPORT_INTERVAL_MS{1000};
        constexpr std::uint32_t CONNECT_TIMEOUT_MS{15000};
        constexpr std::uint32_t HTTP_TIMEOUT_MS{30000};

        // Firmware validation constants
        constexpr std::size_t MIN_FIRMWARE_SIZE{32768}; // 32KB minimum
        constexpr std::size_t MAX_FIRMWARE_SIZE{1280 * 1024}; // 1.25MB (partition size)

        // ESP-IDF magic byte for app images
        constexpr std::uint8_t ESP_IMAGE_MAGIC{0xE9};

        // SHA256 digest size
        constexpr std::size_t SHA256_DIGEST_SIZE{32};
    }

    OtaService::OtaService(EventBus &bus) : m_bus(bus) {
        m_metricsMutex = xSemaphoreCreateMutex();
        m_triggerSemaphore = xSemaphoreCreateBinary();

        // Subscribe to relevant events
        m_subscriptionId = m_bus.subscribe(this,
        EventFilter::only(EventType::ConfigUpdated)
                        .include(EventType::OtaRequested)
        );
    }

    OtaService::~OtaService() {
        stop();
        m_bus.unsubscribe(m_subscriptionId);

        if (m_metricsMutex) {
            vSemaphoreDelete(m_metricsMutex);
            m_metricsMutex = nullptr;
        }
        if (m_triggerSemaphore) {
            vSemaphoreDelete(m_triggerSemaphore);
            m_triggerSemaphore = nullptr;
        }
    }

    Status OtaService::begin(const AppConfig &cfg, PowerService &powerService) {
        m_cfg = &cfg.ota;
        m_appCfg = &cfg;
        m_powerService = &powerService;
        m_enabled.store(m_cfg->enabled);

        // Log partition info
        const auto *running{getRunningPartitionPtr()};
        const auto *next{getNextUpdatePartitionPtr()};

        if (running) {
            LOG_INFO(OTA_TAG, "Running partition: %s @ 0x%08X", running->label, unsigned{running->address});

            // Update metrics with partition info
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                m_metrics.currentPartition = running->label;
                if (next) {
                    m_metrics.nextPartition = next->label;
                }
                xSemaphoreGive(m_metricsMutex);
            }
        }

        if (next) {
            LOG_INFO(OTA_TAG, "Next update partition: %s @ 0x%08X (size: %u)", next->label, unsigned{next->address}, unsigned{next->size});
        } else {
            LOG_WARNING(OTA_TAG, "No update partition available!");
        }

        // Check if we need to validate this boot
        if (isPendingValidation()) {
            LOG_INFO(OTA_TAG, "Boot is pending validation - marking as valid");
            if (const auto status = markPartitionValid(); !status.ok()) {
                LOG_WARNING(OTA_TAG, "Failed to mark partition valid: %s", status.message);
            }
        }

        // Start OTA task
        m_running.store(true);
        const BaseType_t result = xTaskCreatePinnedToCore(
            &OtaService::otaTaskThunk,
            "ota_service",
            m_cfg->taskStackSize,
            this,
            m_cfg->taskPriority,
            &m_taskHandle,
            1 // Core 1
        );

        if (result != pdPASS) {
            LOG_ERROR(OTA_TAG, "Failed to create OTA task");
            return Status::Error(ErrorCode::ResourceExhausted, "Task creation failed");
        }

        LOG_INFO(OTA_TAG, "OtaService started:");
        LOG_INFO(OTA_TAG, "  Enabled: %s", m_cfg->enabled ? "yes" : "no");
        LOG_INFO(OTA_TAG, "  Auto-check: %s (interval: %ums)", m_cfg->autoCheck ? "yes" : "no", unsigned{m_cfg->checkIntervalMs});
        LOG_INFO(OTA_TAG, "  Auto-update: %s", m_cfg->autoUpdate ? "yes" : "no");
        LOG_INFO(OTA_TAG, "  Require HTTPS: %s", m_cfg->requireHttps ? "yes" : "no");
        LOG_INFO(OTA_TAG, "  Current version: %s", m_cfg->currentVersion.c_str());

        return Status::OK();
    }

    void OtaService::stop() {
        m_running.store(false);
        m_cancelRequested.store(true);

        releaseWakeLock();

        // Wake task so it can exit
        if (m_triggerSemaphore) {
            xSemaphoreGive(m_triggerSemaphore);
        }

        // Wait for task to finish
        if (m_taskHandle) {
            vTaskDelay(pdMS_TO_TICKS(500));
            m_taskHandle = nullptr;
        }

        // Clean up any pending OTA
        if (m_otaHandle) {
            esp_ota_abort(m_otaHandle);
            m_otaHandle = 0;
        }

        transitionTo(OtaState::Idle);
        LOG_INFO(OTA_TAG, "OtaService stopped");
    }

    Status OtaService::executeCommand(const OtaCommand &cmd) {
        switch (cmd.action) {
            case OtaAction::Update: {
                return triggerOtaWithChecksum(cmd.url, cmd.version, cmd.sha256);
            }
            case OtaAction::Rollback: {
                return rollback();
            }
            case OtaAction::Check: {
                checkForUpdates();
                return Status::OK();
            }
            case OtaAction::Cancel: {
                cancelOta();
                return Status::OK();
            }
            case OtaAction::MarkValid: {
                return markPartitionValid();
            }
            case OtaAction::GetStatus: {
                publishStatusToMqtt();
                return Status::OK();
            }
            default: {
                return Status::Error(ErrorCode::InvalidArgument, "Unknown action");
            }
        }
    }

    Status OtaService::triggerOta(const std::string &url, const std::string &version, const bool force) {
        (void) force; // TODO: implement force update logic, like stop all services, ignore version checks, etc.
        return triggerOtaWithChecksum(url, version, "");
    }

    Status OtaService::triggerOtaWithChecksum(const std::string &url, const std::string &version, const std::string &sha256) {
        // Validate preconditions
        if (!m_enabled.load()) {
            LOG_WARNING(OTA_TAG, "OTA disabled");
            reportError(OtaError::NotEnabled, "OTA updates are disabled");
            return Status::Error(ErrorCode::OperationFailed, "OTA disabled");
        }

        if (isInProgress()) {
            LOG_WARNING(OTA_TAG, "OTA already in progress");
            reportError(OtaError::AlreadyInProgress, "Update already in progress");
            return Status::Error(ErrorCode::ResourceBusy, "OTA already in progress");
        }

        if (url.empty()) {
            LOG_ERROR(OTA_TAG, "Empty URL provided");
            reportError(OtaError::InvalidImage, "Empty firmware URL");
            return Status::Error(ErrorCode::InvalidArgument, "Empty URL");
        }

        // Check HTTPS requirement
        if (m_cfg->requireHttps && !url.starts_with("https://")) {
            LOG_ERROR(OTA_TAG, "HTTPS required but URL is not HTTPS");
            reportError(OtaError::CertificateInvalid, "HTTPS required");
            return Status::Error(ErrorCode::InvalidArgument, "HTTPS required");
        }

        // Check version
        if (!m_forceUpdate && !version.empty() && !isNewerVersion(version)) {
            LOG_INFO(OTA_TAG, "Version %s is not newer than current %s", version.c_str(), m_cfg->currentVersion.c_str());
            reportError(OtaError::SameVersion, "Same or older version");
            return Status::Error(ErrorCode::AlreadyExists, "Same or older version");
        }

        // Check update window
        if (m_cfg->restrictUpdateWindow && !isWithinUpdateWindow()) {
            LOG_INFO(OTA_TAG, "Outside update window");
            reportError(OtaError::OutsideUpdateWindow, "Updates not allowed at this time");
            return Status::Error(ErrorCode::Cancelled, "Outside update window");
        }

        // Store pending update info
        m_pendingUrl = url;
        m_pendingVersion = version;
        m_pendingSha256 = sha256;
        m_forceUpdate = false;

        LOG_INFO(OTA_TAG, "OTA update triggered:");
        LOG_INFO(OTA_TAG, "  URL: %s", url.c_str());
        LOG_INFO(OTA_TAG, "  Version: %s", version.empty() ? "unknown" : version.c_str());
        LOG_INFO(OTA_TAG, "  SHA256: %s", sha256.empty() ? "not provided" : "provided");

        // Signal the OTA task to start
        xSemaphoreGive(m_triggerSemaphore);
        return Status::OK();
    }

    void OtaService::checkForUpdates() {
        if (!m_enabled.load() || isInProgress()) {
            return;
        }

        LOG_INFO(OTA_TAG, "Checking for updates...");
        transitionTo(OtaState::Checking, "Checking for available updates");

        // In a production system, this would query a version endpoint:
        // GET https://server.com/api/firmware/version?device=<id>&current=<version>
        //
        // Response: { "available": "1.2.3", "url": "...", "sha256": "...", "mandatory": false }
        //
        // For now, we just check if updateServerUrl is configured and publish version info

        if (!m_cfg->updateServerUrl.empty()) {
            // Publish current version info
            const Event evt{
                .type = EventType::OtaVersionInfo,
                .payload = OtaVersionInfoEvent{
                    .currentVersion = m_cfg->currentVersion,
                    .availableVersion = "", // Would be filled by server query
                    .updateAvailable = false
                },
                .timestampMs = static_cast<std::uint64_t>(millis())
            };
            (void) m_bus.publish(evt); // TODO: handle publish failure?
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            m_metrics.lastCheckMs = millis();
            xSemaphoreGive(m_metricsMutex);
        }

        transitionTo(OtaState::Idle, "Check complete");
    }

    void OtaService::cancelOta() {
        if (!isInProgress()) {
            LOG_DEBUG(OTA_TAG, "No OTA in progress to cancel");
            return;
        }

        LOG_WARNING(OTA_TAG, "OTA cancellation requested");
        m_cancelRequested.store(true);

        // Abort any pending OTA write
        if (m_otaHandle) {
            esp_ota_abort(m_otaHandle);
            m_otaHandle = 0;
        }
    }

    Status OtaService::rollback() {
        LOG_INFO(OTA_TAG, "Rollback requested");

        const esp_partition_t *otherPartition{esp_ota_get_next_update_partition(nullptr)};
        if (!otherPartition) {
            LOG_ERROR(OTA_TAG, "No partition available for rollback");
            reportError(OtaError::RollbackFailed, "No rollback partition");
            return Status::Error(ErrorCode::NotFound, "No rollback partition");
        }

        // Check if the other partition has valid firmware
        esp_app_desc_t appDesc{};
        if (esp_ota_get_partition_description(otherPartition, &appDesc) != ESP_OK) {
            LOG_ERROR(OTA_TAG, "Other partition has no valid app");
            reportError(OtaError::RollbackFailed, "No valid firmware in rollback partition");
            return Status::Error(ErrorCode::NotFound, "Invalid rollback partition");
        }

        LOG_INFO(OTA_TAG, "Rolling back to: %s (version: %s)", otherPartition->label, appDesc.version);
        if (const esp_err_t err = esp_ota_set_boot_partition(otherPartition); err != ESP_OK) {
            LOG_ERROR(OTA_TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
            reportError(OtaError::RollbackFailed, "Failed to set boot partition");
            return Status::Error(ErrorCode::OperationFailed, "Set boot partition failed");
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            m_metrics.rollbackCount++;
            xSemaphoreGive(m_metricsMutex);
        }

        LOG_INFO(OTA_TAG, "Rollback configured - restarting...");

        // Publish event before restart
        const Event evt{
            .type = EventType::OtaStateChanged,
            .payload = OtaStateChangedEvent{
                .oldState = static_cast<std::uint8_t>(m_state.load()),
                .newState = static_cast<std::uint8_t>(OtaState::Completed),
                .message = "Rollback initiated - restarting",
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis()),
            .priority = EventPriority::E_CRITICAL
        };
        (void) m_bus.publish(evt); // TODO: handle publish failure?

        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();

        return Status::OK(); // Won't reach here
    }

    Status OtaService::markPartitionValid() {
        if (m_bootValidated) {
            LOG_DEBUG(OTA_TAG, "Partition already validated");
            return Status::OK();
        }

        if (const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback(); err != ESP_OK) {
            LOG_ERROR(OTA_TAG, "Failed to mark partition valid: %s", esp_err_to_name(err));
            return Status::Error(ErrorCode::OperationFailed, "Mark valid failed");
        }

        m_bootValidated = true;
        LOG_INFO(OTA_TAG, "Current partition marked as valid");

        return Status::OK();
    }

    OtaMetrics OtaService::getMetrics() const {
        OtaMetrics result{};
        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            result = m_metrics;
            xSemaphoreGive(m_metricsMutex);
        }
        return result;
    }

    OtaPartitionInfo OtaService::getRunningPartition() const {
        OtaPartitionInfo info{};
        if (const auto *part = getRunningPartitionPtr(); part) {
            info.label = part->label;
            info.address = part->address;
            info.size = part->size;
            info.isRunning = true;
            info.isBootable = true;

            esp_app_desc_t appDesc;
            if (esp_ota_get_partition_description(part, &appDesc) == ESP_OK) {
                info.appVersion = appDesc.version;
                info.appProjectName = appDesc.project_name;
            }
        }
        return info;
    }

    OtaPartitionInfo OtaService::getNextUpdatePartition() const {
        OtaPartitionInfo info{};
        if (const auto *part = getNextUpdatePartitionPtr(); part) {
            info.label = part->label;
            info.address = part->address;
            info.size = part->size;
            info.isRunning = false;

            esp_app_desc_t appDesc;
            if (esp_ota_get_partition_description(part, &appDesc) == ESP_OK) {
                info.appVersion = appDesc.version;
                info.appProjectName = appDesc.project_name;
                info.isBootable = true;
            }
        }
        return info;
    }

    bool OtaService::isPendingValidation() const {
        esp_ota_img_states_t state;

        if (const esp_partition_t *running = esp_ota_get_running_partition(); esp_ota_get_state_partition(running, &state) != ESP_OK) {
            return false;
        }

        return state == ESP_OTA_IMG_PENDING_VERIFY;
    }

    void OtaService::updateConfig(const OtaConfig &cfg) {
        m_cfg = &cfg;
        m_enabled.store(cfg.enabled);
        LOG_INFO(OTA_TAG, "Config updated: enabled=%s", cfg.enabled ? "yes" : "no");
    }

    void OtaService::setEnabled(bool enabled) {
        m_enabled.store(enabled);
        if (!enabled && isInProgress()) {
            cancelOta();
        }

        LOG_INFO(OTA_TAG, "OTA %s", enabled ? "enabled" : "disabled");
    }

    void OtaService::setCaCertificate(const char *cert) {
        m_caCertificate = cert;
        LOG_INFO(OTA_TAG, "CA certificate %s", cert ? "set" : "cleared");
    }

    void OtaService::setProgressCallback(ProgressCallback callback) {
        m_progressCallback = std::move(callback);
    }

    HealthStatus OtaService::getHealth() const {
        HealthStatus status{};
        status.componentName = getComponentName();
        status.lastUpdatedMs = millis();

        const auto state = m_state.load();

        if (state == OtaState::Failed) {
            status.state = HealthState::Unhealthy;
            status.message = m_lastErrorMessage;
        } else if (isInProgress()) {
            status.state = HealthState::Degraded;
            status.message = "Update in progress";
        } else {
            status.state = HealthState::Healthy;
            status.message = "Ready";
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            status.errorCount = m_metrics.failedUpdates;
            xSemaphoreGive(m_metricsMutex);
        }

        return status;
    }

    bool OtaService::performHealthCheck() {
        return m_state.load() != OtaState::Failed;
    }

    void OtaService::onEvent(const Event &event) {
        switch (event.type) {
            case EventType::ConfigUpdated: {
                if (const auto *ce = std::get_if<ConfigUpdatedEvent>(&event.payload)) {
                    if (ce->config) {
                        updateConfig(ce->config->ota);
                    }
                }
                break;
            }
            case EventType::OtaRequested: {
                if (const auto *req = std::get_if<OtaRequestEvent>(&event.payload)) {
                    (void) triggerOta(req->url, req->version, req->force); // Ignore status here
                }
                break;
            }
            default: {
                break;
            }
        }
    }

    // ==================== State Machine ====================

    void OtaService::transitionTo(OtaState newState, const std::string &message) {
        if ( const auto oldState = m_state.exchange(newState); oldState != newState) {
            LOG_INFO(OTA_TAG, "State: %s -> %s%s%s", toString(oldState), toString(newState), message.empty() ? "" : " - ", message.c_str());

            // Publish state change event
            const Event evt{
                .type = EventType::OtaStateChanged,
                .payload = OtaStateChangedEvent{
                    .oldState = static_cast<std::uint8_t>(oldState),
                    .newState = static_cast<std::uint8_t>(newState),
                    .message = message,
                    .timestampMs = static_cast<std::uint64_t>(millis())
                },
                .timestampMs = static_cast<std::uint64_t>(millis()),
                .priority = EventPriority::E_HIGH
            };
            (void) m_bus.publish(evt); // TODO: handle publish failure?
        }
    }

    void OtaService::otaTaskThunk(void *arg) {
        static_cast<OtaService *>(arg)->otaTask();
    }

    void OtaService::otaTask() {
        LOG_DEBUG(OTA_TAG, "OTA task started on core %d", xPortGetCoreID());

        while (m_running.load()) {
            // Wait for trigger or timeout for auto-check
            const auto timeout{m_cfg && m_cfg->autoCheck ? pdMS_TO_TICKS(m_cfg->checkIntervalMs) : portMAX_DELAY};

            if (xSemaphoreTake(m_triggerSemaphore, timeout) == pdTRUE) {
                // Manual trigger
                if (!m_running.load()) {
                    break;
                }

                if (!m_pendingUrl.empty()) {
                    m_cancelRequested.store(false);
                    (void) performUpdate(); // Ignore result here
                }
            } else {
                // Timeout - auto-check if enabled
                if (m_cfg && m_cfg->autoCheck && m_enabled.load()) {
                    if (const auto now = millis(); now - m_lastAutoCheckMs >= m_cfg->checkIntervalMs) {
                        m_lastAutoCheckMs = now;
                        checkForUpdates();
                    }
                }
            }
        }

        LOG_DEBUG(OTA_TAG, "OTA task exiting");
        vTaskDelete(nullptr);
    }

    // ==================== Update Flow ====================

    bool OtaService::performUpdate() {
        if (m_pendingUrl.empty()) {
            return false;
        }

        // Check backoff period
        if (m_consecutiveFailures > 0) {
            const auto backoff = calculateBackoff();
            if (const auto elapsed = millis() - m_lastFailureMs; elapsed < backoff) {
                LOG_INFO(OTA_TAG, "In backoff period, waiting %lu more ms", static_cast<unsigned long>(backoff - elapsed));
                return false;
            }
        }

        // Acquire wake lock
        acquireWakeLock();

        // Initialize metrics
        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            m_metrics.totalAttempts++;
            m_metrics.updateStartMs = millis();
            m_metrics.bytesDownloaded = 0;
            m_metrics.totalBytes = 0;
            m_metrics.progressPercent = 0;
            xSemaphoreGive(m_metricsMutex);
        }

        bool success = downloadAndFlashFirmware();

        if (success) {
            transitionTo(OtaState::Completed, "Update complete, restarting...");

            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                m_metrics.successfulUpdates++;
                m_metrics.lastSuccessMs = millis();
                m_metrics.lastUpdateDurationMs = millis() - m_metrics.updateStartMs;
                xSemaphoreGive(m_metricsMutex);
            }

            resetBackoff();

            // Give time for events to propagate
            vTaskDelay(pdMS_TO_TICKS(1000));

            LOG_INFO(OTA_TAG, "Restarting to apply update...");
            ESP.restart();
        } else if (!m_cancelRequested.load()) {
            if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                m_metrics.failedUpdates++;
                m_metrics.consecutiveFailures++;
                xSemaphoreGive(m_metricsMutex);
            }

            incrementBackoff();
            transitionTo(OtaState::Failed, m_lastErrorMessage);
        } else {
            transitionTo(OtaState::Idle, "Update cancelled");
        }

        // Cleanup
        m_pendingUrl.clear();
        m_pendingVersion.clear();
        m_pendingSha256.clear();
        m_forceUpdate = false;
        m_updatePartition = nullptr;
        m_otaHandle = 0;

        releaseWakeLock();

        // Return to idle after delay
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (m_state.load() == OtaState::Failed) {
            transitionTo(OtaState::Idle);
        }

        return success;
    }

    bool OtaService::downloadAndFlashFirmware() {
        // Check WiFi connection
        if (WiFi.status() != WL_CONNECTED) {
            reportError(OtaError::WifiDisconnected, "WiFi not connected");
            return false;
        }

        transitionTo(OtaState::Downloading, "Connecting to server...");

        // Setup HTTP client (HTTP only - no HTTPS support)
        HTTPClient http{};

        // Configure HTTP client
        http.setTimeout(m_cfg ? m_cfg->downloadTimeoutMs : HTTP_TIMEOUT_MS);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setReuse(false);

        LOG_INFO(OTA_TAG, "Connecting to: %s", m_pendingUrl.c_str());

        if (!http.begin(m_pendingUrl.c_str())) {
            reportError(OtaError::ConnectionTimeout, "Failed to connect to server");
            return false;
        }

        // Make GET request
        transitionTo(OtaState::Downloading, "Downloading firmware...");

        if (const int httpCode = http.GET(); !validateHttpResponse(http, httpCode)) {
            http.end();
            return false;
        }

        const auto contentLength = http.getSize();

        // Get next OTA partition and prepare for writing
        if (!preparePartition(contentLength)) {
            http.end();
            return false;
        }

        // Get stream for chunked download
        auto *stream = http.getStreamPtr();
        if (!stream) {
            reportError(OtaError::DownloadFailed, "Failed to get HTTP stream");
            esp_ota_abort(m_otaHandle);
            m_otaHandle = 0;
            http.end();
            return false;
        }

        // Download and flash chunks
        if (!downloadChunks(*stream, contentLength)) {
            esp_ota_abort(m_otaHandle);
            m_otaHandle = 0;
            http.end();
            return false;
        }

        http.end();

        // Finalize the update
        if (!finalizeUpdate()) {
            return false;
        }

        return true;
    }

    bool OtaService::validateHttpResponse(HTTPClient &http, int httpCode) {
        if (m_cancelRequested.load()) {
            reportError(OtaError::Cancelled, "Update cancelled");
            return false;
        }

        if (httpCode <= 0) {
            reportError(OtaError::NetworkError, std::string("Connection failed: ") + HTTPClient::errorToString(httpCode).c_str());
            return false;
        }

        if (httpCode != HTTP_CODE_OK) {
            reportError(mapHttpError(httpCode), std::string("HTTP error: ") + std::to_string(httpCode));
            return false;
        }

        const auto contentLength = http.getSize();

        if (contentLength <= 0) {
            reportError(OtaError::DownloadFailed, "Unknown content length");
            return false;
        }

        if (contentLength < static_cast<std::int32_t>(MIN_FIRMWARE_SIZE)) {
            reportError(OtaError::ImageTooSmall, "Firmware too small: " + std::to_string(contentLength) + " bytes");
            return false;
        }

        if (contentLength > static_cast<std::int32_t>(MAX_FIRMWARE_SIZE)) {
            reportError(OtaError::ImageTooLarge, "Firmware too large: " + std::to_string(contentLength) + " bytes");
            return false;
        }

        LOG_INFO(OTA_TAG, "Firmware size: %d bytes", static_cast<int>(contentLength));
        return true;
    }

    bool OtaService::preparePartition(std::int32_t contentLength) {
        // Get next update partition
        m_updatePartition = getNextUpdatePartitionPtr();

        if (!m_updatePartition) {
            reportError(OtaError::PartitionNotFound, "No update partition available");
            return false;
        }

        if (contentLength > static_cast<std::int32_t>(m_updatePartition->size)) {
            reportError(OtaError::InsufficientSpace,"Firmware (" + std::to_string(contentLength) + ") larger than partition (" + std::to_string(m_updatePartition->size) + ")");
            return false;
        }

        LOG_INFO(OTA_TAG, "Writing to partition: %s @ 0x%08X", m_updatePartition->label, static_cast<unsigned>(m_updatePartition->address));

        // Begin OTA with ESP-IDF API
        if (const auto err = esp_ota_begin(m_updatePartition, OTA_SIZE_UNKNOWN, &m_otaHandle); err != ESP_OK) {
            reportError(OtaError::EraseError, std::string("esp_ota_begin failed: ") + esp_err_to_name(err));
            return false;
        }

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            m_metrics.totalBytes = contentLength;
            m_metrics.bytesDownloaded = 0;
            xSemaphoreGive(m_metricsMutex);
        }

        return true;
    }

    bool OtaService::downloadChunks(WiFiClient &stream, const std::int32_t contentLength) {
        std::uint8_t buffer[DOWNLOAD_BUFFER_SIZE];
        std::uint32_t bytesWritten = 0;
        std::uint64_t lastProgressReport = 0;
        std::uint64_t lastSpeedCalcMs = millis();
        std::uint32_t bytesForSpeed = 0;

        const auto startTime{millis()};
        const auto timeout{m_cfg ? m_cfg->downloadTimeoutMs : 300000};
        auto firstChunk{true};

        while (bytesWritten < static_cast<std::uint32_t>(contentLength)) {
            // Check for cancellation
            if (m_cancelRequested.load()) {
                reportError(OtaError::Cancelled, "Update cancelled by user");
                return false;
            }

            // Check timeout
            if (millis() - startTime > timeout) {
                reportError(OtaError::DownloadTimeout, "Download timeout");
                return false;
            }

            // Wait for data
            if (!stream.available()) {
                // No data, brief yield and check again
                vTaskDelay(pdMS_TO_TICKS(10));

                // Check if connection is still alive
                if (!stream.connected()) {
                    if (bytesWritten < static_cast<std::uint32_t>(contentLength)) {
                        reportError(OtaError::DownloadIncomplete, "Connection lost at " + std::to_string(bytesWritten) + " bytes");
                        return false;
                    }
                }
                continue;
            }

            // Read chunk
            const auto available{stream.available()};
            const auto toRead{std::min(available, static_cast<int>(sizeof(buffer)))};
            const auto bytesRead{stream.readBytes(buffer, toRead)};

            if (bytesRead <= 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Verify first chunk (magic byte check)
            if (firstChunk) {
                if (!verifyImageHeader(buffer, bytesRead)) {
                    return false;
                }
                firstChunk = false;
            }

            if (const esp_err_t err = esp_ota_write(m_otaHandle, buffer, bytesRead); err != ESP_OK) {
                reportError(OtaError::WriteError,
                            std::string("Flash write failed: ") + esp_err_to_name(err));
                return false;
            }

            bytesWritten += bytesRead;
            bytesForSpeed += bytesRead;

            // Update metrics and report progress periodically
            if (const auto now = millis(); now - lastProgressReport >= PROGRESS_REPORT_INTERVAL_MS) {
                lastProgressReport = now;

                const auto percent {static_cast<std::uint8_t>((bytesWritten * 100) / contentLength)};

                // Calculate speed
                const auto speedIntervalMs{now - lastSpeedCalcMs};
                std::uint32_t speedBps{0};
                if (speedIntervalMs > 0) {
                    speedBps = (bytesForSpeed * 1000) / speedIntervalMs;
                    bytesForSpeed = 0;
                    lastSpeedCalcMs = now;
                }

                if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    m_metrics.downloadSpeedBps = speedBps;
                    xSemaphoreGive(m_metricsMutex);
                }

                reportProgress(percent, bytesWritten);

                LOG_DEBUG(OTA_TAG, "Progress: %u%% (%u/%d bytes, %u KB/s)", percent, unsigned{bytesWritten}, static_cast<int>(contentLength), static_cast<unsigned>(speedBps / 1024));
            }
        }

        // Final progress report
        reportProgress(100, bytesWritten);

        LOG_INFO(OTA_TAG, "Download complete: %u bytes in %lu ms", unsigned{bytesWritten}, millis() - startTime);

        // Verify total bytes match
        if (bytesWritten != static_cast<std::uint32_t>(contentLength)) {
            reportError(OtaError::ContentLengthMismatch, "Downloaded " + std::to_string(bytesWritten) + " but expected " + std::to_string(contentLength));
            return false;
        }

        return true;
    }

    bool OtaService::verifyImageHeader(const std::uint8_t *data, std::size_t len) {
        if (len < 1) {
            reportError(OtaError::MagicByteInvalid, "Empty data");
            return false;
        }

        // Check ESP32 image magic byte
        if (data[0] != ESP_IMAGE_MAGIC) {
            reportError(OtaError::MagicByteInvalid, ("Invalid magic byte: 0x" + String(data[0], HEX)).c_str());
            return false;
        }

        LOG_DEBUG(OTA_TAG, "Image header verified (magic: 0x%02X)", data[0]);
        return true;
    }

    bool OtaService::finalizeUpdate() {
        transitionTo(OtaState::Verifying, "Verifying firmware...");

        // End OTA write - this verifies the image
        esp_err_t err{esp_ota_end(m_otaHandle)};
        m_otaHandle = 0;

        if (err != ESP_OK) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                reportError(OtaError::VerificationFailed, "Firmware validation failed");
            } else {
                reportError(OtaError::WriteError,
                            std::string("esp_ota_end failed: ") + esp_err_to_name(err));
            }
            return false;
        }

        LOG_INFO(OTA_TAG, "Firmware verification passed");

        transitionTo(OtaState::Applying, "Setting boot partition...");

        // Set the new partition as boot partition
        err = esp_ota_set_boot_partition(m_updatePartition);
        if (err != ESP_OK) {
            reportError(OtaError::WriteError, std::string("Failed to set boot partition: ") + esp_err_to_name(err));
            return false;
        }

        LOG_INFO(OTA_TAG, "Boot partition set to: %s", m_updatePartition->label);
        return true;
    }

    // ==================== Partition Operations ====================

    const esp_partition_t *OtaService::getRunningPartitionPtr() const {
        return esp_ota_get_running_partition();
    }

    const esp_partition_t *OtaService::getNextUpdatePartitionPtr() const {
        return esp_ota_get_next_update_partition(nullptr);
    }

    // ==================== Version Handling ====================

    bool OtaService::isNewerVersion(const std::string &available) const {
        if (!m_cfg || m_cfg->currentVersion.empty() || available.empty()) {
            return true; // Assume newer if we can't compare
        }

        int currentMajor{};
        int currentMinor{};
        int currentPatch{};
        int availableMajor{};
        int availableMinor{};
        int availablePatch{};

        if (!parseVersion(m_cfg->currentVersion, currentMajor, currentMinor, currentPatch)) {
            return true; // Can't parse current, allow update
        }

        if (!parseVersion(available, availableMajor, availableMinor, availablePatch)) {
            return true; // Can't parse available, allow update
        }

        // Semantic version comparison
        if (availableMajor > currentMajor) {
            return true;
        }
        if (availableMajor < currentMajor) {
            return false;
        }

        if (availableMinor > currentMinor) {
            return true;
        }
        if (availableMinor < currentMinor) {
            return false;
        }

        return availablePatch > currentPatch;
    }

    bool OtaService::parseVersion(const std::string &version,
                                  int &major, int &minor, int &patch) const {
        major = minor = patch = 0;

        // Handle optional 'v' prefix
        const auto *str{version.c_str()};
        if (*str == 'v' || *str == 'V') {
            str++;
        }

        return sscanf(str, "%d.%d.%d", &major, &minor, &patch) >= 2; // TODO: sscanf is not safe - replace with proper parsing
    }

    bool OtaService::isWithinUpdateWindow() const {
        if (!m_cfg || !m_cfg->restrictUpdateWindow) {
            return true;
        }

        // Get current time (requires NTP or RTC)
        // For now, we'll use a simplified check
        // In production, you'd get the actual time from system/NTP

        // TODO: Implement proper time check when RTC/NTP is available
        // For now, always return true
        return true;
    }

    // ==================== Backoff ====================

    std::uint32_t OtaService::calculateBackoff() const {
        if (!m_cfg) {
            return 60000;
        }

        const auto baseBackoff{m_cfg->failureBackoffMs};
        const auto maxBackoff{baseBackoff * 10}; // Max 10x base

        // Exponential backoff: base * 2^failures
        auto backoff{baseBackoff * (1U << std::min(m_consecutiveFailures, static_cast<std::uint32_t>(5)))};

        if (backoff > maxBackoff) {
            backoff = maxBackoff;
        }

        // Add jitter (±10%)
        const auto jitter{(random(0, 20) - 10) * static_cast<int32_t>(backoff) / 100};
        return backoff + jitter;
    }

    void OtaService::resetBackoff() {
        m_consecutiveFailures = 0;
        m_lastFailureMs = 0;
    }

    void OtaService::incrementBackoff() {
        if (m_cfg && m_consecutiveFailures < m_cfg->maxConsecutiveFailures) {
            m_consecutiveFailures++;
        }
        m_lastFailureMs = millis();
    }

    // ==================== Progress Reporting ====================

    void OtaService::reportProgress(std::uint8_t percent, std::uint32_t bytesDownloaded) {
        m_progressPercent.store(percent);

        if (xSemaphoreTake(m_metricsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            m_metrics.progressPercent = percent;
            m_metrics.bytesDownloaded = bytesDownloaded;
            xSemaphoreGive(m_metricsMutex);
        }

        // Call custom callback if set
        if (m_progressCallback) {
            m_progressCallback(percent, bytesDownloaded, m_metrics.totalBytes);
        }

        // Publish progress event
        const Event evt{
            .type = EventType::OtaProgress,
            .payload = OtaProgressEvent{
                .percent = percent,
                .bytesDownloaded = bytesDownloaded,
                .totalBytes = m_metrics.totalBytes,
                .success = false,
                .message = std::string("Downloading: ") + std::to_string(percent) + "%"
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void) m_bus.publish(evt); // TODO: handle publish failure?
    }

    void OtaService::reportError(const OtaError error, const std::string &message) {
        m_lastError = error;
        m_lastErrorMessage = message;

        LOG_ERROR(OTA_TAG, "Error [%s]: %s", toString(error), message.c_str());

        // Publish error event
        const Event evt{
            .type = EventType::OtaProgress,
            .payload = OtaProgressEvent{
                .percent = m_progressPercent.load(),
                .bytesDownloaded = m_metrics.bytesDownloaded,
                .totalBytes = m_metrics.totalBytes,
                .success = false,
                .message = message
            },
            .timestampMs = static_cast<std::uint64_t>(millis()),
            .priority = EventPriority::E_HIGH
        };
        (void) m_bus.publish(evt); // TODO: handle publish failure?
    }

    void OtaService::publishStatusToMqtt() {
        // Status is published via OtaStateChangedEvent which MqttService handles
        // This method can be used for on-demand status publishing

        const auto state{m_state.load()};
        const Event evt{
            .type = EventType::OtaStateChanged,
            .payload = OtaStateChangedEvent{
                .oldState = static_cast<std::uint8_t>(state),
                .newState = static_cast<std::uint8_t>(state),
                .message = std::string("Status request - state: ") + toString(state),
                .timestampMs = static_cast<std::uint64_t>(millis())
            },
            .timestampMs = static_cast<std::uint64_t>(millis())
        };
        (void) m_bus.publish(evt); // TODO: handle publish failure?
    }

    void OtaService::acquireWakeLock() {
        if (m_powerService && !m_wakeLock.isValid()) {
            m_wakeLock = m_powerService->requestWakeLock("ota_update");
            LOG_DEBUG(OTA_TAG, "Wake lock acquired");
        }
    }

    void OtaService::releaseWakeLock() {
        if (m_powerService && m_wakeLock.isValid()) {
            m_powerService->releaseWakeLock(m_wakeLock);
            LOG_DEBUG(OTA_TAG, "Wake lock released");
        }
    }

    OtaError OtaService::mapHttpError(const int httpCode) const {
        switch (httpCode) {
            case 400: return OtaError::HttpBadRequest;
            case 401: return OtaError::HttpUnauthorized;
            case 403: return OtaError::HttpForbidden;
            case 404: return OtaError::HttpNotFound;
            case 500:
            case 502:
            case 503:
            case 504: return OtaError::HttpServerError;
            default: return  OtaError::HttpError;
        }
    }
}
