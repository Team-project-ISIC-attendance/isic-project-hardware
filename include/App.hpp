#ifndef ISIC_APP_HPP
#define ISIC_APP_HPP

#include "common/Types.hpp"
#include "core/EventBus.hpp"

#include "services/AttendanceService.hpp"
#include "services/ConfigService.hpp"
#include "services/FeedbackService.hpp"
#include "services/HealthService.hpp"
#include "services/MqttService.hpp"
#include "services/OtaService.hpp"
#include "services/Pn532Service.hpp"
#include "services/PowerService.hpp"
#include "services/WiFiService.hpp"

#include <TaskSchedulerDeclarations.h>

namespace isic
{

/**
 * @brief Main application class coordinating all services
 */
class App
{
public:
    App();
    ~App() = default;

    // Lifecycle
    [[nodiscard]] Status begin();
    void loop();

    enum class AppState
    {
        Uninitialized,
        Initializing,
        Running,
        Stopping,
        Stopped,
        Error
    };

    [[nodiscard]] AppState getState() const
    {
        return m_appState;
    }
    bool isConfigured() const
    {
        return m_configService.isConfigured();
    }

    EventBus &getEventBus()
    {
        return m_eventBus;
    }
    ConfigService &getConfigService()
    {
        return m_configService;
    }
    WiFiService &getWiFiService()
    {
        return m_wifiService;
    }
    MqttService &getMqttService()
    {
        return m_mqttService;
    }
    Pn532Service &getPn532Service()
    {
        return m_pn532Service;
    }
    AttendanceService &getAttendanceService()
    {
        return m_attendanceService;
    }
    FeedbackService &getFeedbackService()
    {
        return m_feedbackService;
    }
    HealthService &getHealthService()
    {
        return m_healthService;
    }
    PowerService &getPowerService()
    {
        return m_powerService;
    }
    Scheduler &getScheduler()
    {
        return m_scheduler;
    }

private:
    void setupScheduler();

    // Static callbacks for TaskScheduler
    static constexpr uint32_t EVENTBUS_INTERVAL_MS = 10; // High priority: 100Hz event dispatch
    static constexpr uint32_t CONFIG_INTERVAL_MS = 5000;
    static constexpr uint32_t WIFI_INTERVAL_MS = 1000;
    static constexpr uint32_t MQTT_INTERVAL_MS = 1000;
    static constexpr uint32_t PN532_INTERVAL_MS = 100;
    static constexpr uint32_t ATTENDANCE_INTERVAL_MS = 100;
    static constexpr uint32_t FEEDBACK_INTERVAL_MS = 20; // Fast for smooth LED
    static constexpr uint32_t HEALTH_INTERVAL_MS = 10000;
    static constexpr uint32_t OTA_INTERVAL_MS = 1000;
    static constexpr uint32_t POWER_INTERVAL_MS = 1000;

    Scheduler m_scheduler;
    EventBus m_eventBus;

    ConfigService m_configService;
    WiFiService m_wifiService;
    MqttService m_mqttService;
    OtaService m_otaService;
    Pn532Service m_pn532Service;
    AttendanceService m_attendanceService;
    FeedbackService m_feedbackService;
    HealthService m_healthService;
    PowerService m_powerService;

    Task m_eventBusTask;
    Task m_configTask;
    Task m_wifiTask;
    Task m_mqttTask;
    Task m_pn532Task;
    Task m_attendanceTask;
    Task m_feedbackTask;
    Task m_healthTask;
    Task m_otaTask;
    Task m_powerTask;

    // State
    AppState m_appState{AppState::Uninitialized};

    // Static instance for task callbacks
    static App *m_instance;
};
} // namespace isic

#endif // ISIC_APP_HPP
