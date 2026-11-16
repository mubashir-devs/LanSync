#pragma once
// RAII wrappers over the SQLite C API.
//
// Threading: a Database (and its Statements/Transactions) must be used from one
// thread at a time. Callers serialize access (a dedicated DB executor arrives
// with the repositories in Phase 3); the connection is opened in SQLite's
// serialized mode only as a safety net against handle corruption.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/common/result.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace lansync::storage {

class Database;

class Statement {
public:
    Statement(Statement&&) noexcept;
    Statement& operator=(Statement&&) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    // Parameter indices are 1-based, as in SQLite.
    Result<void> bindInt(int index, std::int64_t value);
    Result<void> bindReal(int index, double value);
    Result<void> bindText(int index, std::string_view value);
    Result<void> bindNull(int index);

    // true => a row is available; false => statement finished.
    Result<bool> step();
    Result<void> reset();

    // Column indices are 0-based.
    [[nodiscard]] std::int64_t columnInt(int index) const;
    [[nodiscard]] double columnReal(int index) const;
    [[nodiscard]] std::string columnText(int index) const;
    [[nodiscard]] bool columnIsNull(int index) const;

private:
    friend class Database;
    Statement(sqlite3* db, sqlite3_stmt* stmt) noexcept : db_(db), stmt_(stmt) {}
    Error lastError(const char* what) const;

    sqlite3* db_{nullptr};
    sqlite3_stmt* stmt_{nullptr};
};

class Database {
public:
    // Opens (creating if necessary) the database. Use ":memory:" for tests.
    // Applies connection pragmas: foreign keys ON, WAL journal (file DBs),
    // busy timeout, synchronous=NORMAL.
    static Result<Database> open(const std::filesystem::path& path);

    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    // Executes one or more statements without bound parameters.
    Result<void> execute(std::string_view sql);
    Result<Statement> prepare(std::string_view sql);

    // Convenience: first column of first row as integer (nullopt if no rows/NULL).
    Result<std::optional<std::int64_t>> queryInt(std::string_view sql);

    [[nodiscard]] std::int64_t lastInsertRowId() const;
    [[nodiscard]] int changes() const;
    [[nodiscard]] sqlite3* handle() const noexcept { return db_.get(); }

private:
    struct Closer { void operator()(sqlite3* db) const noexcept; };
    explicit Database(sqlite3* db) noexcept : db_(db) {}
    Error lastError(const char* what) const;

    std::unique_ptr<sqlite3, Closer> db_;
};

// BEGIN IMMEDIATE ... COMMIT. Rolls back on destruction unless commit() succeeded.
class Transaction {
public:
    static Result<Transaction> begin(Database& db);

    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    Result<void> commit();

private:
    explicit Transaction(Database& db) noexcept : db_(&db) {}
    Database* db_{nullptr};   // null once committed/moved-from
};

}  // namespace lansync::storage
