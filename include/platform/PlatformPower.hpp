#ifndef ISIC_PLATFORM_POWER_HPP
#define ISIC_PLATFORM_POWER_HPP

#include <Arduino.h>
#include "../common/Types.hpp"

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <esp_sleep.h>
#include <rom/rtc.h>

namespace isic::platform
{
inline WakeupReason detectWakeupReason()
{
    switch (esp_sleep_get_wakeup_cause())
    {
        case ESP_SLEEP_WAKEUP_TIMER:
            return WakeupReason::Timer;
        case ESP_SLEEP_WAKEUP_EXT0:
        case ESP_SLEEP_WAKEUP_EXT1:
            return WakeupReason::External;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
        case ESP_SLEEP_WAKEUP_ULP:
            return WakeupReason::Unknown;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            // Check reset reason for power on, watchdog, etc.
            switch (rtc_get_reset_reason(0))
            {
                case POWERON_RESET:
                    return WakeupReason::PowerOn;
                case RTCWDT_RTC_RESET:
                case TGWDT_CPU_RESET:
                    return WakeupReason::WatchdogReset;
                case EXT_CPU_RESET:
                    return WakeupReason::External;
                default:
                    return WakeupReason::Unknown;
            }
    }
}
}

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

#include <user_interface.h>

namespace isic::platform
{
inline WakeupReason detectWakeupReason()
{

    switch (const auto *info = ESP.getResetInfoPtr(); info->reason)
    {
        case REASON_DEEP_SLEEP_AWAKE:
            return WakeupReason::Timer;
        case REASON_EXT_SYS_RST:
            return WakeupReason::External;
        case REASON_WDT_RST:
        case REASON_SOFT_WDT_RST:
            return WakeupReason::WatchdogReset;
        case REASON_DEFAULT_RST:
            return WakeupReason::PowerOn;
        default:
            return WakeupReason::Unknown;
    }
}
}

#else
#error "Unsupported platform"
#endif

#endif // ISIC_PLATFORM_POWER_HPP
