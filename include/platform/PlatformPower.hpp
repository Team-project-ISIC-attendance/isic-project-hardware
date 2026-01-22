#ifndef ISIC_PLATFORM_POWER_HPP
#define ISIC_PLATFORM_POWER_HPP

/**
 * @file PlatformPower.hpp
 * @brief Platform-specific power management utilities
 *
 * Provides wakeup reason detection for ESP8266 and ESP32.
 * Used by PowerService to determine system boot cause and
 * implement appropriate recovery behavior.
 */

#include "common/Types.hpp"

#include <Arduino.h>

// ============================================================================
// ESP32 Implementation
// ============================================================================

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <esp_sleep.h>
#include <rom/rtc.h>

namespace isic::platform
{
/**
 * @brief Detect why the ESP32 woke up or reset
 *
 * Checks both sleep wakeup cause and reset reason to provide
 * a unified WakeupReason for application logic.
 *
 * @return WakeupReason indicating boot/wakeup cause
 *
 * @par Wakeup Sources Detected
 * - Timer: RTC timer expired during deep sleep
 * - External: EXT0/EXT1 GPIO wakeup
 * - PowerOn: Initial power-on reset
 * - WatchdogReset: RTC or task watchdog triggered
 */
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
            // Not from sleep - check reset reason
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

} // namespace isic::platform

// ============================================================================
// ESP8266 Implementation
// ============================================================================

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

#include <user_interface.h>

namespace isic::platform
{
/**
 * @brief Detect why the ESP8266 woke up or reset
 *
 * Uses reset info structure to determine boot cause.
 *
 * @return WakeupReason indicating boot/wakeup cause
 *
 * @par Reset Reasons Detected
 * - Timer: Woke from deep sleep (RTC timer)
 * - External: External reset signal
 * - PowerOn: Default/power-on reset
 * - WatchdogReset: Hardware or software watchdog
 */
inline WakeupReason detectWakeupReason()
{
    const auto *info = ESP.getResetInfoPtr();

    switch (info->reason)
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
} // namespace isic::platform

#else
#error "Unsupported platform: Define ARDUINO_ARCH_ESP32 or ARDUINO_ARCH_ESP8266"
#endif

#endif // ISIC_PLATFORM_POWER_HPP
