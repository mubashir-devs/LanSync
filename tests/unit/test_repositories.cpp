#include <gtest/gtest.h>

#include "storage/database/migrator.hpp"
#include "storage/repositories/repositories.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::storage;

namespace {
struct Db {
    Database db = std::move(*Database::open(":memory:"));
    Db() {
        support::FakeClock clock;
        auto r = Migrator::migrate(db, builtinMigrations(), clock);
        EXPECT_TRUE(r) << r.error().message;
    }
};

FolderRow folder(const std::string& id = "f1", const std::string& dev = "d1") {
    FolderRow f;
    f.id = id;
    f.localPath = "/tmp/" + id;
    f.displayName = "docs";
    f.remoteDeviceId = dev;
    f.createdAt = 1;
    return f;
}
void addDevice(Db& d, const std::string& id = "d1") {
    DeviceRow row;
    row.id = id;
    row.name = "peer";
    row.createdAt = 1;
    ASSERT_TRUE(DeviceRepository(d.db).upsert(row));
}
}  // namespace

TEST(Repositories, DeviceUpsertGetRevoke) {
    Db d;
    DeviceRepository repo(d.db);
    DeviceRow row;
    row.id = "abc";
    row.name = "Laptop";
    row.publicKeyPem = "PEM";
    row.fingerprint = "fp";
    row.pairingStatus = "paired";
    row.createdAt = 5;
    ASSERT_TRUE(repo.upsert(row));
    auto got = repo.get("abc");
    ASSERT_TRUE(got && *got);
    EXPECT_EQ((*got)->name, "Laptop");
    EXPECT_EQ((*got)->fingerprint, "fp");

    row.name = "Renamed";
    ASSERT_TRUE(repo.upsert(row));
    EXPECT_EQ((*repo.get("abc"))->name, "Renamed");
    EXPECT_EQ(repo.list()->size(), 1u);

    ASSERT_TRUE(repo.revoke("abc"));
    auto revoked = *repo.get("abc");
    EXPECT_EQ(revoked->pairingStatus, "revoked");
    EXPECT_FALSE(revoked->fingerprint);   // pin cleared: can never be trusted again without re-pairing
    EXPECT_FALSE((*repo.get("missing")).has_value());
}

TEST(Repositories, FoldersCrudAndShareLookup) {
    Db d;
    addDevice(d);
    FolderRepository repo(d.db);
    ASSERT_TRUE(repo.insert(folder()));
    EXPECT_FALSE(repo.insert(folder("f2")));   // same share name for the same device is ambiguous
    auto found = repo.findByShare("docs", "d1");
    ASSERT_TRUE(found && *found);
    EXPECT_EQ((*found)->id, "f1");
    EXPECT_FALSE(*repo.findByShare("docs", "other"));
    ASSERT_TRUE(repo.setEnabled("f1", false));
    EXPECT_FALSE((*repo.get("f1"))->enabled);
    ASSERT_TRUE(repo.remove("f1"));
    EXPECT_TRUE(repo.list()->empty());
}

TEST(Repositories, FileUpsertAssignsIdAndUpdatesInPlace) {
    Db d;
    addDevice(d);
    ASSERT_TRUE(FolderRepository(d.db).insert(folder()));
    FileRepository repo(d.db);
    FileRow f;
    f.folderId = "f1";
    f.path = "a/b.txt";
    f.size = 10;
    f.modifiedAt = 99;
    f.hash = "h1";
    ASSERT_TRUE(repo.upsert(f));
    EXPECT_GT(f.fileId, 0);
    const auto firstId = f.fileId;

    f.size = 20;
    f.hash = "h2";
    ASSERT_TRUE(repo.upsert(f));
    EXPECT_EQ(f.fileId, firstId);   // same row, not a duplicate
    auto got = *repo.get("f1", "a/b.txt");
    EXPECT_EQ(got->size, 20);
    EXPECT_EQ(got->hash, "h2");
    EXPECT_EQ(repo.listFolder("f1")->size(), 1u);
    EXPECT_TRUE((*repo.findLiveByHash("f1", "h2")).has_value());
    EXPECT_FALSE((*repo.findLiveByHash("f1", "h1")).has_value());
}

