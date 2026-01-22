#ifndef ISIC_PLATFORM_WIFI_HPP
#define ISIC_PLATFORM_WIFI_HPP

/**
 * @file PlatformWiFi.hpp
 * @brief WiFi platform helpers for ESP8266/ESP32
 *
 * Provides a small abstraction layer over WiFi features that differ
 * between ESP8266 and ESP32, including encryption detection and
 * power-management helpers.
 */

#include <Arduino.h>
#include <cstdint>

// ============================================================================
// ESP32 Implementation
// ============================================================================

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_wifi.h>

namespace isic::platform
{
/**
 * @brief Determine whether a scanned network uses encryption
 *
 * @param networkIndex Index from WiFi.scanNetworks()
 * @return true if secured, false if open
 */
inline bool isNetworkSecure(const std::uint8_t networkIndex)
{
    return WiFi.encryptionType(networkIndex) != WIFI_AUTH_OPEN;
}

/**
 * @brief Enable WiFi modem sleep (light sleep)
 */
inline void setWiFiLightSleep()
{
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

/**
 * @brief Power down WiFi subsystem
 *
 * @note ESP32: no-op here; WiFi.mode(WIFI_OFF) handles power down
 */
inline void wiFiPowerDown()
{
    // ESP32: WiFi.mode(WIFI_OFF) already powers down the WiFi
    // No additional action needed
}

/**
 * @brief Power up WiFi subsystem
 *
 * @note ESP32: no-op here; WiFi will wake when mode is set back to STA
 */
inline void wiFiPowerUp()
{
    // ESP32: WiFi will wake when we set mode back to STA
    // No additional action needed
}
} // namespace isic::platform

// ============================================================================
// ESP8266 Implementation
// ============================================================================

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

#include <ESP8266WiFi.h>
#include <user_interface.h> // ESP8266 low-level WiFi control (wifi_set_sleep_type)

namespace isic::platform
{
/**
 * @brief Determine whether a scanned network uses encryption
 *
 * @param networkIndex Index from WiFi.scanNetworks()
 * @return true if secured, false if open
 */
inline bool isNetworkSecure(const std::uint8_t networkIndex)
{
    return WiFi.encryptionType(networkIndex) != ENC_TYPE_NONE;
}

/**
 * @brief Enable WiFi light sleep
 */
inline void setWiFiLightSleep()
{
    wifi_set_sleep_type(LIGHT_SLEEP_T);
}

/**
 * @brief Power down WiFi subsystem
 *
 * @note ESP8266 requires a short delay after forceSleepBegin()
 */
inline void wiFiPowerDown()
{
    WiFi.forceSleepBegin();
    delay(1);  // Required for forceSleepBegin to take effect
}

/**
 * @brief Power up WiFi subsystem
 *
 * @note ESP8266 requires a short delay after forceSleepWake()
 */
inline void wiFiPowerUp()
{
    WiFi.forceSleepWake();
    delay(1);
}
} // namespace isic::platform

#else
#error "Unsupported platform: Define ARDUINO_ARCH_ESP32 or ARDUINO_ARCH_ESP8266"
#endif

#endif // ISIC_PLATFORM_WIFI_HPP
