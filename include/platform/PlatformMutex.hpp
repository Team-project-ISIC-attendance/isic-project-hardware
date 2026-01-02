#ifndef ISIC_CORE_PLATFORMMUTEX_HPP
#define ISIC_CORE_PLATFORMMUTEX_HPP

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

#elif defined(ARDUINO_ARCH_ESP8266) || defined(ISIC_PLATFORM_ESP8266)

#include <Arduino.h>

namespace isic
{
/**
 * @brief Mutex implementation for ESP8266 using interrupt disabling
 *
 * @note This is a simple mutex that disables interrupts to protect critical sections.
 * @warning it is not suitable for use in multi-threaded environments.
 */
class Mutex
{
public:
    Mutex() = default;
    ~Mutex() = default;

    // Non-copyable
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;

    void lock() // NOLINT, disable static method warning
    {
        noInterrupts(); // Disable interrupts to protect critical sections
    }

    void unlock() // NOLINT, disable static method warning
    {
        interrupts(); // Re-enable interrupts
    }

    bool try_lock()
    {
        lock(); // Always succeeds, since interrupts are disabled
        return true;
    }
};

/**
 * @brief Lock guard for Mutex
 *
 * @tparam MutexType Type of the mutex
 *
 * @note This class provides RAII-style locking for Mutex.
 * @warning Not suitable for multi-threaded environments.
 */
template<typename MutexType>
class LockGuard
{
public:
    explicit LockGuard(MutexType &mutex)
        : m_mutex(mutex)
    {
        m_mutex.lock();
    }

    ~LockGuard()
    {
        m_mutex.unlock();
    }

    // Non-copyable
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

private:
    MutexType &m_mutex;
};


/**
 * @brief Unique lock for Mutex
 *
 * @tparam MutexType Type of the mutex
 *
 * @note This class provides unique ownership semantics for Mutex.
 * @warning Not suitable for multi-threaded environments.
 */
template<typename MutexType>
class UniqueLock
{
public:
    UniqueLock()
        : m_mutex(nullptr)
        , owns_(false)
    {
    }

    explicit UniqueLock(MutexType &mutex)
        : m_mutex(&mutex)
        , owns_(true)
    {
        m_mutex->lock();
    }

    ~UniqueLock()
    {
        if (owns_)
        {
            unlock();
        }
    }

    // Non-copyable
    UniqueLock(const UniqueLock &) = delete;
    UniqueLock &operator=(const UniqueLock &) = delete;

    // Movable
    UniqueLock(UniqueLock &&other) noexcept
        : m_mutex(other.m_mutex)
        , owns_(other.owns_)
    {
        other.m_mutex = nullptr;
        other.owns_ = false;
    }

    UniqueLock &operator=(UniqueLock &&other) noexcept
    {
        if (this != &other)
        {
            if (owns_)
            {
                unlock();
            }
            m_mutex = other.m_mutex;
            owns_ = other.owns_;
            other.m_mutex = nullptr;
            other.owns_ = false;
        }
        return *this;
    }

    void lock()
    {
        if (!m_mutex)
        {
            return;
        }

        m_mutex->lock();
        owns_ = true;
    }

    void unlock()
    {
        if (!owns_)
        {
            return;
        }

        if (m_mutex)
        {
            m_mutex->unlock();
        }
        owns_ = false;
    }

    [[nodiscard]] bool owns_lock() const noexcept
    {
        return owns_;
    }

private:
    MutexType *m_mutex;
    bool owns_;
};
} // namespace isic

#else
#error "Unsupported platform"
#endif

#endif // ISIC_CORE_PLATFORMMUTEX_HPP
