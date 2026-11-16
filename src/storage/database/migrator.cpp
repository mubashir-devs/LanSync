#include "storage/database/migrator.hpp"

#include <string>

#include "core/common/logging.hpp"

namespace lansync::storage {

namespace {
constexpr std::string_view kCreateTracking =
    "CREATE TABLE IF NOT EXISTS schema_migrations ("
    "  version    INTEGER PRIMARY KEY,"
    "  name       TEXT NOT NULL,"
    "  applied_at INTEGER NOT NULL"
    ") STRICT;";

Result<void> validate(std::span<const Migration> migrations) {
    int previous = 0;
    for (const auto& m : migrations) {
        if (m.version <= previous) {
            return Error{ErrorCode::MigrationError,
                         "migrations must have strictly increasing versions >= 1 (got " +
                             std::to_string(m.version) + " after " + std::to_string(previous) + ")"};
        }
        previous = m.version;
    }
    return {};
}
}  // namespace

int Migrator::latestVersion(std::span<const Migration> migrations) {
    return migrations.empty() ? 0 : migrations.back().version;
}

Result<int> Migrator::currentVersion(Database& db) {
    if (auto r = db.execute(kCreateTracking); !r) return r.error();
    auto v = db.queryInt("SELECT MAX(version) FROM schema_migrations;");
    if (!v) return v.error();
    return static_cast<int>(v->value_or(0));
}

Result<int> Migrator::migrate(Database& db, std::span<const Migration> migrations,
                              const IClock& clock) {
    if (auto r = validate(migrations); !r) return r.error();

    auto current = currentVersion(db);
    if (!current) return current.error();

    const int latest = latestVersion(migrations);
    if (*current > latest) {
        return Error{ErrorCode::MigrationError,
                     "database schema version " + std::to_string(*current) +
                         " is newer than this application supports (" + std::to_string(latest) +
                         "); refusing to open it"};
    }

    int applied = 0;
    for (const auto& m : migrations) {
        if (m.version <= *current) continue;

        LS_INFO("applying migration {} ({})", m.version, m.name);
        auto tx = Transaction::begin(db);
        if (!tx) return tx.error();

        if (auto r = db.execute(m.sql); !r) {
            return Error{ErrorCode::MigrationError,
                         "migration " + std::to_string(m.version) + " (" + std::string(m.name) +
                             ") failed: " + r.error().message};
        }

        auto ins = db.prepare(
            "INSERT INTO schema_migrations(version, name, applied_at) VALUES (?1, ?2, ?3);");
        if (!ins) return ins.error();
        if (auto r = ins->bindInt(1, m.version); !r) return r.error();
        if (auto r = ins->bindText(2, m.name); !r) return r.error();
        if (auto r = ins->bindInt(3, clock.nowMs()); !r) return r.error();
        if (auto r = ins->step(); !r) return r.error();

        if (auto r = tx->commit(); !r) return r.error();
        ++applied;
    }
    return applied;
}

}  // namespace lansync::storage
