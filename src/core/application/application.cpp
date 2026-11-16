#include "core/application/application.hpp"

#include <system_error>
#include <utility>

#include "core/common/logging.hpp"
#include "platform/platform.hpp"
#include "storage/database/migrator.hpp"

namespace lansync::app {

AppConfig AppConfig::defaults() {
    AppConfig cfg;
    cfg.dataDir = platform::defaultDataDir();
    cfg.deviceName = platform::hostName();
    return cfg;
}

Application::Application(AppConfig config, std::shared_ptr<const IClock> clock,
                         storage::Database db, int schemaVersion)
    : config_(std::move(config)),
      clock_(std::move(clock)),
      database_(std::move(db)),
      schemaVersion_(schemaVersion) {}

Application::~Application() = default;

Result<std::unique_ptr<Application>> Application::create(AppConfig config,
                                                          std::shared_ptr<const IClock> clock) {
    if (config.dataDir.empty()) {
        return Error{ErrorCode::InvalidArgument, "data directory must not be empty"};
    }
    std::error_code ec;
    std::filesystem::create_directories(config.dataDir, ec);
    if (ec) {
        return Error{ErrorCode::IoError, "cannot create data directory '" +
                                             config.dataDir.string() + "': " + ec.message()};
    }

    auto db = storage::Database::open(config.databasePath());
    if (!db) return db.error();

    auto applied = storage::Migrator::migrate(*db, storage::builtinMigrations(), *clock);
    if (!applied) return applied.error();

    auto version = storage::Migrator::currentVersion(*db);
    if (!version) return version.error();

    LS_INFO("database ready at {} (schema v{}, {} migration(s) applied)",
            config.databasePath().string(), *version, *applied);

    return std::unique_ptr<Application>(
        new Application(std::move(config), std::move(clock), std::move(*db), *version));
}

}  // namespace lansync::app
