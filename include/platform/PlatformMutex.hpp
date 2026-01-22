#ifndef ISIC_PLATFORM_MUTEX_HPP
#define ISIC_PLATFORM_MUTEX_HPP

/**
 * @file PlatformMutex.hpp
 * @brief Platform-specific mutex and lock implementations
 *
 * Provides portable synchronization primitives for ESP8266 and ESP32.
 * ESP32 uses standard library mutexes; ESP8266 uses interrupt disabling
 * due to its single-core, cooperative multitasking model.
 */

// ============================================================================
// ESP32 Implementation - Standard Library
// ============================================================================

#if defined(ARDUINO_ARCH_ESP32) || defined(ISIC_PLATFORM_ESP32)

#include <mutex>

namespace isic
{
using Mutex = std::mutex;

template<typename T>
using LockGuard = std::lock_guard<T>;

template<typename T>
using UniqueLock = std::unique_lock<T>;
} // namespace isic

// ============================================================================
// ESP8266 Implementation - Interrupt-Based
// ============================================================================

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

#include <Arduino.h>

namespace isic
{
/**
 * @class Mutex
 * @brief Mutex implementation using interrupt disabling
 *
 * ESP8266 is single-core with cooperative multitasking. Critical sections
 * are protected by disabling interrupts rather than using true mutexes.
 *
 * @warning Not suitable for multi-threaded environments
 * @warning Keep critical sections short to avoid interrupt latency
 */
class Mutex
{
public:
    Mutex() = default;
    ~Mutex() = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    /// Acquire lock by disabling interrupts
    void lock() { noInterrupts(); }  // NOLINT(readability-convert-member-functions-to-static)

    /// Release lock by re-enabling interrupts
    void unlock() { interrupts(); }  // NOLINT(readability-convert-member-functions-to-static)

    /// Always succeeds on single-core platform
    bool try_lock()
    {
        lock();
        return true;
    }
};

/**
 * @class LockGuard
 * @brief RAII lock guard for automatic mutex management
 *
 * Acquires mutex on construction, releases on destruction.
 * Non-copyable to prevent double-release bugs.
 *
 * @tparam MutexType Mutex type (must have lock()/unlock())
 *
 * @par Usage
 * @code
 * Mutex mtx;
 * {
 *     LockGuard<Mutex> lock(mtx);
 *     // Critical section
 * }  // Auto-unlock
 * @endcode
 */
template<typename MutexType>
class LockGuard
{
public:
    explicit LockGuard(MutexType& mutex)
        : m_mutex(mutex)
    {
        m_mutex.lock();
    }

    ~LockGuard() { m_mutex.unlock(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    MutexType& m_mutex;
};

/**
 * @class UniqueLock
 * @brief Movable lock with deferred/manual locking support
 *
 * Provides more flexible locking than LockGuard:
 * - Movable ownership semantics
 * - Manual lock/unlock control
 * - Deferred locking
 *
 * @tparam MutexType Mutex type (must have lock()/unlock())
 */
template<typename MutexType>
class UniqueLock
{
public:
    /// Construct without mutex (deferred)
    UniqueLock()
        : m_mutex(nullptr)
        , m_owns(false)
    {
    }

    /// Construct and immediately lock
    explicit UniqueLock(MutexType& mutex)
        : m_mutex(&mutex)
        , m_owns(true)
    {
        m_mutex->lock();
    }

    ~UniqueLock()
    {
        if (m_owns)
        {
            unlock();
        }
    }

    // Non-copyable
    UniqueLock(const UniqueLock&) = delete;
    UniqueLock& operator=(const UniqueLock&) = delete;

    // Movable
    UniqueLock(UniqueLock&& other) noexcept
        : m_mutex(other.m_mutex)
        , m_owns(other.m_owns)
    {
        other.m_mutex = nullptr;
        other.m_owns = false;
    }

    UniqueLock& operator=(UniqueLock&& other) noexcept
    {
        if (this != &other)
        {
            if (m_owns)
            {
                unlock();
            }
            m_mutex = other.m_mutex;
            m_owns = other.m_owns;
            other.m_mutex = nullptr;
            other.m_owns = false;
        }
        return *this;
    }

    /// Manually acquire lock
    void lock()
    {
        if (m_mutex)
        {
            m_mutex->lock();
            m_owns = true;
        }
    }

    /// Manually release lock
    void unlock()
    {
        if (m_owns && m_mutex)
        {
            m_mutex->unlock();
        }
        m_owns = false;
    }

    /// Check if currently holding lock
    [[nodiscard]] bool owns_lock() const noexcept { return m_owns; }

private:
    MutexType* m_mutex;
    bool m_owns;
};
} // namespace isic

#else
#error "Unsupported platform: Define ARDUINO_ARCH_ESP32 or ARDUINO_ARCH_ESP8266"
#endif

#endif // ISIC_PLATFORM_MUTEX_HPP
