#include <gtest/gtest.h>

#include <fstream>

#include "core/common/crypto.hpp"
#include "core/sync/local_index.hpp"
#include "core/sync/version_vector.hpp"
#include "storage/database/sql.hpp"
#include "filesystem/scanner/scanner.hpp"
#include "storage/database/migrator.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::fsys;
namespace fs = std::filesystem;

namespace {
void write(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}
const FileMeta* find(const ScanOutput& o, const std::string& path) {
    for (const auto& e : o.entries) if (e.path == path) return &e;
    return nullptr;
}
}  // namespace

TEST(Scanner, ListsFilesAndDirsWithMetadataAndHash) {
    support::TempDir dir;
    write(dir.path() / "a.txt", "hello");
    write(dir.path() / "sub/deep/b.bin", "world");
    auto out = scanFolder(dir.path(), {});
    ASSERT_TRUE(out) << out.error().message;
    ASSERT_EQ(out->entries.size(), 4u);   // a.txt, sub, sub/deep, sub/deep/b.bin
    const auto* a = find(*out, "a.txt");
    ASSERT_TRUE(a);
    EXPECT_FALSE(a->isDir);
    EXPECT_EQ(a->size, 5);
    EXPECT_EQ(a->hash, crypto::sha256Hex("hello"));
    EXPECT_TRUE(find(*out, "sub")->isDir);
    EXPECT_TRUE(find(*out, "sub")->hash.empty());
    EXPECT_NE(find(*out, "sub/deep/b.bin"), nullptr);
}

TEST(Scanner, SkipsHashingWhenMetadataMatchesKnown) {
    support::TempDir dir;
    write(dir.path() / "a.txt", "hello");
    auto first = scanFolder(dir.path(), {});
    ASSERT_TRUE(first);
    EXPECT_EQ(first->bytesHashed, 5u);

    KnownMap known;
    const auto* a = find(*first, "a.txt");
    known["a.txt"] = {a->size, a->mtimeMs, "RECORDED-HASH"};
    auto second = scanFolder(dir.path(), known);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->bytesHashed, 0u);
    EXPECT_EQ(find(*second, "a.txt")->hash, "RECORDED-HASH");   // trusted, not recomputed
    EXPECT_FALSE(find(*second, "a.txt")->rehashed);

    ScanOptions opt;
    opt.rehashAll = true;   // content verification on demand
    auto third = scanFolder(dir.path(), known, opt);
    EXPECT_EQ(find(*third, "a.txt")->hash, crypto::sha256Hex("hello"));
}

TEST(Scanner, RehashesWhenSizeOrMtimeChanged) {
    support::TempDir dir;
    write(dir.path() / "a.txt", "hello");
    KnownMap known{{"a.txt", {5, 12345, "OLD"}}};   // mtime differs
    auto out = scanFolder(dir.path(), known);
    EXPECT_EQ(find(*out, "a.txt")->hash, crypto::sha256Hex("hello"));
}

TEST(Scanner, MissingRootIsAnErrorNotAnEmptyResult) {
    auto out = scanFolder("/definitely/not/here", {});
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code, ErrorCode::NotFound);
}

TEST(Scanner, SkipsSymlinksInternalDirAndReportsWarnings) {
    support::TempDir dir;
    write(dir.path() / "real.txt", "x");
    write(dir.path() / ".lansync/tmp/part", "partial");
    fs::create_symlink(dir.path() / "real.txt", dir.path() / "link.txt");
    auto out = scanFolder(dir.path(), {});
    ASSERT_TRUE(out);
    EXPECT_NE(find(*out, "real.txt"), nullptr);
    EXPECT_EQ(find(*out, "link.txt"), nullptr);
    EXPECT_EQ(find(*out, ".lansync"), nullptr);
    EXPECT_EQ(find(*out, ".lansync/tmp/part"), nullptr);
    bool warned = false;
    for (const auto& w : out->warnings) warned |= (w.path == "link.txt");
    EXPECT_TRUE(warned);
}

TEST(Scanner, UnreadableDirectoryIsReportedNotTreatedAsEmpty) {
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores permissions";
    support::TempDir dir;
    write(dir.path() / "locked/inside.txt", "x");
    fs::permissions(dir.path() / "locked", fs::perms::none);
    auto out = scanFolder(dir.path(), {});
    fs::permissions(dir.path() / "locked", fs::perms::owner_all);
    ASSERT_TRUE(out);
    ASSERT_EQ(out->unreadablePrefixes.size(), 1u);
    EXPECT_EQ(out->unreadablePrefixes[0], "locked");
}

TEST(Scanner, MtimeRoundTrip) {
    support::TempDir dir;
    write(dir.path() / "f", "x");
    ASSERT_TRUE(setFileMtimeMs(dir.path() / "f", 1'600'000'123'456));
    EXPECT_EQ(*fileMtimeMs(dir.path() / "f"), 1'600'000'123'456);
}

// ---------------------------------------------------------------- local index

