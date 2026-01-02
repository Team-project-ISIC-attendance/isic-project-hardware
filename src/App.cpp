#include "App.hpp"

#include <TaskScheduler.h>
#include "common/Logger.hpp"

namespace isic
{
constexpr auto *TAG{"App"};

App::App()
    : m_eventBus()
    , m_configService(m_eventBus)
    , m_wifiService(m_eventBus, m_configService.get().wifi, m_configService.getMutable())
    , m_mqttService(m_eventBus, m_configService.get().mqtt, m_configService.get().device)
    , m_otaService(m_eventBus, m_configService.get().ota)
    , m_pn532Service(m_eventBus, m_configService.get())
    , m_attendanceService(m_eventBus, m_configService.getMutable().attendance)
    , m_feedbackService(m_eventBus, m_configService.getMutable().feedback)
    , m_healthService(m_eventBus, m_configService.getMutable().health)
    , m_powerService(m_eventBus, m_configService.getMutable().power)
{
    LOG_INFO(TAG, "ISIC Attendance System");
    LOG_INFO(TAG, "Firmware: %s", DeviceConfig::Constants::kFirmwareVersion);
}
Status App::begin()
{
    LOG_INFO(TAG, "=== Starting Application ===");
    m_appState = AppState::Initializing;

    // Initialize core services first
    auto status = m_configService.begin();
    if (status.failed())
    {
        LOG_ERROR(TAG, "ConfigService init failed");
        m_appState = AppState::Error;
        return status;
    }

    // Initialize WiFi (may start in AP mode)
    status = m_wifiService.begin();
    if (status.failed())
    {
        LOG_ERROR(TAG, "WiFiService init failed");
        m_appState = AppState::Error;
        return status;
    }

    // Initialize MQTT
    status = m_mqttService.begin();
    if (status.failed())
    {
        LOG_ERROR(TAG, "MqttService init failed");
        m_appState = AppState::Error;
        return status;
    }

    // Initialize PN532 NFC reader
    status = m_pn532Service.begin();
    if (status.failed())
    {
        LOG_WARN(TAG, "Pn532Service init failed - continuing without NFC");
        // Don't fail the app, NFC might be reconnected later
    }

    // Initialize attendance tracking
    status = m_attendanceService.begin();
    if (status.failed())
    {
        LOG_ERROR(TAG, "AttendanceService init failed");
        m_appState = AppState::Error;
        return status;
    }

    // Initialize feedback
    status = m_feedbackService.begin();
    if (status.failed())
    {
        LOG_WARN(TAG, "FeedbackService init failed - continuing without feedback");
    }

    // Initialize health monitoring
    status = m_healthService.begin();
    if (status.failed())
    {
        LOG_WARN(TAG, "HealthService init failed - continuing without health monitoring");
    }

    // Initialize OTA
    status = m_otaService.begin();
    if (status.failed())
    {
        LOG_WARN(TAG, "OtaService init failed - continuing without OTA");
    }

    // Register services with health monitor (after all services initialized)
    m_healthService.registerComponent(&m_configService);
    m_healthService.registerComponent(&m_wifiService);
    m_healthService.registerComponent(&m_mqttService);
    m_healthService.registerComponent(&m_pn532Service);
    m_healthService.registerComponent(&m_attendanceService);
    m_healthService.registerComponent(&m_feedbackService);
    m_healthService.registerComponent(&m_otaService);
    m_healthService.registerComponent(&m_powerService);

    // Initialize power management (early to detect wakeup reason)
    status = m_powerService.begin();
    if (status.failed())
    {
        LOG_WARN(TAG, "PowerService init failed - continuing without power management");
    }

    // Setup scheduler tasks
    setupScheduler();

    m_appState = AppState::Running;
    LOG_INFO(TAG, "=== Application Started ===");
    LOG_INFO(TAG, "Free heap: %u bytes", ESP.getFreeHeap());

    return Status::Ok();
}

void App::loop()
{
    if (m_appState != AppState::Running)
    {
        return;
    }

    // Execute scheduler (includes automatic EventBus dispatch at 100Hz)
    m_scheduler.execute();

    // Yield to system
    yield();
}

// State methods implemented in header

// Scheduler setup in setupScheduler()

void App::setupScheduler()
{
    // EventBus dispatch task - CRITICAL: Runs at 100Hz (every 10ms)
    //
    // This task processes ALL async events for the entire system.
    // All services publish events which are queued in ring buffers,
    // then this task dispatches them to subscribers.
    //
    // Priority: HIGHEST - must run before other tasks to ensure timely delivery
    // Frequency: 100Hz - fast enough for real-time responsiveness
    // Overhead: ~10-50μs per call (depends on pending event count)
    m_eventBusTask.set(EVENTBUS_INTERVAL_MS, TASK_FOREVER, [this]() {
        std::size_t dispatched = m_eventBus.dispatch();
        (void) dispatched; // Suppress unused variable warning
#ifdef ISIC_DEBUG
        // Monitor event bus saturation (debug builds only)
        std::size_t pending = m_eventBus.pendingCount();
        if (dispatched > 10 || pending > 8)
        {
            LOG_WARN(TAG, "EventBus high load: dispatched=%u, pending=%u",
                     dispatched, pending);
        }
#endif
    });
    m_scheduler.addTask(m_eventBusTask);
    m_eventBusTask.enable();

    // ConfigService task - low frequency
    m_configTask.set(CONFIG_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_configService.loop();
    });
    m_scheduler.addTask(m_configTask);
    m_configTask.enable();

    // WiFiService task
    m_wifiTask.set(WIFI_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_wifiService.loop();
    });
    m_scheduler.addTask(m_wifiTask);
    m_wifiTask.enable();

    // MqttService task
    m_mqttTask.set(MQTT_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_mqttService.loop();
    });
    m_scheduler.addTask(m_mqttTask);
    m_mqttTask.enable();

    // Pn532Service task - high frequency for responsive card reading
    m_pn532Task.set(PN532_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_pn532Service.loop();
    });
    m_scheduler.addTask(m_pn532Task);
    m_pn532Task.enable();

    // AttendanceService task
    m_attendanceTask.set(ATTENDANCE_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_attendanceService.loop();
    });
    m_scheduler.addTask(m_attendanceTask);
    m_attendanceTask.enable();

    // FeedbackService task - high frequency for smooth LED/buzzer patterns
    m_feedbackTask.set(FEEDBACK_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_feedbackService.loop();
    });
    m_scheduler.addTask(m_feedbackTask);
    m_feedbackTask.enable();

    // HealthService task - low frequency
    m_healthTask.set(HEALTH_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_healthService.loop();
    });
    m_scheduler.addTask(m_healthTask);
    m_healthTask.enable();

    // OtaService task
    m_otaTask.set(OTA_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_otaService.loop();
    });
    m_scheduler.addTask(m_otaTask);
    m_otaTask.enable();

    // PowerService task
    m_powerTask.set(POWER_INTERVAL_MS, TASK_FOREVER, [this]() {
        m_powerService.loop();
    });
    m_scheduler.addTask(m_powerTask);
    m_powerTask.enable();

    LOG_DEBUG(TAG, "Scheduler configured with %d tasks", 10);
}
} // namespace isic