TEST(Repositories, FilePathsWithQuotesAreSafe) {
    Db d;
    addDevice(d);
    ASSERT_TRUE(FolderRepository(d.db).insert(folder()));
    FileRepository repo(d.db);
    FileRow f;
    f.folderId = "f1";
    f.path = "x'); DROP TABLE files; --.txt";
    f.modifiedAt = 1;
    ASSERT_TRUE(repo.upsert(f));
    EXPECT_TRUE((*repo.get("f1", f.path)).has_value());
    EXPECT_EQ(**d.db.queryInt("SELECT COUNT(*) FROM files;"), 1);
}

TEST(Repositories, TransfersVersionsConflictsHistorySettings) {
    Db d;
    addDevice(d);
    ASSERT_TRUE(FolderRepository(d.db).insert(folder()));
    FileRepository files(d.db);
    FileRow f;
    f.folderId = "f1";
    f.path = "a.txt";
    f.modifiedAt = 1;
    ASSERT_TRUE(files.upsert(f));

    TransferRepository tr(d.db);
    TransferRow t;
    t.id = "t1";
    t.folderId = "f1";
    t.path = "a.txt";
    t.direction = "download";
    t.sourceDeviceId = "d1";
    t.destDeviceId = "me";
    t.status = "transferring";
    t.totalBytes = 100;
    t.startedAt = 10;
    ASSERT_TRUE(tr.upsert(t));
    t.status = "completed";
    t.bytesTransferred = 100;
    ASSERT_TRUE(tr.upsert(t));
    EXPECT_EQ(tr.recent(10)->at(0).status, "completed");
    t.id = "t2";
    t.status = "transferring";
    ASSERT_TRUE(tr.upsert(t));
    EXPECT_EQ(*tr.failInterrupted(50), 1);

    VersionRepository vr(d.db);
    VersionRow v;
    v.fileId = f.fileId;
    v.hash = "h";
    v.size = 3;
    v.storageLocation = "x";
    v.createdAt = 7;
    ASSERT_TRUE(vr.insert(v));
    EXPECT_GT(v.versionId, 0);
    EXPECT_EQ(vr.listForFile(f.fileId)->size(), 1u);
    ASSERT_TRUE(vr.remove(v.versionId));
    EXPECT_FALSE((*vr.get(v.versionId)).has_value());

    ConflictRepository cr(d.db);
    ConflictRow c;
    c.fileId = f.fileId;
    c.localHash = "a";
    c.remoteHash = "b";
    c.detectedAt = 1;
    ASSERT_TRUE(cr.insert(c));
    EXPECT_EQ(cr.list(false)->size(), 1u);
    ASSERT_TRUE(cr.resolve(c.id, "keep_both", 9));
    EXPECT_EQ(cr.list(false)->size(), 0u);
    EXPECT_EQ(cr.list(true)->size(), 1u);
    EXPECT_FALSE(cr.resolve(c.id, "bogus", 9));   // CHECK constraint

    HistoryRepository hr(d.db);
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(hr.add({0, "d1", "a", "download", "success", "", i}));
    EXPECT_EQ(hr.recent(3)->size(), 3u);
    EXPECT_EQ(hr.recent(3)->at(0).timestamp, 9);   // newest first
    ASSERT_TRUE(hr.trim(4));
    EXPECT_EQ(hr.recent(100)->size(), 4u);

    SettingsRepository sr(d.db);
    ASSERT_TRUE(sr.set("k", "v1"));
    ASSERT_TRUE(sr.set("k", "v2"));
    EXPECT_EQ(sr.all()->at("k"), "v2");
}

TEST(Repositories, DeletingFolderCascadesToFiles) {
    Db d;
    addDevice(d);
    ASSERT_TRUE(FolderRepository(d.db).insert(folder()));
    FileRepository files(d.db);
    FileRow f;
    f.folderId = "f1";
    f.path = "a";
    f.modifiedAt = 1;
    ASSERT_TRUE(files.upsert(f));
    ASSERT_TRUE(FolderRepository(d.db).remove("f1"));
    EXPECT_EQ(**d.db.queryInt("SELECT COUNT(*) FROM files;"), 0);
}
