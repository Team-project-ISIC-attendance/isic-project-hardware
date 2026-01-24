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
#include <cstring>
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

/// RTC memory buffer size in bytes (same as ESP8266's 512 bytes user memory)
constexpr std::size_t kRtcMemorySize{512};

/// RTC memory buffer that persists across deep sleep on ESP32
RTC_DATA_ATTR inline std::uint8_t rtcMemoryBuffer[kRtcMemorySize]{};

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
 * @brief Write data to RTC user memory (survives deep sleep)
 *
 * @param offset Memory offset in 4-byte blocks
 * @param data Pointer to data buffer
 * @param size Size in bytes (must be multiple of 4)
 * @return true on success, false if out of bounds
 *
 * @note Uses RTC_DATA_ATTR buffer on ESP32 for compatibility with ESP8266 API
 */
inline bool rtcUserMemoryWrite(std::uint32_t offset, std::uint32_t *data, std::size_t size)
{
    const std::size_t byteOffset = offset * sizeof(std::uint32_t);
    if (byteOffset + size > kRtcMemorySize)
    {
        return false;
    }
    std::memcpy(&rtcMemoryBuffer[byteOffset], data, size);
    return true;
}

/**
 * @brief Read data from RTC user memory
 *
 * @param offset Memory offset in 4-byte blocks
 * @param data Pointer to destination buffer
 * @param size Size in bytes (must be multiple of 4)
 * @return true on success, false if out of bounds
 *
 * @note Uses RTC_DATA_ATTR buffer on ESP32 for compatibility with ESP8266 API
 */
inline bool rtcUserMemoryRead(std::uint32_t offset, std::uint32_t *data, std::size_t size)
{
    const std::size_t byteOffset = offset * sizeof(std::uint32_t);
    if (byteOffset + size > kRtcMemorySize)
    {
        return false;
    }
    std::memcpy(data, &rtcMemoryBuffer[byteOffset], size);
    return true;
}

/**
 * @brief Configure GPIO wakeup for deep sleep (ESP32 only)
 *
 * @param gpio GPIO pin number (must be RTC GPIO: 0,2,4,12-15,25-27,32-39)
 * @param level Level to trigger wakeup (0 = LOW, 1 = HIGH)
 * @return true on success
 *
 * @note Call before deepSleep() to enable GPIO wakeup
 * @note On ESP32, this uses EXT0 wakeup which supports a single GPIO
 */
inline bool configureGpioWakeup(std::uint8_t gpio, std::uint8_t level = 0)
{
    const auto err = esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(gpio), level);
    return err == ESP_OK;
}

/**
 * @brief Check if GPIO wakeup is supported
 * @return true (ESP32 supports GPIO wakeup from deep sleep)
 */
inline constexpr bool supportsGpioWakeup()
{
    return true;
}

/**
 * @brief Enter deep sleep with timer wakeup
 *
 * @param sleepUs Sleep duration in microseconds (0 = indefinite, wake only on external)
 *
 * @warning Does not return - resets on wakeup
 * @note Call configureGpioWakeup() before this to enable GPIO wakeup
 */
inline void deepSleep(std::uint64_t sleepUs)
{
    if (sleepUs > 0)
    {
        esp_sleep_enable_timer_wakeup(sleepUs);
    }
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
 * @brief Configure GPIO wakeup for deep sleep
 *
 * @param gpio GPIO pin number (ignored on ESP8266)
 * @param level Level to trigger wakeup (ignored on ESP8266)
 * @return false (ESP8266 does not support GPIO wakeup from deep sleep)
 *
 * @note ESP8266 can only wake from deep sleep via timer or RST pin
 * @note For NFC wakeup, connect PN532 IRQ to RST pin via transistor
 */
inline bool configureGpioWakeup(std::uint8_t gpio, std::uint8_t level = 0)
{
    (void) gpio;
    (void) level;
    return false; // ESP8266 does not support GPIO wakeup from deep sleep
}

/**
 * @brief Check if GPIO wakeup is supported
 * @return false (ESP8266 does not support GPIO wakeup from deep sleep)
 */
inline constexpr bool supportsGpioWakeup()
{
    return false;
}

/**
 * @brief Enter deep sleep with timer wakeup
 *
 * @param sleepUs Sleep duration in microseconds
 * @param mode RF wakeup mode (default: WAKE_RF_DEFAULT)
 *
 * @warning Does not return - resets on wakeup
 * @note ESP8266 only supports timer and RST pin wakeup, not GPIO interrupt
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
