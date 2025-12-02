#ifndef ISIC_PROJECT_HARDWARE_OTASERVICE_HPP
#define ISIC_PROJECT_HARDWARE_OTASERVICE_HPP

/**
 * @file OtaService.hpp
 * @brief Production-grade Over-The-Air (OTA) firmware update service for ESP32.
 *
 * This service implements a complete OTA update flow suitable for production deployments:
 *
 * Features:
 * - Full state machine: Idle → Checking → Downloading → Verifying → Applying → Completed/Failed
 * - ESP-IDF native OTA APIs (esp_ota_begin, esp_ota_write, esp_ota_end)
 * - Dual-partition OTA layout (ota_0, ota_1) with automatic partition selection
 * - HTTPS support with optional certificate validation
 * - Firmware rollback support after failed boot
 * - SHA256 checksum verification
 * - Non-blocking operation in dedicated FreeRTOS task
 * - Wake lock integration to prevent sleep during update
 * - Progress reporting via EventBus
 * - MQTT status publishing
 * - Exponential backoff on failures
 * - Configurable update windows
 *
 * Partition Layout (see partitions_ota_esp32.csv):
 * - nvs:     0x9000  - 0xE000   (20KB)  - Non-volatile storage
 * - otadata: 0xE000  - 0x10000  (8KB)   - OTA state tracking
 * - app0:    0x10000 - 0x150000 (1.25MB) - OTA slot 0 (ota_0)
 * - app1:    0x150000- 0x290000 (1.25MB) - OTA slot 1 (ota_1)
 * - spiffs:  0x290000- 0x400000 (1.4MB) - Filesystem
 *
 * Boot Flow:
 * 1. Bootloader reads otadata to determine which slot to boot
 * 2. After OTA, new partition is marked pending validation
 * 3. On successful boot, app marks partition as valid
 * 4. If boot fails, bootloader rolls back to previous partition
 *
 * Security Model:
 * - HTTPS with CA certificate validation (optional)
 * - SHA256 checksum verification of downloaded image
 * - Magic byte validation of firmware header
 * - Version comparison prevents downgrades (configurable)
 *
 * MQTT Protocol:
 * - Command topic: device/<id>/ota/set
 * - Status topic:  device/<id>/ota/status  (retained)
 * - Progress topic: device/<id>/ota/progress
 * - Error topic:   device/<id>/ota/error
 *
 * @see OtaModule for module wrapper
 * @see MqttService for MQTT integration
 */

#include <Arduino.h>
#include <HTTPClient.h>

#include <atomic>
#include <string>
#include <cstdint>
#include <functional>
#include <optional>

#include "AppConfig.hpp"
#include "core/EventBus.hpp"
#include "core/IHealthCheck.hpp"
#include "core/Result.hpp"
#include "services/PowerService.hpp"

// ESP-IDF includes for OTA
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_app_format.h>
#include <esp_system.h>
#include <WiFiClient.h>

// Forward declaration for dependencies
namespace isic {
    class PowerService;
}

namespace isic {
    /**
     * @brief OTA command action types (received via MQTT).
     */
    enum class OtaAction : std::uint8_t {
        Update,         // Start firmware update
        Rollback,       // Roll back to previous firmware
        Check,          // Check for available updates
        Cancel,         // Cancel in-progress update
        MarkValid,      // Manually mark current partition as valid
        GetStatus,      // Request current OTA status
    };

    [[nodiscard]] constexpr const char* toString(OtaAction action) noexcept {
        switch (action) {
            case OtaAction::Update:    return "update";
            case OtaAction::Rollback:  return "rollback";
            case OtaAction::Check:     return "check";
            case OtaAction::Cancel:    return "cancel";
            case OtaAction::MarkValid: return "mark_valid";
            case OtaAction::GetStatus: return "get_status";
            default:                   return "unknown";
        }
    }

    /**
     * @brief OTA command received from MQTT.
     *
     * JSON format:
     * {
     *   "action": "update",
     *   "url": "https://server.com/firmware.bin",
     *   "version": "1.2.3",
     *   "force": false,
     *   "sha256": "abc123..." (optional)
     * }
     */
    struct OtaCommand {
        OtaAction action{OtaAction::Update};
        std::string url{};
        std::string version{};
        std::string sha256{};     // Expected SHA256 hash (hex string)
        bool force{false};        // Force update even if same/older version
        std::uint32_t timeout{0}; // Custom timeout (0 = use default)
    };

    // ==================== OTA Update Metrics ====================

