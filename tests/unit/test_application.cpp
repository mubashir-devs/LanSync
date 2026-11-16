#include <gtest/gtest.h>

#include "core/application/application.hpp"
#include "storage/database/migrator.hpp"
#include "test_support.hpp"

using namespace lansync;

TEST(Application, CreatesDataDirAndMigratedDatabase) {
    support::TempDir dir;
    app::AppConfig cfg;
    cfg.dataDir = dir.path() / "data";
    cfg.deviceName = "test-device";

    auto application = app::Application::create(cfg);
    ASSERT_TRUE(application) << application.error().message;
    EXPECT_TRUE(std::filesystem::exists(cfg.databasePath()));
    EXPECT_EQ((*application)->schemaVersion(),
              storage::Migrator::latestVersion(storage::builtinMigrations()));
}

TEST(Application, ReopeningExistingDatabaseAppliesNothingNew) {
    support::TempDir dir;
    app::AppConfig cfg;
    cfg.dataDir = dir.path();
    {
        auto first = app::Application::create(cfg);
        ASSERT_TRUE(first);
        ASSERT_TRUE((*first)->database().execute(
            "INSERT INTO settings(key,value) VALUES('k','v');"));
    }
    auto second = app::Application::create(cfg);
    ASSERT_TRUE(second);
    EXPECT_EQ(**(*second)->database().queryInt("SELECT COUNT(*) FROM settings;"), 1);
}

TEST(Application, RejectsEmptyDataDir) {
    app::AppConfig cfg;
    auto application = app::Application::create(cfg);
    ASSERT_FALSE(application);
    EXPECT_EQ(application.error().code, ErrorCode::InvalidArgument);
}

TEST(Application, DefaultsUseOsDataDirAndHostname) {
    auto cfg = app::AppConfig::defaults();
    EXPECT_FALSE(cfg.dataDir.empty());
    EXPECT_FALSE(cfg.deviceName.empty());
}
