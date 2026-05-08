#ifndef ISIC_PLATFORM_HAL_HPP
#define ISIC_PLATFORM_HAL_HPP

/**
 * @file HAL.hpp
 * @brief Cross-platform hardware abstraction for ESP8266, ESP32, and AVR (UNO).
 *
 * Provides a stable API for tone generation, watchdog feeding, restart,
 * and heap inspection that compiles on all three targets without modification
 * to call-sites.
 */

#include <Arduino.h>
#include <cstdint>

namespace isic::hal
{

// ─── Watchdog ────────────────────────────────────────────────────────────────

#if defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)
inline void wdtFeed() noexcept
{
    ESP.wdtFeed();
}
#elif defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)
inline void wdtFeed() noexcept
{
    // ESP32 FreeRTOS watchdog is fed by the idle task; explicit feed not needed
}
#elif defined(ARDUINO_ARCH_AVR) || defined(ISIC_STANDALONE_MODE)
#include <avr/wdt.h>
inline void wdtFeed() noexcept
{
    wdt_reset();
}
#else
inline void wdtFeed() noexcept {}
#endif

// ─── Restart ─────────────────────────────────────────────────────────────────

inline void restart() noexcept
{
#if defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266) || \
    defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)
    ESP.restart();
#elif defined(ARDUINO_ARCH_AVR) || defined(ISIC_STANDALONE_MODE)
    void (*resetFn)() = nullptr;
    resetFn();
#else
    ESP.restart();
#endif
}

// ─── Heap ────────────────────────────────────────────────────────────────────

[[nodiscard]] inline std::uint32_t getFreeHeap() noexcept
{
#if defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266) || \
    defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)
    return ESP.getFreeHeap();
#else
    return 0;
#endif
}

// ─── Tone ────────────────────────────────────────────────────────────────────

// Returns true if hardware PWM tone is available on this platform
[[nodiscard]] inline constexpr bool hasTone() noexcept
{
#if defined(ISIC_STANDALONE_MODE)
    return true; // AVR timer-based tone
#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266) || \
      defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)
    return true;
#else
    return false;
#endif
}

inline void playTone(const std::uint8_t pin, const std::uint16_t frequencyHz, const std::uint16_t durationMs = 0) noexcept
{
    if (!hasTone())
    {
        return;
    }
    if (durationMs > 0)
    {
        tone(pin, frequencyHz, durationMs);
    }
    else
    {
        tone(pin, frequencyHz);
    }
}

inline void stopTone(const std::uint8_t pin) noexcept
{
    if (hasTone())
    {
        noTone(pin);
        digitalWrite(pin, LOW);
    }
}

} // namespace isic::hal

#endif // ISIC_PLATFORM_HAL_HPP
