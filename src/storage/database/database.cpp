#include "storage/database/database.hpp"

#include <sqlite3.h>

#include <limits>
#include <utility>

#include "core/common/logging.hpp"

namespace lansync::storage {

namespace {
Error makeError(sqlite3* db, const char* what) {
    std::string msg = what;
    msg += ": ";
    msg += db ? sqlite3_errmsg(db) : "no database handle";
    return Error{ErrorCode::DatabaseError, std::move(msg)};
}
}  // namespace

// ---------------------------------------------------------------- Statement

Statement::Statement(Statement&& o) noexcept
    : db_(std::exchange(o.db_, nullptr)), stmt_(std::exchange(o.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& o) noexcept {
    if (this != &o) {
        if (stmt_) sqlite3_finalize(stmt_);
        db_ = std::exchange(o.db_, nullptr);
        stmt_ = std::exchange(o.stmt_, nullptr);
    }
    return *this;
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Error Statement::lastError(const char* what) const { return makeError(db_, what); }

Result<void> Statement::bindInt(int index, std::int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) return lastError("bind int");
    return {};
}
Result<void> Statement::bindReal(int index, double value) {
    if (sqlite3_bind_double(stmt_, index, value) != SQLITE_OK) return lastError("bind real");
    return {};
}
Result<void> Statement::bindText(int index, std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Error{ErrorCode::InvalidArgument, "text value too large"};
    }
    if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        return lastError("bind text");
    }
    return {};
}
Result<void> Statement::bindNull(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) return lastError("bind null");
    return {};
}

Result<bool> Statement::step() {
    switch (sqlite3_step(stmt_)) {
        case SQLITE_ROW: return true;
        case SQLITE_DONE: return false;
        default: return lastError("step");
    }
}

Result<void> Statement::reset() {
    sqlite3_reset(stmt_);
    if (sqlite3_clear_bindings(stmt_) != SQLITE_OK) return lastError("clear bindings");
    return {};
}

std::int64_t Statement::columnInt(int i) const { return sqlite3_column_int64(stmt_, i); }
double Statement::columnReal(int i) const { return sqlite3_column_double(stmt_, i); }
bool Statement::columnIsNull(int i) const { return sqlite3_column_type(stmt_, i) == SQLITE_NULL; }
std::string Statement::columnText(int i) const {
    const auto* text = sqlite3_column_text(stmt_, i);
    if (!text) return {};
    const int bytes = sqlite3_column_bytes(stmt_, i);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

// ---------------------------------------------------------------- Database

void Database::Closer::operator()(sqlite3* db) const noexcept {
    if (db) sqlite3_close_v2(db);
}

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;
Database::~Database() = default;

Error Database::lastError(const char* what) const { return makeError(db_.get(), what); }

Result<Database> Database::open(const std::filesystem::path& path) {
    const bool inMemory = path == ":memory:";
    if (!inMemory && path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return Error{ErrorCode::IoError,
                         "cannot create database directory: " + ec.message()};
        }
    }

    sqlite3* raw = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(path.string().c_str(), &raw, flags, nullptr);
    Database db(raw);   // owns the handle even on failure (must be closed)
    if (rc != SQLITE_OK) return db.lastError("open database");

    sqlite3_busy_timeout(raw, 5000);
    if (auto r = db.execute("PRAGMA foreign_keys = ON;"); !r) return r.error();
    if (auto r = db.execute("PRAGMA synchronous = NORMAL;"); !r) return r.error();
    if (!inMemory) {
        if (auto r = db.execute("PRAGMA journal_mode = WAL;"); !r) return r.error();
    }
    return db;
}

Result<void> Database::execute(std::string_view sql) {
    const std::string text(sql);   // sqlite3_exec needs NUL termination
    char* msg = nullptr;
    if (sqlite3_exec(db_.get(), text.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
        std::string m = msg ? msg : "unknown error";
        sqlite3_free(msg);
        return Error{ErrorCode::DatabaseError, "execute: " + m};
    }
    return {};
}

Result<Statement> Database::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql.data(), static_cast<int>(sql.size()), &stmt,
                           nullptr) != SQLITE_OK) {
        return lastError("prepare");
    }
    return Statement(db_.get(), stmt);
}

Result<std::optional<std::int64_t>> Database::queryInt(std::string_view sql) {
    auto stmt = prepare(sql);
    if (!stmt) return stmt.error();
    auto row = stmt->step();
    if (!row) return row.error();
    if (!*row || stmt->columnIsNull(0)) return std::optional<std::int64_t>{};
    return std::optional<std::int64_t>{stmt->columnInt(0)};
}

std::int64_t Database::lastInsertRowId() const { return sqlite3_last_insert_rowid(db_.get()); }
int Database::changes() const { return sqlite3_changes(db_.get()); }

// ------------------------------------------------------------- Transaction

Result<Transaction> Transaction::begin(Database& db) {
    if (auto r = db.execute("BEGIN IMMEDIATE;"); !r) return r.error();
    return Transaction(db);
}

Transaction::Transaction(Transaction&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)) {}

Transaction::~Transaction() {
    if (db_) {
        if (auto r = db_->execute("ROLLBACK;"); !r) {
            LS_ERROR("transaction rollback failed: {}", r.error().message);
        }
    }
}

Result<void> Transaction::commit() {
    if (!db_) return Error{ErrorCode::InvalidArgument, "transaction already finished"};
    auto r = db_->execute("COMMIT;");
    if (r) db_ = nullptr;   // on failure keep db_ so the destructor rolls back
    return r;
}

}  // namespace lansync::storage
