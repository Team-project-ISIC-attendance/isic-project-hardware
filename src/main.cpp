/**
 * @file main.cpp
 * @brief ISIC Attendance System - ESP32 Firmware Entry Point
 *
 * Production-grade firmware for ISIC card-based attendance tracking.
 *
 * @note C++20 Standard - No C++23 features used.
 * @note Embedded-safe: No exceptions, minimal heap allocation.
 *
 * Features:
 * - NFC card reading via PN532 with interrupt-based wake
 * - MQTT-based attendance reporting with smart batching
 * - Power management with light/deep sleep support
 * - Comprehensive health monitoring
 * - Full OTA update capability with state machine
 * - User feedback via LED and buzzer
 * - Configurable via MQTT or NVS
 * - Extensible module system
 *
 * Architecture:
 * - Event-driven with optimized EventBus (priority support)
 * - Modular services with clear interfaces
 * - Non-blocking high-throughput design
 * - FreeRTOS tasks with configurable priorities
 *
 * Normal Operation Flow:
 * 1. Device sleeps with minimal power
 * 2. PN532 interrupt wakes the ESP32 when card presented
 * 3. PN532 driver reads the ISIC card
 * 4. AttendanceModule processes and provides feedback
 * 5. AttendanceBatcher collects events for batched MQTT publish
 * 6. Device returns to sleep after idle timeout
 */

#include <Arduino.h>

// Core components
#include "AppConfig.hpp"
#include "core/EventBus.hpp"
#include "core/Types.hpp"
#include "core/Logger.hpp"
#include "core/ModuleManager.hpp"

// Services
#include "services/ConfigService.hpp"
#include "services/MqttService.hpp"
#include "services/OtaService.hpp"
#include "services/PowerService.hpp"
#include "services/HealthMonitorService.hpp"
#include "services/UserFeedbackService.hpp"
#include "services/AttendanceBatcher.hpp"

// Drivers
#include "drivers/Pn532Driver.hpp"

// Modules
#include "modules/AttendanceModule.hpp"
#include "modules/OtaModule.hpp"

namespace {
    constexpr auto* MAIN_TAG = "Main";

    // ==================== Global Instances ====================
    // Allocated in setup, persist across loop

    // Core
    isic::EventBus* g_eventBus = nullptr;
    isic::ConfigService* g_configService = nullptr;
    isic::ModuleManager* g_moduleManager = nullptr;

    // Services
    isic::PowerService* g_powerService = nullptr;
    isic::MqttService* g_mqttService = nullptr;
    isic::OtaService* g_otaService = nullptr;
    isic::HealthMonitorService* g_healthMonitor = nullptr;
    isic::UserFeedbackService* g_feedbackService = nullptr;
    isic::AttendanceBatcher* g_attendanceBatcher = nullptr;

    // Drivers
    isic::Pn532Driver* g_pn532Driver = nullptr;

    // Modules
    isic::AttendanceModule* g_attendanceModule = nullptr;
    isic::OtaModule* g_otaModule = nullptr;

    bool g_initialized = false;

    // ==================== Timing ====================
    std::uint32_t g_lastStatusLogMs = 0;
    constexpr std::uint32_t STATUS_LOG_INTERVAL_MS = 3600;
}

/**
 * @brief Initialize all system components.
 * @return true if initialization successful
 */
