/**
 * @file result.h
 * @brief Monadic Result<T> type for error handling without exceptions.
 *
 * Pattern: all fallible operations return Result<T>.
 * Callers must explicitly check is_ok() before accessing value().
 * This eliminates hidden exception paths and forces error handling.
 */

#ifndef INSTALLER_CORE_RESULT_H
#define INSTALLER_CORE_RESULT_H

#include "installer/core/types.h"
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace installer {

/**
 * Result<T> — a type-safe alternative to exceptions.
 *
 * Usage:
 *   Result<int> r = compute_something();
 *   if (r.is_ok()) {
 *       use(r.value());
 *   } else {
 *       log_error(r.error());
 *   }
 *
 * For void results, use Result<void>.
 */
template <typename T>
class Result {
public:
    // ---- Construction ----
    static Result<T> ok(T value) {
        Result<T> r;
        r.value_ = std::move(value);
        r.has_value_ = true;
        return r;
    }

    static Result<T> err(InstallerError error) {
        Result<T> r;
        r.error_ = std::move(error);
        r.has_value_ = false;
        return r;
    }

    // Default: invalid state (neither ok nor err set)
    Result() = default;

    // Movable
    Result(Result&& other) noexcept
        : value_(std::move(other.value_))
        , error_(std::move(other.error_))
        , has_value_(other.has_value_) {}

    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            value_ = std::move(other.value_);
            error_ = std::move(other.error_);
            has_value_ = other.has_value_;
        }
        return *this;
    }

    // Copyable (if T is copyable)
    Result(const Result& other) = default;
    Result& operator=(const Result& other) = default;

    // ---- Queries ----
    bool is_ok() const { return has_value_; }
    bool is_err() const { return !has_value_; }
    explicit operator bool() const { return is_ok(); }

    // ---- Accessors ----
    T& value() { return value_.value(); }
    const T& value() const { return value_.value(); }

    T&& take_value() {
        has_value_ = false;
        return std::move(value_.value());
    }

    const InstallerError& error() const { return error_.value(); }
    InstallerError take_error() {
        auto e = std::move(error_.value());
        error_.reset();
        return e;
    }

    // ---- Monadic operations ----
    template <typename F>
    auto map(F&& f) -> Result<decltype(f(std::declval<T>()))> {
        using R = decltype(f(std::declval<T>()));
        if (is_ok()) {
            return Result<R>::ok(f(value()));
        }
        return Result<R>::err(take_error());
    }

    template <typename F>
    auto and_then(F&& f) -> decltype(f(std::declval<T>())) {
        using RetType = decltype(f(std::declval<T>()));
        if (is_ok()) {
            return f(value());
        }
        return RetType::err(take_error());
    }

    template <typename F>
    Result<T> or_else(F&& f) {
        if (is_err()) {
            return f(error());
        }
        return std::move(*this);
    }

private:
    std::optional<T> value_;
    std::optional<InstallerError> error_;
    bool has_value_ = false;
};

// ---- Result<void> Specialization ----
template <>
class Result<void> {
public:
    static Result<void> ok() {
        Result<void> r;
        r.has_value_ = true;
        return r;
    }

    static Result<void> err(InstallerError error) {
        Result<void> r;
        r.error_ = std::move(error);
        r.has_value_ = false;
        return r;
    }

    Result() = default;
    Result(Result&& other) noexcept = default;
    Result& operator=(Result&& other) noexcept = default;
    Result(const Result& other) = default;
    Result& operator=(const Result& other) = default;

    bool is_ok() const { return has_value_; }
    bool is_err() const { return !has_value_; }
    explicit operator bool() const { return is_ok(); }

    const InstallerError& error() const { return error_.value(); }
    InstallerError take_error() {
        auto e = std::move(error_.value());
        error_.reset();
        return e;
    }

    template <typename F>
    auto and_then(F&& f) -> decltype(f()) {
        using RetType = decltype(f());
        if (is_ok()) {
            return f();
        }
        return RetType::err(take_error());
    }

private:
    std::optional<InstallerError> error_;
    bool has_value_ = false;
};

} // namespace installer

#endif // INSTALLER_CORE_RESULT_H
