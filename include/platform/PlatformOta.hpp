#ifndef ISIC_PLATFORM_OTA_HPP
#define ISIC_PLATFORM_OTA_HPP

/**
 * @file PlatformOta.hpp
 * @brief OTA platform helpers for ESP8266/ESP32
 *
 * Provides a small abstraction layer over OTA features that differ
 * between ESP8266 and ESP32, including HTTPClient includes,
 * Update library includes, and platform constants.
 */

#include <cstdint>

// ============================================================================
// ESP32 Implementation
// ============================================================================

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>

namespace isic::platform
{

inline constexpr const char *kBoardName{"esp32dev"};
inline constexpr std::uint32_t kUpdateSizeUnknown{UPDATE_SIZE_UNKNOWN};

} // namespace isic::platform

// ============================================================================
// ESP8266 Implementation
// ============================================================================

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <WiFiClient.h>

namespace isic::platform
{

inline constexpr const char *kBoardName{"esp8266"};
inline constexpr std::uint32_t kUpdateSizeUnknown{0U};

} // namespace isic::platform

#else
#error "Unsupported platform: Define ARDUINO_ARCH_ESP32 or ARDUINO_ARCH_ESP8266"
#endif

#endif // ISIC_PLATFORM_OTA_HPP
