#ifndef HARDWARE_LOGGER_HPP
#define HARDWARE_LOGGER_HPP

/**
 * @file Logger.hpp
 * @brief Logging utilities for embedded C++20 systems.
 *
 * Provides compile-time log level selection and type-safe logging macros.
 */

#include <Arduino.h>

#include <cstdint>

namespace isic {

    /**
     * @brief Log severity levels.
     *
     * Uses `enum class` with fixed underlying type for type safety.
     */
    enum class LogLevel : std::uint8_t {
        Trace,
        Debug,
        Info,
        Warn,
        Error,

        Count  // Sentinel for iteration
    };

    /**
     * @brief Convert log level to single-character string.
     * @param lvl Log level to convert
     * @return Single character string representation
     */
    [[nodiscard]] inline constexpr const char* toString(LogLevel lvl) noexcept {
        switch (lvl) {
            case LogLevel::Trace: return "T";
            case LogLevel::Debug: return "D";
            case LogLevel::Info:  return "I";
            case LogLevel::Warn:  return "W";
            case LogLevel::Error: return "E";
            default:              return "?";
        }
    }

}

/**
 * @brief Core logging macro - formats and prints to serial.
 *
 * @note Uses do-while(0) idiom for safe macro expansion.
 */
#define LOG_PRINT(level, tag, fmt, ...) \
    do { \
        Serial.printf("[%s][%s] " fmt "\n", isic::toString(level), tag, ##__VA_ARGS__); \
    } while (0)

// Convenience logging macros with level prefix
#define LOG_TRACE(tag, fmt, ...)   LOG_PRINT(isic::LogLevel::Trace, tag, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(tag, fmt, ...)   LOG_PRINT(isic::LogLevel::Debug, tag, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)    LOG_PRINT(isic::LogLevel::Info,  tag, fmt, ##__VA_ARGS__)
#define LOG_WARNING(tag, fmt, ...) LOG_PRINT(isic::LogLevel::Warn,  tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...)   LOG_PRINT(isic::LogLevel::Error, tag, fmt, ##__VA_ARGS__)

#endif  // HARDWARE_LOGGER_HPP
