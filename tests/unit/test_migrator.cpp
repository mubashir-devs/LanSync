#include <gtest/gtest.h>

#include <vector>

#include "storage/database/migrator.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::storage;

namespace {
const std::vector<Migration> kTwo = {
    {1, "create_a", "CREATE TABLE a(id INTEGER PRIMARY KEY);"},
    {2, "create_b", "CREATE TABLE b(id INTEGER PRIMARY KEY);"},
};
}

TEST(Migrator, AppliesAllOnFreshDatabase) {
    auto db = Database::open(":memory:");
    support::FakeClock clock;
    auto applied = Migrator::migrate(*db, kTwo, clock);
    ASSERT_TRUE(applied);
    EXPECT_EQ(*applied, 2);
    EXPECT_EQ(*Migrator::currentVersion(*db), 2);
    EXPECT_TRUE(db->execute("INSERT INTO a DEFAULT VALUES; INSERT INTO b DEFAULT VALUES;"));
}

TEST(Migrator, IsIdempotent) {
    auto db = Database::open(":memory:");
    support::FakeClock clock;
    ASSERT_TRUE(Migrator::migrate(*db, kTwo, clock));
    auto again = Migrator::migrate(*db, kTwo, clock);
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, 0);
}

TEST(Migrator, AppliesOnlyPendingMigrations) {
    auto db = Database::open(":memory:");
    support::FakeClock clock;
    ASSERT_TRUE(Migrator::migrate(*db, std::span(kTwo).first(1), clock));
    auto r = Migrator::migrate(*db, kTwo, clock);
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, 1);
}

TEST(Migrator, RecordsAppliedTimestampFromInjectedClock) {
    auto db = Database::open(":memory:");
    support::FakeClock clock(123456);
    ASSERT_TRUE(Migrator::migrate(*db, kTwo, clock));
    EXPECT_EQ(**db->queryInt("SELECT applied_at FROM schema_migrations WHERE version = 1;"),
              123456);
}

TEST(Migrator, FailedMigrationRollsBackAndKeepsEarlierOnes) {
    auto db = Database::open(":memory:");
    support::FakeClock clock;
    const std::vector<Migration> bad = {
        {1, "ok", "CREATE TABLE a(id INTEGER);"},
        {2, "broken", "CREATE TABLE b(id INTEGER); THIS IS NOT SQL;"},
    };
    auto r = Migrator::migrate(*db, bad, clock);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::MigrationError);
    EXPECT_EQ(*Migrator::currentVersion(*db), 1);
    // The partially-run statement of migration 2 must not have persisted.
    EXPECT_FALSE(db->execute("INSERT INTO b DEFAULT VALUES;"));
}

TEST(Migrator, RefusesDatabaseNewerThanBinary) {
    auto db = Database::open(":memory:");
    support::FakeClock clock;
    ASSERT_TRUE(Migrator::migrate(*db, kTwo, clock));
    auto r = Migrator::migrate(*db, std::span(kTwo).first(1), clock);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::MigrationError);
}

TEST(Migrator, RejectsNonIncreasingVersions) {
    auto db = Database::open(":memory:");
    support::FakeClock clock;
    const std::vector<Migration> dup = {{1, "a", "SELECT 1;"}, {1, "b", "SELECT 1;"}};
    EXPECT_FALSE(Migrator::migrate(*db, dup, clock));
    const std::vector<Migration> zero = {{0, "a", "SELECT 1;"}};
    EXPECT_FALSE(Migrator::migrate(*db, zero, clock));
}

TEST(Migrator, BuiltinSchemaAppliesAndHasExpectedTables) {
    auto db = Database::open(":memory:");
    support::FakeClock clock;
    auto r = Migrator::migrate(*db, builtinMigrations(), clock);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_GE(*r, 1);
    for (const char* table : {"devices", "sync_folders", "files", "transfers", "file_versions",
                              "conflicts", "sync_history", "settings"}) {
        auto n = db->queryInt(std::string("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                                          "AND name='") + table + "';");
        ASSERT_TRUE(n);
        EXPECT_EQ(**n, 1) << "missing table " << table;
    }
}

TEST(Migrator, BuiltinSchemaConstraintsHold) {
    auto db = Database::open(":memory:");
    support::FakeClock clock;
    ASSERT_TRUE(Migrator::migrate(*db, builtinMigrations(), clock));
    // invalid pairing status rejected
    EXPECT_FALSE(db->execute("INSERT INTO devices(device_id,name,pairing_status,created_at) "
                             "VALUES('d','n','bogus',0);"));
    ASSERT_TRUE(db->execute("INSERT INTO devices(device_id,name,created_at) VALUES('d','n',0);"));
    ASSERT_TRUE(db->execute("INSERT INTO sync_folders(folder_id,local_path,display_name,"
                            "remote_device_id,created_at) VALUES('f','/x','x','d',0);"));
    ASSERT_TRUE(db->execute("INSERT INTO files(folder_id,relative_path,modified_at) "
                            "VALUES('f','a.txt',0);"));
    // duplicate path in same folder rejected
    EXPECT_FALSE(db->execute("INSERT INTO files(folder_id,relative_path,modified_at) "
                             "VALUES('f','a.txt',1);"));
    // cascade: removing the folder removes its files
    ASSERT_TRUE(db->execute("DELETE FROM sync_folders WHERE folder_id='f';"));
    EXPECT_EQ(**db->queryInt("SELECT COUNT(*) FROM files;"), 0);
}