    /**
     * @brief Comprehensive OTA metrics for monitoring and debugging.
     */
    struct OtaMetrics {
        // Attempt counters
        std::uint32_t totalAttempts{0};
        std::uint32_t successfulUpdates{0};
        std::uint32_t failedUpdates{0};
        std::uint32_t rollbackCount{0};
        std::uint32_t consecutiveFailures{0};

        // Timing
        std::uint64_t lastCheckMs{0};
        std::uint64_t lastUpdateMs{0};
        std::uint64_t lastSuccessMs{0};
        std::uint64_t updateStartMs{0};
        std::uint32_t lastUpdateDurationMs{0};

        // Current download progress
        std::uint32_t bytesDownloaded{0};
        std::uint32_t totalBytes{0};
        std::uint8_t progressPercent{0};
        std::uint32_t downloadSpeedBps{0};

        // Partition info
        std::string currentPartition{};
        std::string nextPartition{};
    };

    // ==================== OTA Error Codes ====================

    /**
     * @brief Detailed OTA error codes for diagnostics.
     */
    enum class OtaError : std::uint8_t {
        None = 0,

        // Network errors
        NetworkError,
        WifiDisconnected,
        DnsResolutionFailed,
        ConnectionTimeout,
        SslHandshakeFailed,
        CertificateInvalid,

        // HTTP errors
        HttpError,
        HttpBadRequest,
        HttpUnauthorized,
        HttpForbidden,
        HttpNotFound,
        HttpServerError,

        // Download errors
        DownloadFailed,
        DownloadTimeout,
        DownloadIncomplete,
        ContentLengthMismatch,

        // Verification errors
        VerificationFailed,
        ChecksumMismatch,
        MagicByteInvalid,
        ImageTooLarge,
        ImageTooSmall,

        // Flash errors
        PartitionNotFound,
        InsufficientSpace,
        WriteError,
        EraseError,
        FlashCorrupted,

        // General errors
        InvalidImage,
        InvalidVersion,
        Timeout,
        Cancelled,
        AlreadyInProgress,
        NotEnabled,
        SameVersion,
        OutsideUpdateWindow,
        RollbackFailed,
        InternalError,
    };

    [[nodiscard]] constexpr const char* toString(const OtaError error) noexcept {
        switch (error) {
            case OtaError::None:                  return "none";
            case OtaError::NetworkError:          return "network_error";
            case OtaError::WifiDisconnected:      return "wifi_disconnected";
            case OtaError::DnsResolutionFailed:   return "dns_failed";
            case OtaError::ConnectionTimeout:     return "connection_timeout";
            case OtaError::SslHandshakeFailed:    return "ssl_handshake_failed";
            case OtaError::CertificateInvalid:    return "certificate_invalid";
            case OtaError::HttpError:             return "http_error";
            case OtaError::HttpBadRequest:        return "http_bad_request";
            case OtaError::HttpUnauthorized:      return "http_unauthorized";
            case OtaError::HttpForbidden:         return "http_forbidden";
            case OtaError::HttpNotFound:          return "http_not_found";
            case OtaError::HttpServerError:       return "http_server_error";
            case OtaError::DownloadFailed:        return "download_failed";
            case OtaError::DownloadTimeout:       return "download_timeout";
            case OtaError::DownloadIncomplete:    return "download_incomplete";
            case OtaError::ContentLengthMismatch: return "content_length_mismatch";
            case OtaError::VerificationFailed:    return "verification_failed";
            case OtaError::ChecksumMismatch:      return "checksum_mismatch";
            case OtaError::MagicByteInvalid:      return "magic_byte_invalid";
            case OtaError::ImageTooLarge:         return "image_too_large";
            case OtaError::ImageTooSmall:         return "image_too_small";
            case OtaError::PartitionNotFound:     return "partition_not_found";
            case OtaError::InsufficientSpace:     return "insufficient_space";
            case OtaError::WriteError:            return "write_error";
            case OtaError::EraseError:            return "erase_error";
            case OtaError::FlashCorrupted:        return "flash_corrupted";
            case OtaError::InvalidImage:          return "invalid_image";
            case OtaError::InvalidVersion:        return "invalid_version";
            case OtaError::Timeout:               return "timeout";
            case OtaError::Cancelled:             return "cancelled";
            case OtaError::AlreadyInProgress:     return "already_in_progress";
            case OtaError::NotEnabled:            return "not_enabled";
            case OtaError::SameVersion:           return "same_version";
            case OtaError::OutsideUpdateWindow:   return "outside_update_window";
            case OtaError::RollbackFailed:        return "rollback_failed";
            case OtaError::InternalError:         return "internal_error";
            default:                              return "unknown";
        }
    }

