#ifndef HARDWARE_RESULT_HPP
#define HARDWARE_RESULT_HPP

/**
 * @file Result.hpp
 * @brief Error handling types for C++20 embedded systems.
 *
 * This file provides a lightweight `Result<T>` type similar to C++23's
 * `std::expected`, but compatible with C++20 and suitable for embedded
 * systems where exceptions are disabled.
 */

#include <cstdint>
#include <optional>

namespace isic {

    /**
     * @brief Error codes for operations throughout the system.
     *
     * Uses `enum class` with fixed underlying type for type safety
     * and predictable memory layout on embedded systems.
     */
    enum class ErrorCode : std::uint8_t {
        Ok = 0,
        StorageError,
        JsonError,
        NetworkError,
        MqttError,
        Timeout,
        InvalidArgument,
        NotInitialized,
        ResourceBusy,
        ResourceExhausted,
        HardwareError,
        ConfigError,
        AuthError,
        NotFound,
        AlreadyExists,
        OperationFailed,
        Cancelled,
        Unknown,

        Count  // Number of error codes
    };

    /**
     * @brief Get string representation of error code.
     * @param code Error code to convert
     * @return Null-terminated string literal (never nullptr)
     */
    [[nodiscard]] inline constexpr const char* toString(ErrorCode code) noexcept {
        switch (code) {
            case ErrorCode::Ok:                return "ok";
            case ErrorCode::StorageError:      return "storage_error";
            case ErrorCode::JsonError:         return "json_error";
            case ErrorCode::NetworkError:      return "network_error";
            case ErrorCode::MqttError:         return "mqtt_error";
            case ErrorCode::Timeout:           return "timeout";
            case ErrorCode::InvalidArgument:   return "invalid_argument";
            case ErrorCode::NotInitialized:    return "not_initialized";
            case ErrorCode::ResourceBusy:      return "resource_busy";
            case ErrorCode::ResourceExhausted: return "resource_exhausted";
            case ErrorCode::HardwareError:     return "hardware_error";
            case ErrorCode::ConfigError:       return "config_error";
            case ErrorCode::AuthError:         return "auth_error";
            case ErrorCode::NotFound:          return "not_found";
            case ErrorCode::AlreadyExists:     return "already_exists";
            case ErrorCode::OperationFailed:   return "operation_failed";
            case ErrorCode::Cancelled:         return "cancelled";
            default:                           return "unknown";
        }
    }

    /**
     * @brief Status result for operations that may fail.
     *
     * A lightweight alternative to exceptions for error handling.
     * Designed for embedded systems with minimal overhead.
     */
    struct Status {
        ErrorCode code{ErrorCode::Ok};
        const char* message{""};

        constexpr Status() noexcept = default;

        constexpr Status(const ErrorCode code, const char* message) noexcept : code(code), message(message) {

        }

        /**
         * @brief Check if operation was successful.
         */
        [[nodiscard]] constexpr bool ok() const noexcept {
            return code == ErrorCode::Ok;
        }

        /**
         * @brief Check if operation failed.
         */
        [[nodiscard]] constexpr bool failed() const noexcept {
            return code != ErrorCode::Ok;
        }

        /**
         * @brief Explicit conversion to bool.
         */
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return ok();
        }

        /**
         * @brief Create a success status.
         */
        [[nodiscard]] static constexpr Status OK() noexcept {
            return Status{ErrorCode::Ok, ""};
        }

        /**
         * @brief Create an error status.
         */
        [[nodiscard]] static constexpr Status Error(ErrorCode code, const char* msg = "") noexcept {
            return Status{code, msg};
        }
    };

    /**
     * @brief Result type for operations that return a value or error.
     *
     * A C++20 alternative to C++23's `std::expected<T, E>`.
     * Provides value-or-error semantics without exceptions.
     *
     * @tparam T The type of the value on success
     *
     * Example usage:
     * @code
     * Result<int> divide(int a, int b) noexcept {
     *     if (b == 0) {
     *         return Result<int>::Error(ErrorCode::InvalidArgument, "division by zero");
     *     }
     *     return Result<int>::Ok(a / b);
     * }
     *
     * auto result = divide(10, 2);
     * if (result.ok()) {
     *     int value = result.value;
     * }
     * @endcode
     */
    template<typename T>
    struct Result {
        Status status{Status::OK()};
        T value{};

        constexpr Result() noexcept = default;

        constexpr explicit Result(T val) noexcept : status(Status::OK()), value(std::move(val)) {
        }

        constexpr explicit Result(const Status err) noexcept : status(err), value{} {
        }

        constexpr Result(const ErrorCode code, const char* msg) noexcept : status(Status::Error(code, msg)), value{} {
        }

        /**
         * @brief Check if result contains a value.
         */
        [[nodiscard]] constexpr bool ok() const noexcept {
            return status.ok();
        }

        /**
         * @brief Check if result contains an error.
         */
        [[nodiscard]] constexpr bool failed() const noexcept {
            return status.failed();
        }

        /**
         * @brief Explicit conversion to bool.
         */
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return ok();
        }

        /**
         * @brief Get value or default if error.
         */
        [[nodiscard]] constexpr T valueOr(T defaultValue) const noexcept {
            return ok() ? value : defaultValue;
        }

        /**
         * @brief Create a success result with value.
         */
        [[nodiscard]] static constexpr Result<T> Ok(T val) noexcept {
            return Result<T>{std::move(val)};
        }

        /**
         * @brief Create an error result.
         */
        [[nodiscard]] static constexpr Result<T> Error(const ErrorCode code, const char* msg = "") noexcept {
            return Result<T>{Status::Error(code, msg)};
        }
    };

    /**
     * @brief Specialization for void results (just success/failure).
     *
     * Use when a function can fail but doesn't return a value.
     */
    template<>
    struct Result<void> {
        Status status{Status::OK()};

        constexpr Result() noexcept = default;

        constexpr explicit Result(const Status s) noexcept : status(s) {
        }

        constexpr Result(const ErrorCode code, const char* msg) noexcept : status(Status::Error(code, msg)) {
        }

        [[nodiscard]] constexpr bool ok() const noexcept {
            return status.ok();
        }
        [[nodiscard]] constexpr bool failed() const noexcept {
            return status.failed();
        }
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return ok();
        }

        [[nodiscard]] static constexpr Result<void> Ok() noexcept {
            return Result<void>{};
        }
        [[nodiscard]] static constexpr Result<void> Error(const ErrorCode code, const char* msg = "") noexcept {
            return Result<void>{Status::Error(code, msg)};
        }
    };

}  // namespace isic

#endif  // HARDWARE_RESULT_HPP
