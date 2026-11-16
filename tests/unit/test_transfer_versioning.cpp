#include <gtest/gtest.h>

#include <fstream>
#include <set>

#include "core/common/crypto.hpp"
#include "core/common/worker_pool.hpp"
#include "core/transfer/chunk_writer.hpp"
#include "core/versioning/version_store.hpp"
#include "storage/database/migrator.hpp"
#include "storage/database/sql.hpp"
#include "test_support.hpp"

using namespace lansync;
namespace fs = std::filesystem;

namespace {
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& p, const std::string& c) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << c;
}
}  // namespace

// ---------------------------------------------------------------- ChunkWriter

TEST(ChunkWriter, ReceivesInChunksAndVerifies) {
    support::TempDir dir;
    const std::string content(100'000, 'z');
    auto w = transfer::ChunkWriter::createFresh(dir.path() / "p.part", content.size(), crypto::sha256Hex(content));
    ASSERT_TRUE(w);
    for (std::size_t off = 0; off < content.size(); off += 4096) {
        ASSERT_TRUE((*w)->write(std::string_view(content).substr(off, 4096)));
    }
    EXPECT_TRUE((*w)->complete());
    ASSERT_TRUE((*w)->finish());
    EXPECT_EQ(readAll(dir.path() / "p.part"), content);
}

TEST(ChunkWriter, RejectsMoreDataThanAnnounced) {
    support::TempDir dir;
    auto w = transfer::ChunkWriter::createFresh(dir.path() / "p", 5, crypto::sha256Hex("12345"));
    ASSERT_TRUE((*w)->write("1234"));
    EXPECT_FALSE((*w)->write("567"));
}

TEST(ChunkWriter, HashMismatchDeletesPartFile) {
    support::TempDir dir;
    auto w = transfer::ChunkWriter::createFresh(dir.path() / "p", 5, crypto::sha256Hex("12345"));
    ASSERT_TRUE((*w)->write("12346"));   // same size, different content
    auto r = (*w)->finish();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::SecurityError);
    EXPECT_FALSE(fs::exists(dir.path() / "p"));   // corrupt data is never kept for resume
}

TEST(ChunkWriter, IncompleteTransferFailsFinish) {
    support::TempDir dir;
    auto w = transfer::ChunkWriter::createFresh(dir.path() / "p", 10, crypto::sha256Hex("0123456789"));
    ASSERT_TRUE((*w)->write("0123"));
    EXPECT_FALSE((*w)->finish());
}

TEST(ChunkWriter, ResumeContinuesAndStillVerifiesWholeFile) {
    support::TempDir dir;
    const std::string content = "The quick brown fox jumps over the lazy dog";
    const auto part = dir.path() / "p.part";
    {
        auto w = transfer::ChunkWriter::createFresh(part, content.size(), crypto::sha256Hex(content));
        ASSERT_TRUE((*w)->write(content.substr(0, 17)));
        (*w)->abort(/*deletePart=*/false);   // interrupted
    }
    auto w = transfer::ChunkWriter::resume(part, content.size(), crypto::sha256Hex(content));
    ASSERT_TRUE(w);
    EXPECT_EQ((*w)->received(), 17u);
    ASSERT_TRUE((*w)->write(content.substr(17)));
    ASSERT_TRUE((*w)->finish());
    EXPECT_EQ(readAll(part), content);
}

TEST(ChunkWriter, ResumeOfTamperedPrefixIsCaughtAtVerification) {
    support::TempDir dir;
    const std::string content = "0123456789abcdef";
    const auto part = dir.path() / "p.part";
    write(part, "XXXXXXXX");   // corrupt prefix left on disk
    auto w = transfer::ChunkWriter::resume(part, content.size(), crypto::sha256Hex(content));
    ASSERT_TRUE(w);
    ASSERT_TRUE((*w)->write(content.substr(8)));
    EXPECT_FALSE((*w)->finish());   // whole-file hash catches it
}

TEST(ChunkWriter, OversizedPartIsDiscardedOnResume) {
    support::TempDir dir;
    const auto part = dir.path() / "p.part";
    write(part, "this is longer than expected");
    auto w = transfer::ChunkWriter::resume(part, 4, crypto::sha256Hex("abcd"));
    ASSERT_TRUE(w);
    EXPECT_EQ((*w)->received(), 0u);
}

// ---------------------------------------------------------------- VersionStore