    // ==================== OTA Partition Info ====================

    /**
     * @brief Information about an OTA partition.
     */
    struct OtaPartitionInfo {
        std::string label{};
        std::uint32_t address{0};
        std::uint32_t size{0};
        bool isBootable{false};
        bool isRunning{false};
        std::string appVersion{};
        std::string appProjectName{};
    };

    // ==================== OTA Service ====================

    /**
     * @brief Production-grade OTA service with full state machine.
     *
     * Usage:
     *   OtaService ota(bus);
     *   ota.begin(config, powerService);
     *
     *   // Trigger via MQTT command or programmatically:
     *   OtaCommand cmd;
     *   cmd.action = OtaAction::Update;
     *   cmd.url = "https://server.com/firmware.bin";
     *   cmd.version = "1.2.0";
     *   ota.executeCommand(cmd);
     */
    class OtaService : public IEventListener, public IHealthCheck {
    public:
        // Progress callback type
        using ProgressCallback = std::function<void(std::uint8_t percent, std::uint32_t bytesDownloaded, std::uint32_t totalBytes)>;

        explicit OtaService(EventBus& bus);
        ~OtaService() override;

        // Non-copyable, non-movable
        OtaService(const OtaService&) = delete;
        OtaService& operator=(const OtaService&) = delete;
        OtaService(OtaService&&) = delete;
        OtaService& operator=(OtaService&&) = delete;

        // ==================== Lifecycle ====================

        /**
         * @brief Initialize the OTA service.
         *
         * - Starts OTA monitoring task
         * - Checks if rollback is needed
         * - Marks partition as valid if boot succeeded
         */
        [[nodiscard]] Status begin(const AppConfig& cfg, PowerService& powerService);

        /**
         * @brief Stop the OTA service.
         */
        void stop();

        // ==================== OTA Control ====================

        /**
         * @brief Execute an OTA command (update, rollback, check, etc.).
         * @param cmd Command to execute
         * @return Status indicating if command was accepted
         */
        [[nodiscard]] Status executeCommand(const OtaCommand& cmd);

        /**
         * @brief Trigger an OTA update (convenience method).
         * @param url URL of the firmware binary
         * @param version Target version string
         * @param force If true, update even if same version
         * @return Status indicating if trigger was accepted
         */
        [[nodiscard]] Status triggerOta(const std::string& url, const std::string& version, bool force = false);

        /**
         * @brief Trigger update with expected SHA256 checksum.
         */
        [[nodiscard]] Status triggerOtaWithChecksum(const std::string& url, const std::string& version, const std::string& sha256);

        /**
         * @brief Check for available updates (async).
         * Will publish OtaVersionInfo event when complete.
         */
        void checkForUpdates();

        /**
         * @brief Cancel an in-progress OTA update.
         */
        void cancelOta();

        /**
         * @brief Roll back to the previous firmware partition.
         * @return Status indicating success/failure
         */
        [[nodiscard]] Status rollback();

        /**
         * @brief Mark the current running partition as valid.
         * Call this after successful startup to confirm the new firmware works.
         */
        [[nodiscard]] Status markPartitionValid();

        // ==================== State Queries ====================

        /**
         * @brief Get current OTA state.
         */
        [[nodiscard]] OtaState getState() const { return m_state.load(); }

        /**
         * @brief Check if OTA is currently in progress.
         */
        [[nodiscard]] bool isInProgress() const {
            const auto s = m_state.load();
            return s != OtaState::Idle && s != OtaState::Completed && s != OtaState::Failed;
        }

        /**
         * @brief Get current metrics.
         */
        [[nodiscard]] OtaMetrics getMetrics() const;

        /**
         * @brief Get last error code.
         */
        [[nodiscard]] OtaError getLastError() const { return m_lastError; }

        /**
         * @brief Get last error message.
         */
        [[nodiscard]] std::string getLastErrorMessage() const { return m_lastErrorMessage; }

        /**
         * @brief Get progress percentage (0-100).
         */
        [[nodiscard]] std::uint8_t getProgress() const { return m_progressPercent.load(); }

        /**
         * @brief Get information about current running partition.
         */
        [[nodiscard]] OtaPartitionInfo getRunningPartition() const;

        /**
         * @brief Get information about next update partition.
         */
        [[nodiscard]] OtaPartitionInfo getNextUpdatePartition() const;

        /**
         * @brief Check if the current boot is pending validation.
         * Returns true if this is a fresh boot after OTA and partition
         * hasn't been marked valid yet.
         */
        [[nodiscard]] bool isPendingValidation() const;