bool initializeSystem() {
    LOG_INFO(MAIN_TAG, "========================================");
    LOG_INFO(MAIN_TAG, "  ISIC Attendance System v%.*s",
             static_cast<int>(isic::defaults::FIRMWARE_VERSION.size()),
             isic::defaults::FIRMWARE_VERSION.data());
    LOG_INFO(MAIN_TAG, "  Build: %s %s", __DATE__, __TIME__);
    LOG_INFO(MAIN_TAG, "========================================");

    // 1. Create EventBus (central message hub)
    LOG_INFO(MAIN_TAG, "[1/12] Initializing EventBus...");
    const isic::EventBus::Config busConfig{
        .queueLength = isic::defaults::EVENTBUS_QUEUE_SIZE,
        .highPriorityQueueLength = 16,
        .taskStackSize = isic::defaults::EVENTBUS_TASK_STACK,
        .taskPriority = isic::defaults::EVENTBUS_TASK_PRIORITY,
        .taskCore = isic::defaults::EVENTBUS_TASK_CORE
    };
    g_eventBus = new isic::EventBus(busConfig);
    g_eventBus->start();

    // 2. Create and initialize ConfigService
    LOG_INFO(MAIN_TAG, "[2/12] Initializing ConfigService...");
    g_configService = new isic::ConfigService(*g_eventBus);
    if (const auto status = g_configService->begin(); !status.ok()) {
        LOG_ERROR(MAIN_TAG, "ConfigService init failed: %s", status.message);
        return false;
    }

    const auto& config = g_configService->get();
    LOG_INFO(MAIN_TAG, "Config loaded: device=%s, location=%s",
             config.device.deviceId.c_str(),
             config.device.locationId.c_str());

    // 3. Create and initialize PowerService
    LOG_INFO(MAIN_TAG, "[3/12] Initializing PowerService...");
    g_powerService = new isic::PowerService(*g_eventBus);
    if (const auto status = g_powerService->begin(config); !status.ok()) {
        LOG_ERROR(MAIN_TAG, "PowerService init failed: %s", status.message);
        return false;
    }

    //4. Create and initialize UserFeedbackService
    LOG_INFO(MAIN_TAG, "[4/12] Initializing UserFeedbackService...");
    g_feedbackService = new isic::UserFeedbackService(*g_eventBus);
    if (const auto status = g_feedbackService->begin(config); !status.ok()) {
        LOG_WARNING(MAIN_TAG, "UserFeedbackService init failed: %s (continuing)", status.message);
        // Non-critical - continue without feedback
    }

    // 5. Create and initialize MqttService
    LOG_INFO(MAIN_TAG, "[5/12] Initializing MqttService...");
    g_mqttService = new isic::MqttService(*g_eventBus);
    if (const auto status = g_mqttService->begin(config, *g_powerService); !status.ok()) {
        LOG_ERROR(MAIN_TAG, "MqttService init failed: %s", status.message);
        return false;
    }

    // 6. Create and initialize OtaService
    LOG_INFO(MAIN_TAG, "[6/12] Initializing OtaService...");
    g_otaService = new isic::OtaService(*g_eventBus);
    if (const auto status = g_otaService->begin(config, *g_powerService); !status.ok()) {
        LOG_WARNING(MAIN_TAG, "OtaService init failed: %s (continuing)", status.message);
        // Non-critical - continue without OTA
    }

    // 7. Create and initialize HealthMonitorService
    LOG_INFO(MAIN_TAG, "[7/12] Initializing HealthMonitorService...");
    g_healthMonitor = new isic::HealthMonitorService(*g_eventBus);
    if (const auto status = g_healthMonitor->begin(config); !status.ok()) {
        LOG_ERROR(MAIN_TAG, "HealthMonitorService init failed: %s", status.message);
        return false;
    }

    // 8. Create and initialize PN532 Driver
    LOG_INFO(MAIN_TAG, "[8/12] Initializing PN532 Driver...");
    g_pn532Driver = new isic::Pn532Driver(*g_eventBus);
    if (const auto status = g_pn532Driver->begin(config.pn532); !status.ok()) {
        LOG_WARNING(MAIN_TAG, "PN532 init failed: %s (continuing without NFC)", status.message);
    } else {
        g_pn532Driver->configureWakeSource(*g_powerService);
    }

    // 9. Create and initialize AttendanceBatcher
    LOG_INFO(MAIN_TAG, "[9/12] Initializing AttendanceBatcher...");
    g_attendanceBatcher = new isic::AttendanceBatcher(*g_eventBus);
    if (const auto status = g_attendanceBatcher->begin(config, *g_mqttService, *g_powerService); !status.ok()) {
        LOG_WARNING(MAIN_TAG, "AttendanceBatcher init failed: %s (continuing without batching)",
                   status.message);
    }

    // 10. Create and initialize AttendanceModule
    LOG_INFO(MAIN_TAG, "[10/12] Initializing AttendanceModule...");
    g_attendanceModule = new isic::AttendanceModule(*g_eventBus, *g_pn532Driver, *g_powerService);
    g_attendanceModule->setFeedbackService(g_feedbackService);
    g_attendanceModule->setBatcher(g_attendanceBatcher);
    if (const auto status = g_attendanceModule->begin(config); !status.ok()) {
        LOG_ERROR(MAIN_TAG, "AttendanceModule init failed: %s", status.message);
        return false;
    }

    // 11. Create OtaModule
    LOG_INFO(MAIN_TAG, "[11/12] Initializing OtaModule...");
    g_otaModule = new isic::OtaModule(*g_otaService, *g_eventBus);

    // 12. Create ModuleManager and register modules
    LOG_INFO(MAIN_TAG, "[12/12] Initializing ModuleManager...");
    g_moduleManager = new isic::ModuleManager(*g_eventBus);
    g_moduleManager->addModule(*g_attendanceModule);
    g_moduleManager->addModule(*g_otaModule);

    // Register components with HealthMonitor
    LOG_INFO(MAIN_TAG, "Registering health check components...");
    g_healthMonitor->registerComponent(g_mqttService);
    g_healthMonitor->registerComponent(g_pn532Driver);
    g_healthMonitor->registerComponent(g_attendanceModule);
    g_healthMonitor->registerComponent(g_otaService);

    // Start all modules
    LOG_INFO(MAIN_TAG, "Starting modules...");
    g_moduleManager->startAll();

    // Signal successful startup
    if (g_feedbackService) {
        g_feedbackService->signalConnected();
    }

    LOG_INFO(MAIN_TAG, "========================================");
    LOG_INFO(MAIN_TAG, "  System Initialization Complete");
    LOG_INFO(MAIN_TAG, "  Free heap: %u bytes", ESP.getFreeHeap());
    LOG_INFO(MAIN_TAG, "  Modules: %u running",
             static_cast<unsigned>(g_moduleManager->getMetrics().runningModules));
    LOG_INFO(MAIN_TAG, "========================================");

    return true;
}

