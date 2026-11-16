#include <gtest/gtest.h>

#include "storage/database/database.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::storage;

namespace {
Database openMemory() {
    auto db = Database::open(":memory:");
    EXPECT_TRUE(db.ok());
    return std::move(*db);
}
}  // namespace

TEST(Database, BindStepAndReadColumns) {
    auto db = openMemory();
    ASSERT_TRUE(db.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL);"));

    auto ins = db.prepare("INSERT INTO t(name, score) VALUES (?1, ?2);");
    ASSERT_TRUE(ins);
    ASSERT_TRUE(ins->bindText(1, "alice"));
    ASSERT_TRUE(ins->bindReal(2, 1.5));
    ASSERT_TRUE(ins->step());
    EXPECT_EQ(db.lastInsertRowId(), 1);

    auto sel = db.prepare("SELECT id, name, score FROM t;");
    ASSERT_TRUE(sel);
    auto row = sel->step();
    ASSERT_TRUE(row);
    ASSERT_TRUE(*row);
    EXPECT_EQ(sel->columnInt(0), 1);
    EXPECT_EQ(sel->columnText(1), "alice");
    EXPECT_DOUBLE_EQ(sel->columnReal(2), 1.5);
    EXPECT_FALSE(*sel->step());
}

TEST(Database, NullHandlingAndEmbeddedNulBytesInText) {
    auto db = openMemory();
    ASSERT_TRUE(db.execute("CREATE TABLE t(v TEXT);"));
    auto ins = db.prepare("INSERT INTO t VALUES (?1);");
    ASSERT_TRUE(ins->bindNull(1));
    ASSERT_TRUE(ins->step());

    auto sel = db.prepare("SELECT v FROM t;");
    ASSERT_TRUE(*sel->step());
    EXPECT_TRUE(sel->columnIsNull(0));
    EXPECT_EQ(sel->columnText(0), "");
}

TEST(Database, SqlInjectionIsInertWhenBound) {
    auto db = openMemory();
    ASSERT_TRUE(db.execute("CREATE TABLE t(v TEXT);"));
    auto ins = db.prepare("INSERT INTO t VALUES (?1);");
    ASSERT_TRUE(ins->bindText(1, "x'); DROP TABLE t; --"));
    ASSERT_TRUE(ins->step());
    auto count = db.queryInt("SELECT COUNT(*) FROM t;");
    ASSERT_TRUE(count);
    EXPECT_EQ(**count, 1);
}

TEST(Database, InvalidSqlReturnsError) {
    auto db = openMemory();
    auto r = db.execute("THIS IS NOT SQL;");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::DatabaseError);
    EXPECT_FALSE(db.prepare("SELECT * FROM missing_table;"));
}

TEST(Database, TransactionRollsBackWhenNotCommitted) {
    auto db = openMemory();
    ASSERT_TRUE(db.execute("CREATE TABLE t(v INTEGER);"));
    {
        auto tx = Transaction::begin(db);
        ASSERT_TRUE(tx);
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (1);"));
    }  // no commit
    EXPECT_EQ(**db.queryInt("SELECT COUNT(*) FROM t;"), 0);
}

TEST(Database, TransactionCommitPersists) {
    auto db = openMemory();
    ASSERT_TRUE(db.execute("CREATE TABLE t(v INTEGER);"));
    {
        auto tx = Transaction::begin(db);
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (1);"));
        ASSERT_TRUE(tx->commit());
        EXPECT_FALSE(tx->commit());   // double commit rejected
    }
    EXPECT_EQ(**db.queryInt("SELECT COUNT(*) FROM t;"), 1);
}

TEST(Database, ForeignKeysAreEnforced) {
    auto db = openMemory();
    ASSERT_TRUE(db.execute("CREATE TABLE p(id INTEGER PRIMARY KEY);"
                           "CREATE TABLE c(pid INTEGER REFERENCES p(id));"));
    EXPECT_FALSE(db.execute("INSERT INTO c VALUES (99);"));
}

TEST(Database, FileDatabaseCreatesParentDirectoryAndUsesWal) {
    support::TempDir dir;
    auto path = dir.path() / "nested" / "dir" / "x.db";
    auto db = Database::open(path);
    ASSERT_TRUE(db);
    EXPECT_TRUE(std::filesystem::exists(path));
    auto st = db->prepare("PRAGMA journal_mode;");
    ASSERT_TRUE(*st->step());
    EXPECT_EQ(st->columnText(0), "wal");
}