namespace {
struct Index {
    storage::Database db = std::move(*storage::Database::open(":memory:"));
    storage::FileRepository files{db};
    Index() {
        support::FakeClock c;
        (void)storage::Migrator::migrate(db, storage::builtinMigrations(), c);
        (void)storage::run(db, "INSERT INTO sync_folders(folder_id,local_path,display_name,created_at) VALUES('f','/x','x',0);");
    }
};
}  // namespace

TEST(LocalIndex, DetectsCreateModifyDeleteAndBumpsVersionVector) {
    support::TempDir dir;
    Index idx;
    const std::string me = "aa";

    write(dir.path() / "a.txt", "one");
    auto scan1 = *scanFolder(dir.path(), {});
    auto ch1 = sync::applyScan(idx.db, "f", me, scan1);
    ASSERT_TRUE(ch1) << ch1.error().message;
    EXPECT_EQ(ch1->created, std::vector<std::string>{"a.txt"});
    auto row = **idx.files.get("f", "a.txt");
    EXPECT_EQ(sync::VersionVector::fromString(row.versionVector).get(me), 1u);

    // Unchanged -> no changes, no extra bump.
    auto known = sync::knownFromIndex(*idx.files.listFolder("f"));
    auto ch2 = sync::applyScan(idx.db, "f", me, *scanFolder(dir.path(), known));
    EXPECT_TRUE(ch2->empty());

    // Modify.
    write(dir.path() / "a.txt", "two!");
    known = sync::knownFromIndex(*idx.files.listFolder("f"));
    auto ch3 = sync::applyScan(idx.db, "f", me, *scanFolder(dir.path(), known));
    EXPECT_EQ(ch3->modified, std::vector<std::string>{"a.txt"});
    row = **idx.files.get("f", "a.txt");
    EXPECT_EQ(sync::VersionVector::fromString(row.versionVector).get(me), 2u);
    EXPECT_EQ(row.hash, crypto::sha256Hex("two!"));

    // Delete -> tombstone.
    fs::remove(dir.path() / "a.txt");
    known = sync::knownFromIndex(*idx.files.listFolder("f"));
    auto ch4 = sync::applyScan(idx.db, "f", me, *scanFolder(dir.path(), known));
    EXPECT_EQ(ch4->deleted, std::vector<std::string>{"a.txt"});
    row = **idx.files.get("f", "a.txt");
    EXPECT_TRUE(row.deleted);
    EXPECT_EQ(sync::VersionVector::fromString(row.versionVector).get(me), 3u);

    // Re-create -> alive again, counter keeps growing.
    write(dir.path() / "a.txt", "back");
    known = sync::knownFromIndex(*idx.files.listFolder("f"));
    auto ch5 = sync::applyScan(idx.db, "f", me, *scanFolder(dir.path(), known));
    EXPECT_EQ(ch5->created.size(), 1u);
    EXPECT_EQ(sync::VersionVector::fromString((**idx.files.get("f", "a.txt")).versionVector).get(me), 4u);
}

TEST(LocalIndex, TouchWithoutContentChangeDoesNotBump) {
    support::TempDir dir;
    Index idx;
    write(dir.path() / "a.txt", "same");
    ASSERT_TRUE(sync::applyScan(idx.db, "f", "aa", *scanFolder(dir.path(), {})));
    ASSERT_TRUE(setFileMtimeMs(dir.path() / "a.txt", 1'500'000'000'000));
    auto known = sync::knownFromIndex(*idx.files.listFolder("f"));
    auto ch = sync::applyScan(idx.db, "f", "aa", *scanFolder(dir.path(), known));
    EXPECT_TRUE(ch->empty());
    auto row = **idx.files.get("f", "a.txt");
    EXPECT_EQ(row.modifiedAt, 1'500'000'000'000);                       // metadata refreshed
    EXPECT_EQ(sync::VersionVector::fromString(row.versionVector).get("aa"), 1u);   // not a new version
}

TEST(LocalIndex, UnreadableSubtreeIsNeverMarkedDeleted) {
    support::TempDir dir;
    Index idx;
    write(dir.path() / "keep/a.txt", "x");
    ASSERT_TRUE(sync::applyScan(idx.db, "f", "aa", *scanFolder(dir.path(), {})));

    fsys::ScanOutput fake;   // pretend "keep" could not be listed this time
    fake.unreadablePrefixes.push_back("keep");
    auto ch = sync::applyScan(idx.db, "f", "aa", fake);
    ASSERT_TRUE(ch);
    EXPECT_TRUE(ch->deleted.empty());
    EXPECT_FALSE((**idx.files.get("f", "keep/a.txt")).deleted);
}

TEST(LocalIndex, UnstableFilesKeepTheirPreviousRecord) {
    support::TempDir dir;
    Index idx;
    write(dir.path() / "a.txt", "v1");
    ASSERT_TRUE(sync::applyScan(idx.db, "f", "aa", *scanFolder(dir.path(), {})));
    fsys::ScanOutput scan;
    scan.entries.push_back({"a.txt", false, 99, 1, "", /*unstable=*/true, false});
    auto ch = sync::applyScan(idx.db, "f", "aa", scan);
    EXPECT_TRUE(ch->empty());   // neither modified nor deleted
    EXPECT_FALSE((**idx.files.get("f", "a.txt")).deleted);
}
