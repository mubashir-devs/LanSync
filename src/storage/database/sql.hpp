#pragma once
// Small helpers over Statement so repositories stay declarative.

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <variant>

#include "storage/database/database.hpp"

namespace lansync::storage {

// A bindable value. Use sqlNull() or opt() for NULLs.
struct SqlValue {
    std::variant<std::nullptr_t, std::int64_t, double, std::string> v;
    SqlValue(std::nullptr_t) : v(nullptr) {}                          // NOLINT
    SqlValue(std::int64_t x) : v(x) {}                                 // NOLINT
    SqlValue(int x) : v(static_cast<std::int64_t>(x)) {}               // NOLINT
    SqlValue(unsigned x) : v(static_cast<std::int64_t>(x)) {}          // NOLINT
    SqlValue(bool x) : v(static_cast<std::int64_t>(x ? 1 : 0)) {}      // NOLINT
    SqlValue(double x) : v(x) {}                                       // NOLINT
    SqlValue(std::string x) : v(std::move(x)) {}                       // NOLINT
    SqlValue(const char* x) : v(std::string(x)) {}                     // NOLINT
    SqlValue(std::string_view x) : v(std::string(x)) {}                // NOLINT
    SqlValue(const std::optional<std::string>& x)                      // NOLINT
        : v(x ? SqlValue(*x).v : SqlValue(nullptr).v) {}
    SqlValue(const std::optional<std::int64_t>& x)                     // NOLINT
        : v(x ? SqlValue(*x).v : SqlValue(nullptr).v) {}
};

using SqlParams = std::initializer_list<SqlValue>;

Result<void> bindAll(Statement& st, SqlParams params);

// Prepares, binds and steps once (INSERT/UPDATE/DELETE).
Result<void> run(Database& db, std::string_view sql, SqlParams params = {});

// Calls `onRow` for every result row.
Result<void> query(Database& db, std::string_view sql, SqlParams params,
                   const std::function<void(Statement&)>& onRow);

// Column helper: NULL -> nullopt.
std::optional<std::string> optText(const Statement& st, int col);
std::optional<std::int64_t> optInt(const Statement& st, int col);

}  // namespace lansync::storage
