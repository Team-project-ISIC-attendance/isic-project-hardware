#ifndef ISIC_PLATFORM_WIFI_HPP
#define ISIC_PLATFORM_WIFI_HPP

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_wifi.h>

namespace isic::platform
{
inline bool isNetworkSecure(const std::uint8_t networkIndex)
{
    return WiFi.encryptionType(networkIndex) != WIFI_AUTH_OPEN;
}

inline void setWiFiLightSleep()
{
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

inline void wiFiPowerDown()
{
    // ESP32: WiFi.mode(WIFI_OFF) already powers down the WiFi
    // No additional action needed
}

inline void wiFiPowerUp()
{
    // ESP32: WiFi will wake when we set mode back to STA
    // No additional action needed
}
}

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

#include <ESP8266WiFi.h>
#include <user_interface.h> // ESP8266 low-level WiFi control (wifi_set_sleep_type)

namespace isic::platform
{
inline bool isNetworkSecure(const std::uint8_t networkIndex)
{
    return WiFi.encryptionType(networkIndex) != ENC_TYPE_NONE;
}

inline void setWiFiLightSleep()
{
    wifi_set_sleep_type(LIGHT_SLEEP_T);
}

inline void wiFiPowerDown()
{
    WiFi.forceSleepBegin();
    delay(1);  // Required for forceSleepBegin to take effect
}

inline void wiFiPowerUp()
{
    WiFi.forceSleepWake();
    delay(1);
}
}

#else
#error "Unsupported platform"
#endif

#endif // ISIC_PLATFORM_WIFI_HPP