namespace {
struct VS {
    support::TempDir dir;
    storage::Database db = std::move(*storage::Database::open(":memory:"));
    std::shared_ptr<support::FakeClock> clock = std::make_shared<support::FakeClock>(1'000'000'000'000);
    std::unique_ptr<versioning::VersionStore> store;
    storage::FileRow row;
    VS() {
        (void)storage::Migrator::migrate(db, storage::builtinMigrations(), *clock);
        (void)storage::run(db, "INSERT INTO sync_folders(folder_id,local_path,display_name,created_at) VALUES('f','/x','x',0);");
        store = std::make_unique<versioning::VersionStore>(dir.path() / "versions", db, clock);
        row.folderId = "f";
        row.path = "a.txt";
        row.modifiedAt = 5;
        (void)storage::FileRepository(db).upsert(row);
    }
    fs::path make(const std::string& content) {
        const auto p = dir.path() / "work" / "a.txt";
        write(p, content);
        row.hash = crypto::sha256Hex(content);
        row.size = static_cast<std::int64_t>(content.size());
        return p;
    }
};
}  // namespace

TEST(VersionStore, ArchiveCopyKeepsOriginalConsumeMovesIt) {
    VS vs;
    auto p = vs.make("v1");
    auto v1 = vs.store->archive(vs.row, p, /*consume=*/false);
    ASSERT_TRUE(v1) << v1.error().message;
    EXPECT_TRUE(fs::exists(p));
    EXPECT_EQ(readAll(vs.store->locate(*v1)), "v1");

    auto v2 = vs.store->archive(vs.row, p, /*consume=*/true);
    ASSERT_TRUE(v2);
    EXPECT_FALSE(fs::exists(p));
    EXPECT_EQ(storage::VersionRepository(vs.db).listForFile(vs.row.fileId)->size(), 2u);
}

TEST(VersionStore, ArchiveOfMissingFileFails) {
    VS vs;
    EXPECT_FALSE(vs.store->archive(vs.row, vs.dir.path() / "nope", true));
}

TEST(VersionStore, CopyOutRestoresAndVerifiesIntegrity) {
    VS vs;
    auto p = vs.make("precious");
    auto v = vs.store->archive(vs.row, p, true);
    ASSERT_TRUE(v);
    const auto out = vs.dir.path() / "restored.txt";
    ASSERT_TRUE(vs.store->copyOut(*v, out));
    EXPECT_EQ(readAll(out), "precious");

    write(vs.store->locate(*v), "tampered");   // corrupt the stored blob
    auto bad = vs.store->copyOut(*v, vs.dir.path() / "r2.txt");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::SecurityError);
}

TEST(VersionStore, PruneRespectsCountAndAgeAndNeverDeletesOtherwise) {
    VS vs;
    for (int i = 0; i < 5; ++i) {
        auto p = vs.make("content" + std::to_string(i));
        ASSERT_TRUE(vs.store->archive(vs.row, p, true));
        vs.clock->advance(24 * 3600 * 1000);   // one day apart
    }
    storage::VersionRepository repo(vs.db);
    EXPECT_EQ(*vs.store->prune({0, 0}), 0);                       // unlimited policy keeps all
    EXPECT_EQ(repo.listForFile(vs.row.fileId)->size(), 5u);

    EXPECT_EQ(*vs.store->prune({3, 0}), 2);                       // keep newest 3
    auto left = *repo.listForFile(vs.row.fileId);
    ASSERT_EQ(left.size(), 3u);
    EXPECT_EQ(readAll(vs.store->locate(left[0])), "content4");    // newest survives
    for (const auto& v : left) EXPECT_TRUE(fs::exists(vs.store->locate(v)));

    EXPECT_EQ(*vs.store->prune({0, 2}), 1);                       // versions older than 2 days
    EXPECT_EQ(repo.listForFile(vs.row.fileId)->size(), 2u);
}

TEST(VersionStore, PurgeFolderRemovesBlobs) {
    VS vs;
    auto p = vs.make("x");
    auto v = vs.store->archive(vs.row, p, true);
    ASSERT_TRUE(v);
    vs.store->purgeFolder("f");
    EXPECT_FALSE(fs::exists(vs.store->locate(*v)));
}

// ---------------------------------------------------------------- WorkerPool

TEST(WorkerPool, RunsTasksOnOtherThreadsAndJoinsCleanly) {
    std::atomic<int> n{0};
    std::set<std::thread::id> ids;
    std::mutex m;
    {
        WorkerPool pool(3);
        for (int i = 0; i < 50; ++i) {
            pool.post([&] {
                std::lock_guard l(m);
                ids.insert(std::this_thread::get_id());
                ++n;
            });
        }
        for (int i = 0; i < 200 && n < 50; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(n, 50);
    EXPECT_FALSE(ids.count(std::this_thread::get_id()));
}
