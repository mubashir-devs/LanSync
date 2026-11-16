#pragma once
// Forward-only schema migrations.
//
// * Each migration runs in its own transaction; a failure leaves the database at
//   the last successfully applied version.
// * Applied versions are recorded in `schema_migrations`.
// * A database that is NEWER than this binary knows about is refused rather than
//   silently used (prevents old builds corrupting new data).

#include <span>
#include <string_view>

#include "core/common/clock.hpp"
#include "core/common/result.hpp"
#include "storage/database/database.hpp"

namespace lansync::storage {

struct Migration {
    int version;
    std::string_view name;
    std::string_view sql;
};

// Migrations embedded from migrations/*.sql at build time (generated source).
std::span<const Migration> builtinMigrations();

class Migrator {
public:
    // Applies all pending migrations. Returns the number applied.
    static Result<int> migrate(Database& db, std::span<const Migration> migrations,
                               const IClock& clock);

    // Highest applied version, 0 for a fresh database.
    static Result<int> currentVersion(Database& db);

    // Version of the newest migration in the list (0 if empty).
    static int latestVersion(std::span<const Migration> migrations);
};

}  // namespace lansync::storage
