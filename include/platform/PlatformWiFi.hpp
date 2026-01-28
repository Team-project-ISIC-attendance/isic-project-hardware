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
#include <cstring>

// ============================================================================
// ESP32 Implementation
// ============================================================================

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_wifi.h>
#ifdef ISIC_WIFI_EDUROAM
#include <esp_wpa2.h>
#endif

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
#ifdef ISIC_WIFI_EDUROAM
/**
 * @brief Connect to a WPA2-Enterprise (Eduroam) network
 */
inline void connectEduroam(const char *ssid, const char *username, const char *password)
{
    WiFi.begin(ssid, WPA2_AUTH_PEAP, username, username, password);
}
#endif
} // namespace isic::platform

// ============================================================================
// ESP8266 Implementation
// ============================================================================

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

#include <ESP8266WiFi.h>
#include <user_interface.h> // ESP8266 low-level WiFi control (wifi_set_sleep_type)
#ifdef ISIC_WIFI_EDUROAM
extern "C" {
#include "wpa2_enterprise.h"
}
#endif

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

#ifdef ISIC_WIFI_EDUROAM
/**
 * @brief Connect to a WPA2-Enterprise (Eduroam) network
 *
 * ESP8266 requires low-level SDK calls instead of WiFi.begin()
 * 
 * @note is not really safe to call low Level SDK functions with external C linkage
 */
inline void connectEduroam(const char *ssid, const char *username, const char *password)
{
    wifi_set_opmode(STATION_MODE);

    struct station_config wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strncpy(reinterpret_cast<char *>(wifi_config.ssid), ssid, sizeof(wifi_config.ssid) - 1);
    wifi_station_set_config(&wifi_config);

    wifi_station_clear_cert_key();
    wifi_station_clear_enterprise_ca_cert();
    wifi_station_set_wpa2_enterprise_auth(1);

    const auto usernameLen{strlen(username)};
    const auto passwordLen{strlen(password)};
    wifi_station_set_enterprise_identity(reinterpret_cast<uint8 *>(const_cast<char *>(username)), usernameLen);
    wifi_station_set_enterprise_username(reinterpret_cast<uint8 *>(const_cast<char *>(username)), usernameLen);
    wifi_station_set_enterprise_password(reinterpret_cast<uint8 *>(const_cast<char *>(password)), passwordLen);

    wifi_station_connect();
}
#endif
} // namespace isic::platform

#else
#error "Unsupported platform: Define ARDUINO_ARCH_ESP32 or ARDUINO_ARCH_ESP8266"
#endif

#endif // ISIC_PLATFORM_WIFI_HPP
