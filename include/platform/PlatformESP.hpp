#ifndef ISIC_PLATFORM_ESP_HPP
#define ISIC_PLATFORM_ESP_HPP

/**
 * @file PlatformESP.hpp
 * @brief ESP8266/ESP32 platform abstraction layer
 *
 * Provides unified API for platform-specific functionality across
 * ESP8266 and ESP32 microcontrollers. Includes chip identification,
 * memory management, RTC persistence, and deep sleep control.
 */

#include <Arduino.h>
#include <ctime>
#include <optional>

namespace isic::platform
{
/**
 * @brief Get current Unix timestamp in milliseconds
 *
 * Combines SNTP-synchronized time with millis() for sub-second precision.
 * Returns nullopt if time has not been synchronized yet.
 *
 * @return Unix timestamp in ms, or nullopt if not yet synchronized
 *
 * @note Requires SNTP to be configured and synchronized
 * @note Threshold: considers time valid only after 2020-09-13
 */
inline std::optional<std::uint64_t> getUnixTimeMs()
{
    const auto nowSec = static_cast<std::int64_t>(time(nullptr));

    // Valid only after SNTP sync (threshold: 2020-09-13)
    if (nowSec <= 1'600'000'000)
    {
        return std::nullopt;
    }

    return static_cast<std::uint64_t>(nowSec) * 1000ULL +
           static_cast<std::uint64_t>(millis() % 1000);
}

} // namespace isic::platform

// ============================================================================
// ESP32 Implementation
// ============================================================================

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <esp_sleep.h>
#include <esp_system.h>

namespace isic::platform
{
/// Get unique chip identifier from eFuse MAC
inline std::uint32_t getChipId()
{
    return static_cast<std::uint32_t>(ESP.getEfuseMac());
}

/// Get chip ID as uppercase hexadecimal string
inline String getChipIdHex()
{
    return String(getChipId(), HEX);
}

/**
 * @brief Calculate heap fragmentation percentage
 *
 * ESP32 approximation: 100 - (largest_free_block * 100 / total_free)
 *
 * @return Fragmentation 0-100% (0 = no fragmentation)
 */
inline std::uint32_t getHeapFragmentation()
{
    const std::uint32_t totalFree = ESP.getFreeHeap();
    const std::uint32_t largestBlock = ESP.getMaxAllocHeap();
    return (totalFree == 0) ? 0 : 100 - ((largestBlock * 100) / totalFree);
}

/// Get actual flash chip size in bytes
inline std::uint32_t getFlashChipRealSize()
{
    return ESP.getFlashChipSize();
}

/**
 * @brief Write data to RTC user memory
 * @return false (not supported on ESP32)
 * @note ESP32 uses NVS or RTC_DATA_ATTR instead
 */
inline bool rtcUserMemoryWrite(std::uint32_t offset, std::uint32_t *data, std::size_t size)
{
    (void) offset;
    (void) data;
    (void) size;
    return false;
}

/**
 * @brief Read data from RTC user memory
 * @return false (not supported on ESP32)
 * @note ESP32 uses NVS or RTC_DATA_ATTR instead
 */
inline bool rtcUserMemoryRead(std::uint32_t offset, std::uint32_t *data, std::size_t size)
{
    (void) offset;
    (void) data;
    (void) size;
    return false;
}

/**
 * @brief Enter deep sleep with timer wakeup
 *
 * @param sleepUs Sleep duration in microseconds
 *
 * @warning Does not return - resets on wakeup
 */
inline void deepSleep(std::uint64_t sleepUs)
{
    esp_sleep_enable_timer_wakeup(sleepUs);
    esp_deep_sleep_start();
}
} // namespace isic::platform

// ============================================================================
// ESP8266 Implementation
// ============================================================================

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

namespace isic::platform
{
/// Get unique chip identifier
inline std::uint32_t getChipId()
{
    return EspClass::getChipId();
}

/// Get chip ID as uppercase hexadecimal string
inline String getChipIdHex()
{
    return {getChipId(), HEX};
}

/// Get heap fragmentation percentage (0-100%)
inline std::uint32_t getHeapFragmentation()
{
    return EspClass::getHeapFragmentation();
}

/// Get actual flash chip size in bytes
inline std::uint32_t getFlashChipRealSize()
{
    return EspClass::getFlashChipRealSize();
}

/**
 * @brief Write data to RTC user memory (survives deep sleep)
 *
 * @param offset Memory offset in 4-byte blocks
 * @param data Pointer to data buffer
 * @param size Size in bytes (must be multiple of 4)
 * @return true on success
 *
 * @note ESP8266 has 512 bytes of RTC memory
 */
inline bool rtcUserMemoryWrite(std::uint32_t offset, std::uint32_t *data, std::size_t size)
{
    return EspClass::rtcUserMemoryWrite(offset, data, size);
}

/**
 * @brief Read data from RTC user memory
 *
 * @param offset Memory offset in 4-byte blocks
 * @param data Pointer to destination buffer
 * @param size Size in bytes (must be multiple of 4)
 * @return true on success
 */
inline bool rtcUserMemoryRead(std::uint32_t offset, std::uint32_t *data, std::size_t size)
{
    return EspClass::rtcUserMemoryRead(offset, data, size);
}

/**
 * @brief Enter deep sleep with timer wakeup
 *
 * @param sleepUs Sleep duration in microseconds
 * @param mode RF wakeup mode (default: WAKE_RF_DEFAULT)
 *
 * @warning Does not return - resets on wakeup
 */
inline void deepSleep(std::uint64_t sleepUs, int mode = WAKE_RF_DEFAULT)
{
    EspClass::deepSleep(sleepUs, static_cast<RFMode>(mode));
}
} // namespace isic::platform

#else
#error "Unsupported platform: Define ARDUINO_ARCH_ESP32 or ARDUINO_ARCH_ESP8266"
#endif

#endif // ISIC_PLATFORM_ESP_HPP
