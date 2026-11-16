#pragma once
// Composition root of the core. Owns long-lived services and hands out
// references to them. It knows nothing about Qt or any UI: the desktop app and
// the headless CLI are both thin front-ends over this class.
//
// Later phases register DeviceManager, SyncManager, TransferEngine, etc. here.

#include <filesystem>
#include <memory>
#include <string>

#include "core/common/clock.hpp"
#include "core/common/result.hpp"
#include "storage/database/database.hpp"

namespace lansync::app {

struct AppConfig {
    std::filesystem::path dataDir;        // DB, certificates, versions store
    std::string deviceName;               // shown to other devices
    std::string logLevel{"info"};

    // Platform defaults (data dir from the OS, device name from hostname).
    static AppConfig defaults();

    [[nodiscard]] std::filesystem::path databasePath() const { return dataDir / "lansync.db"; }
    [[nodiscard]] std::filesystem::path logDir() const { return dataDir / "logs"; }
};

class Application {
public:
    // Creates the data directory, opens the database and applies migrations.
    // `clock` is injectable for tests.
    static Result<std::unique_ptr<Application>> create(
        AppConfig config, std::shared_ptr<const IClock> clock = std::make_shared<SystemClock>());

    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] const AppConfig& config() const noexcept { return config_; }
    [[nodiscard]] const IClock& clock() const noexcept { return *clock_; }
    [[nodiscard]] std::shared_ptr<const IClock> sharedClock() const noexcept { return clock_; }
    [[nodiscard]] storage::Database& database() noexcept { return database_; }
    [[nodiscard]] int schemaVersion() const noexcept { return schemaVersion_; }

private:
    Application(AppConfig config, std::shared_ptr<const IClock> clock, storage::Database db,
                int schemaVersion);

    AppConfig config_;
    std::shared_ptr<const IClock> clock_;
    storage::Database database_;
    int schemaVersion_;
};

}  // namespace lansync::app
