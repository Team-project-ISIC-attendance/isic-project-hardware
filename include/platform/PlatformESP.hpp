#ifndef ISIC_PLATFORM_ESP_HPP
#define ISIC_PLATFORM_ESP_HPP

#include <Arduino.h>
#include <ctime>
#include <optional>

namespace isic::platform
{
inline std::optional<std::uint64_t> getUnixTimeMs()
{
    const auto nowSec{static_cast<std::int64_t>(time(nullptr))};

    // Consider time valid only after SNTP sets it (pick a sane epoch threshold: 2020-09-13)
    if (nowSec <= 1'600'000'000)
    {
        return std::nullopt;
    }

    return static_cast<std::uint64_t>(nowSec) * 1000ULL + static_cast<std::uint64_t>(millis() % 1000);
}
}

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <esp_sleep.h>
#include <esp_system.h>

namespace isic::platform
{
inline std::uint32_t getChipId()
{
    return static_cast<uint32_t>(ESP.getEfuseMac());
}

inline String getChipIdHex()
{
    return String(getChipId(), HEX);
}

inline std::uint32_t getHeapFragmentation()
{
    // ESP32 doesn't have getHeapFragmentation(), return 0 or calculate approximation: 100 - (largest_free_block * 100 / total_free)
    const std::uint32_t totalFree{ESP.getFreeHeap()};
    const std::uint32_t largestBlock{ESP.getMaxAllocHeap()};
    return (totalFree == 0) ? 0 : 100 - ((largestBlock * 100) / totalFree);
}

inline std::uint32_t getFlashChipRealSize()
{
    return ESP.getFlashChipSize();
}

inline bool rtcUserMemoryWrite(const std::uint32_t offset, std::uint32_t *data, const std::size_t size)
{
    (void)offset; // Unused
    (void)data; // Unused
    (void)size; // Unused

    // ESP32 doesn't have simple RTC user memory like ESP8266
    return false; // Not supported
}

inline bool rtcUserMemoryRead(const std::uint32_t offset, std::uint32_t *data, const std::size_t size)
{
    (void)offset; // Unused
    (void)data; // Unused
    (void)size; // Unused

    // ESP32 doesn't have simple RTC user memory like ESP8266
    return false; // Not supported
}

// Deep sleep function
inline void deepSleep(const std::uint64_t sleepUs)
{
    esp_sleep_enable_timer_wakeup(sleepUs);
    esp_deep_sleep_start();
}
}

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

namespace isic::platform
{
inline std::uint32_t getChipId()
{
    return EspClass::getChipId();
}

inline String getChipIdHex()
{
    return {getChipId(), HEX};
}

inline std::uint32_t getHeapFragmentation()
{
    return EspClass::getHeapFragmentation();
}

inline std::uint32_t getFlashChipRealSize()
{
    return EspClass::getFlashChipRealSize();
}

inline bool rtcUserMemoryWrite(const std::uint32_t offset, std::uint32_t *data, const std::size_t size)
{
    return EspClass::rtcUserMemoryWrite(offset, data, size);
}

inline bool rtcUserMemoryRead(const std::uint32_t offset, std::uint32_t *data, const std::size_t size)
{
    return EspClass::rtcUserMemoryRead(offset, data, size);
}

inline void deepSleep(const std::uint64_t sleepUs, const int mode = WAKE_RF_DEFAULT)
{
    EspClass::deepSleep(sleepUs, static_cast<RFMode>(mode));
}
}

#else
#error "Unsupported platform"
#endif

#endif // ISIC_PLATFORM_ESP_HPP
