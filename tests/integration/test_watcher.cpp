#include <gtest/gtest.h>

#include <condition_variable>
#include <fstream>
#include <mutex>

#include "filesystem/watcher/watcher.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::fsys;
namespace fs = std::filesystem;

namespace {
struct Collector {
    std::mutex m;
    std::condition_variable cv;
    std::vector<WatchEvent> events;
    void add(const WatchEvent& e) {
        { std::lock_guard l(m); events.push_back(e); }
        cv.notify_all();
    }
    template <typename Pred>
    bool waitFor(Pred p, int ms = 3000) {
        std::unique_lock l(m);
        return cv.wait_for(l, std::chrono::milliseconds(ms), [&] {
            for (const auto& e : events) if (p(e)) return true;
            return false;
        });
    }
};
}  // namespace

class WatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        watcher = makeNativeWatcher();
        if (!watcher) GTEST_SKIP() << "no native watcher on this platform";
        ASSERT_TRUE(watcher->start(dir.path(), [this](const WatchEvent& e) { c.add(e); }));
    }
    void TearDown() override { if (watcher) watcher->stop(); }
    support::TempDir dir;
    Collector c;
    std::unique_ptr<IWatcher> watcher;
};

TEST_F(WatcherTest, ReportsFileCreateModifyDelete) {
    { std::ofstream(dir.path() / "a.txt") << "x"; }
    EXPECT_TRUE(c.waitFor([](auto& e) { return e.type == WatchEventType::FileCreated && e.path == "a.txt"; }));
    { std::ofstream(dir.path() / "a.txt", std::ios::app) << "more"; }
    EXPECT_TRUE(c.waitFor([](auto& e) { return e.type == WatchEventType::FileModified && e.path == "a.txt"; }));
    fs::remove(dir.path() / "a.txt");
    EXPECT_TRUE(c.waitFor([](auto& e) { return e.type == WatchEventType::FileDeleted && e.path == "a.txt"; }));
}

TEST_F(WatcherTest, WatchesNewlyCreatedNestedDirectories) {
    fs::create_directory(dir.path() / "d");
    EXPECT_TRUE(c.waitFor([](auto& e) { return e.type == WatchEventType::DirCreated && e.path == "d"; }));
    { std::ofstream(dir.path() / "d" / "inner.txt") << "x"; }   // needs a watch on the new dir
    EXPECT_TRUE(c.waitFor([](auto& e) { return e.path == "d/inner.txt"; }));
    fs::remove_all(dir.path() / "d");
    EXPECT_TRUE(c.waitFor([](auto& e) { return e.type == WatchEventType::DirDeleted && e.path == "d"; }));
}

TEST_F(WatcherTest, ReportsRenameWithOldAndNewPath) {
    { std::ofstream(dir.path() / "old.txt") << "x"; }
    ASSERT_TRUE(c.waitFor([](auto& e) { return e.path == "old.txt"; }));
    fs::rename(dir.path() / "old.txt", dir.path() / "new.txt");
    EXPECT_TRUE(c.waitFor([](auto& e) {
        return e.type == WatchEventType::Renamed && e.path == "new.txt" && e.oldPath == "old.txt";
    }));
}

TEST_F(WatcherTest, IgnoresInternalDirectory) {
    fs::create_directories(dir.path() / ".lansync");
    { std::ofstream(dir.path() / ".lansync" / "x") << "x"; }
    { std::ofstream(dir.path() / "visible.txt") << "x"; }
    ASSERT_TRUE(c.waitFor([](auto& e) { return e.path == "visible.txt"; }));
    std::lock_guard l(c.m);
    for (const auto& e : c.events) EXPECT_EQ(e.path.rfind(".lansync", 0), std::string::npos) << e.path;
}

TEST(WatcherStandalone, StartOnMissingDirectoryFails) {
    auto w = makeNativeWatcher();
    if (!w) GTEST_SKIP();
    EXPECT_FALSE(w->start("/no/such/dir", [](const WatchEvent&) {}));
}