        // ==================== Configuration ====================

        /**
         * @brief Update configuration at runtime.
         */
        void updateConfig(const OtaConfig& cfg);

        /**
         * @brief Enable/disable OTA service.
         */
        void setEnabled(bool enabled);

        /**
         * @brief Check if OTA is enabled.
         */
        [[nodiscard]] bool isEnabled() const { return m_enabled.load(); }

        /**
         * @brief Set custom CA certificate for HTTPS.
         * @param cert PEM-encoded CA certificate
         */
        void setCaCertificate(const char* cert);

        /**
         * @brief Set progress callback for custom progress handling.
         */
        void setProgressCallback(ProgressCallback callback);

        // ==================== IHealthCheck Interface ====================

        [[nodiscard]] HealthStatus getHealth() const override;
        [[nodiscard]] std::string_view getComponentName() const override { return "OTA"; }
        bool performHealthCheck() override;

        // ==================== IEventListener Interface ====================

        void onEvent(const Event& event) override;

    private:
        // State machine
        void transitionTo(OtaState newState, const std::string& message = "");

        // OTA task
        static void otaTaskThunk(void* arg);
        void otaTask();

        // Update process - main flow
        bool performUpdate();
        bool downloadAndFlashFirmware();

        // Individual steps
        bool connectToServer(HTTPClient& http, WiFiClient& client);
        bool validateHttpResponse(HTTPClient& http, int httpCode);
        bool preparePartition(std::int32_t contentLength);
        bool downloadChunks(WiFiClient& stream, std::int32_t contentLength);
        bool finalizeUpdate();

        // Verification
        bool verifyImageHeader(const std::uint8_t* data, std::size_t len);
        bool verifyChecksum();

        // Partition operations
        [[nodiscard]] const esp_partition_t* getRunningPartitionPtr() const;
        [[nodiscard]] const esp_partition_t* getNextUpdatePartitionPtr() const;

        // Version handling
        [[nodiscard]] bool isNewerVersion(const std::string& available) const;
        [[nodiscard]] bool parseVersion(const std::string& version,
                                         int& major, int& minor, int& patch) const;
        [[nodiscard]] bool isWithinUpdateWindow() const;

        // Backoff calculation
        [[nodiscard]] std::uint32_t calculateBackoff() const;
        void resetBackoff();
        void incrementBackoff();

        // Progress reporting
        void reportProgress(std::uint8_t percent, std::uint32_t bytesDownloaded);
        void reportError(OtaError error, const std::string& message);
        void publishStatusToMqtt();

        // Wake lock management
        void acquireWakeLock();
        void releaseWakeLock();

        // HTTP error mapping
        [[nodiscard]] OtaError mapHttpError(int httpCode) const;

        // References
        EventBus& m_bus;
        PowerService* m_powerService{nullptr};
        const OtaConfig* m_cfg{nullptr};
        const AppConfig* m_appCfg{nullptr};

        // State
        std::atomic<OtaState> m_state{OtaState::Idle};
        std::atomic<bool> m_enabled{true};
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_cancelRequested{false};
        std::atomic<std::uint8_t> m_progressPercent{0};

        // Current update info
        OtaCommand m_pendingCommand{};
        std::string m_pendingUrl{};
        std::string m_pendingVersion{};
        std::string m_pendingSha256{};
        bool m_forceUpdate{false};

        // ESP-IDF OTA handles
        esp_ota_handle_t m_otaHandle{0};
        const esp_partition_t* m_updatePartition{nullptr};

        // Error tracking
        OtaError m_lastError{OtaError::None};
        std::string m_lastErrorMessage{};

        // Metrics
        OtaMetrics m_metrics{};
        mutable SemaphoreHandle_t m_metricsMutex{nullptr};

        // Task management
        TaskHandle_t m_taskHandle{nullptr};
        SemaphoreHandle_t m_triggerSemaphore{nullptr};

        // Wake lock
        WakeLockHandle m_wakeLock{};

        // Backoff state
        std::uint32_t m_consecutiveFailures{0};
        std::uint64_t m_lastFailureMs{0};

        // Auto-check timer
        std::uint64_t m_lastAutoCheckMs{0};

        // HTTPS certificate (optional)
        const char* m_caCertificate{nullptr};

        // Progress callback
        ProgressCallback m_progressCallback{nullptr};

        // EventBus subscription
        EventBus::ListenerId m_subscriptionId{0};

        // Boot validation flag
        bool m_bootValidated{false};
    };

}  // namespace isic

#endif  // ISIC_PROJECT_HARDWARE_OTASERVICE_HPP