/**
 * @brief Arduino setup function - runs once at startup.
 */
void setup() {
    // Initialize serial for logging
    Serial.begin(115200);
    delay(100);  // Allow serial to stabilize

    Serial.println();
    Serial.println("=== ESP32 Starting ===");

    // Initialize the system
    g_initialized = initializeSystem();

    if (!g_initialized) {
        LOG_ERROR(MAIN_TAG, "System initialization failed!");
        LOG_ERROR(MAIN_TAG, "Entering safe mode - restart in 30s");
    }

    g_lastStatusLogMs = millis();
}

/**
 * @brief Arduino loop function - runs repeatedly.
 *
 * In this architecture, most work is done by FreeRTOS tasks.
 * The main loop handles:
 * - Watchdog feeding
 * - Periodic status logging
 * - Safe mode handling
 */
void loop() {
    if (!g_initialized) {
        // Safe mode - just blink and wait for restart
        static std::uint32_t lastBlink = 0;
        static std::uint32_t safeStart = millis();

        if (millis() - lastBlink > 500) {
            lastBlink = millis();
            #ifdef LED_BUILTIN
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            #endif
        }

        // Restart after 30 seconds in safe mode
        if (millis() - safeStart > 30000) {
            LOG_WARNING(MAIN_TAG, "Restarting from safe mode...");
            ESP.restart();
        }

        delay(100);
        return;
    }

    // Normal operation - main loop is mostly idle
    // as work is done by RTOS tasks

    const auto now = millis();

    // Periodic status logging
    if (now - g_lastStatusLogMs >= STATUS_LOG_INTERVAL_MS) {
        g_lastStatusLogMs = now;

        // Log system status
        if (g_healthMonitor) {
            const auto health = g_healthMonitor->getDeviceHealth();
            LOG_INFO(MAIN_TAG, "Status: %s | Heap: %u/%u KB | Uptime: %us",
                     isic::toString(health.overallState),
                     health.freeHeapBytes / 1024,
                     health.minFreeHeapBytes / 1024,
                     health.uptimeSeconds);
        }

        // Log MQTT metrics
        if (g_mqttService) {
            const auto metrics = g_mqttService->getMetrics();
            LOG_INFO(MAIN_TAG, "MQTT: %s | Pub: %u | Fail: %u | Queue: %u",
                     metrics.isConnected ? "Connected" : "Disconnected",
                     metrics.messagesPublished,
                     metrics.messagesFailed,
                     static_cast<unsigned>(metrics.currentQueueSize));
        }

        // Log attendance metrics
        if (g_attendanceModule) {
            const auto metrics = g_attendanceModule->getMetrics();
            LOG_INFO(MAIN_TAG, "Attendance: Cards: %u | Debounced: %u | Batched: %u",
                     metrics.totalCardsProcessed,
                     metrics.cardsDroppedDebounce,
                     metrics.eventsBatched);
        }

        // Log batcher metrics
        if (g_attendanceBatcher) {
            const auto metrics = g_attendanceBatcher->getMetrics();
            LOG_INFO(MAIN_TAG, "Batcher: Sent: %u | Pending: %u | Current: %u",
                     metrics.batchesSent,
                     static_cast<unsigned>(metrics.pendingBufferCount),
                     static_cast<unsigned>(metrics.currentBatchSize));
        }

    }

    // Yield to other tasks
    delay(100);
}

// Optional: Handle system errors
extern "C" void esp_task_wdt_isr_user_handler() {
    LOG_ERROR(MAIN_TAG, "Watchdog timeout - restarting");
}
