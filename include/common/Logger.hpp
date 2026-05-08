#ifndef ISIC_CORE_LOGGER_HPP
#define ISIC_CORE_LOGGER_HPP

#include <Arduino.h>
#include <cstdint>
#include <cstring>

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

// ─── In-memory ring buffer for recent log lines ───────────────────────────────
// 512 bytes in IRAM so it survives a heap crash and is accessible from ISR context.
// Only available on ESP targets — on AVR IRAM_ATTR is a no-op.

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

class RingLogger
{
public:
    static constexpr std::size_t kBufSize{512};

    static void append(const char *str) noexcept
    {
        const std::size_t n{strlen(str)};
        for (std::size_t i = 0; i < n; ++i)
        {
            s_buf[s_write] = str[i];
            s_write = (s_write + 1) % kBufSize;
            if (s_full || s_write == s_read)
            {
                s_full = true;
                s_read = (s_read + 1) % kBufSize; // Overwrite oldest byte
            }
        }
    }

    // Copy ring contents (oldest-first) into caller-supplied buf[outMax].
    // Returns number of bytes written (not NUL-terminated).
    static std::size_t copyTo(char *out, std::size_t outMax) noexcept
    {
        if (outMax == 0)
        {
            return 0;
        }
        std::size_t idx{s_full ? s_write : s_read};
        std::size_t total{s_full ? kBufSize : ((s_write - s_read + kBufSize) % kBufSize)};
        if (total > outMax - 1)
        {
            total = outMax - 1;
            idx = (s_write + kBufSize - total) % kBufSize;
        }
        for (std::size_t i = 0; i < total; ++i)
        {
            out[i] = s_buf[(idx + i) % kBufSize];
        }
        out[total] = '\0';
        return total;
    }

private:
    IRAM_ATTR static char s_buf[kBufSize];
    IRAM_ATTR static std::size_t s_write;
    IRAM_ATTR static std::size_t s_read;
    IRAM_ATTR static bool s_full;
};

inline IRAM_ATTR char RingLogger::s_buf[RingLogger::kBufSize]{};
inline IRAM_ATTR std::size_t RingLogger::s_write{0};
inline IRAM_ATTR std::size_t RingLogger::s_read{0};
inline IRAM_ATTR bool RingLogger::s_full{false};

// ─────────────────────────────────────────────────────────────────────────────

inline void logPrint(const char *level, const char *tag, const char *fmt, ...)
{
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    char line[160];
    snprintf(line, sizeof(line), "[%6lu][%s][%s] %s\n", millis(), level, tag, buf);

    Serial.print(line);
    RingLogger::append(line);
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