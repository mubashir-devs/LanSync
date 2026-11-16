#pragma once
// Explicit error handling without exceptions across module boundaries.
// (std::expected is C++23; this is a minimal C++20 equivalent.)

#include <string>
#include <utility>
#include <variant>

namespace lansync {

enum class ErrorCode {
    Ok = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    IoError,
    DatabaseError,
    MigrationError,
    ProtocolError,
    UnsupportedVersion,
    Busy,
    Conflict,
    SecurityError,
    Cancelled,
    Timeout,
    Internal,
};

struct Error {
    ErrorCode code{ErrorCode::Internal};
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string m) : code(c), message(std::move(m)) {}
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : data_(std::move(value)) {}              // NOLINT: implicit by design
    Result(Error error) : data_(std::move(error)) {}          // NOLINT: implicit by design

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<T>(data_); }
    const T& value() const& { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }
    T* operator->() { return &std::get<T>(data_); }
    const T* operator->() const { return &std::get<T>(data_); }
    T& operator*() & { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    const T& operator*() const& { return value(); }

    [[nodiscard]] const Error& error() const { return std::get<Error>(data_); }

private:
    std::variant<T, Error> data_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}   // NOLINT

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const Error& error() const { return error_; }

private:
    Error error_;
    bool ok_{true};
};

}  // namespace lansync
