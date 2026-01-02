#ifndef ISIC_CORE_LOGGER_HPP
#define ISIC_CORE_LOGGER_HPP

#include <Arduino.h>

namespace isic::log
{
// Compile-time log level configuration
#ifndef ISIC_LOG_LEVEL
#ifdef ISIC_DEBUG
#define ISIC_LOG_LEVEL 1 // Debug
#else
#define ISIC_LOG_LEVEL 2 // Info
#endif
#endif

// TODO: now is loging to Serial, later must be in file that can read from web interface or serial.
// TODO: debug mode only with serial logging.
inline void logPrint(const char *level, const char *tag, const char *fmt, ...)
{
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.printf("[%6lu][%s][%s] %s\n", millis(), level, tag, buf);
}
}

#if ISIC_LOG_LEVEL <= 0
#define LOG_TRACE(tag, fmt, ...) isic::log::logPrint("T", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_TRACE(tag, fmt, ...) ((void) 0)
#endif

#if ISIC_LOG_LEVEL <= 1
#define LOG_DEBUG(tag, fmt, ...) isic::log::logPrint("D", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(tag, fmt, ...) ((void) 0)
#endif

#if ISIC_LOG_LEVEL <= 2
#define LOG_INFO(tag, fmt, ...) isic::log::logPrint("I", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_INFO(tag, fmt, ...) ((void) 0)
#endif

#if ISIC_LOG_LEVEL <= 3
#define LOG_WARN(tag, fmt, ...) isic::log::logPrint("W", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_WARN(tag, fmt, ...) ((void) 0)
#endif

#if ISIC_LOG_LEVEL <= 4
#define LOG_ERROR(tag, fmt, ...) isic::log::logPrint("E", tag, fmt, ##__VA_ARGS__)
#else
#define LOG_ERROR(tag, fmt, ...) ((void) 0)
#endif

#endif // ISIC_CORE_LOGGER_HPP