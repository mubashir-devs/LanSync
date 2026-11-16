// End-to-end tests: two complete engines (own data dirs, DB, identity) talking
// over real TLS on loopback, using real files and the real inotify watcher.
#include <gtest/gtest.h>

#include <fstream>
#include <iostream>
#include <set>
#include <mutex>
#include <thread>

#include "core/application/engine.hpp"
#include "core/common/crypto.hpp"
#include "core/common/path_safety.hpp"
#include "filesystem/scanner/scanner.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::app;
namespace fs = std::filesystem;

namespace {

std::uint16_t freeUdpPort() {
    boost::asio::io_context io;
    boost::asio::ip::udp::socket s(io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"), 0));
    return s.local_endpoint().port();
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void writeFile(const fs::path& p, const std::string& c) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << c;
}
template <typename Pred>
bool waitUntil(Pred p, int ms = 20000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    return p();
}

struct Node {
    support::TempDir dir;
    std::unique_ptr<Application> application;
    std::unique_ptr<Engine> engine;
    std::mutex m;
    std::vector<EngineEvent> events;
    std::string name;

    Node(const std::string& n, std::uint16_t udp, std::uint16_t peerUdp, bool watchers = true) : name(n) {
        AppConfig cfg;
        cfg.dataDir = dir.path() / "data";
        cfg.deviceName = n;
        application = std::move(*Application::create(cfg));
        EngineOptions o;
        o.bindAddress = "127.0.0.1";
        o.discoveryBindAddress = "127.0.0.1";
        o.discoveryMulticast = false;
        o.discoveryBroadcast = false;
        o.enableWatchers = watchers;
        o.discoveryTargets = {{boost::asio::ip::make_address_v4("127.0.0.1"), peerUdp}};
        o.adjustSettings = [udp](Settings& s) {
            s.listenPort = 0;
            s.discoveryPort = udp;
            s.announceIntervalSec = 1;
        };
        engine = std::move(*Engine::create(*application, o));
        engine->setEventHandler([this](const EngineEvent& e) {
            std::lock_guard l(m);
            events.push_back(e);
        });
        auto r = engine->start();
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
    }
    ~Node() { engine->stop(); }

    const std::string& id() const { return engine->deviceId(); }
    fs::path folder(const std::string& n) const { return dir.path() / "sync" / n; }
    std::vector<EngineEvent> eventsOf(EngineEvent::Kind k) {
        std::lock_guard l(m);
        std::vector<EngineEvent> out;
        for (const auto& e : events) if (e.kind == k) out.push_back(e);
        return out;
    }
    bool sees(const std::string& deviceId) {
        for (const auto& d : engine->snapshot().devices) if (d.id == deviceId && d.discovered) return true;
        return false;
    }
    bool paired(const std::string& deviceId) {
        return engine->call([&](Services& s) { return s.devices.isPaired(deviceId) && s.devices.trustedSession(deviceId); });
    }
    std::string addFolder(const std::string& folderName, const std::string& remote,
                          const std::string& direction = "bidirectional") {
        fs::create_directories(folder(folderName));
        auto r = engine->call([&](Services& s) { return s.sync.addFolder(folder(folderName), folderName, remote, direction); });
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return r ? *r : "";
    }
    std::vector<HistoryInfo> history() { return engine->snapshot().history; }
    bool historyHas(const std::string& op, const std::string& result, const std::string& detailPart = "") {
        for (const auto& h : history()) {
            if (h.operation == op && h.result == result && h.detail.find(detailPart) != std::string::npos) return true;
        }
        return false;
    }
};

void dump(Node& n) {
    std::cerr << "---- " << n.name << " files:\n";
    for (const auto& e : fs::recursive_directory_iterator(n.folder("docs")))
        std::cerr << "  " << fs::relative(e.path(), n.folder("docs")).string() << (e.is_regular_file() ? "  = '" + readAll(e.path()) + "'" : "") << "\n";
    auto snap = n.engine->snapshot();
    for (const auto& h : snap.history) std::cerr << "  H " << h.operation << " " << h.result << " " << h.path << " | " << h.detail << "\n";
    for (const auto& f : snap.folders) std::cerr << "  F " << f.state << " err=" << f.error << " pending=" << f.pendingCount << "\n";
    for (const auto& t : snap.transfers) std::cerr << "  T " << t.status << " " << t.path << " " << t.error << "\n";
    n.engine->call([&](Services&) {
        storage::FileRepository repo(n.application->database());
        for (const auto& f : snap.folders) {
            auto rows = repo.listFolder(f.id);
            for (const auto& [path, row] : *rows)
                std::cerr << "  R " << path << " del=" << row.deleted << " hash=" << row.hash.substr(0, 6) << " vv=" << row.versionVector << " mt=" << row.modifiedAt << "\n";
        }
        return 0;
    });
}

struct Pair {
    std::uint16_t pa = freeUdpPort(), pb = freeUdpPort();
    Node a, b;
    explicit Pair(bool watchers = true) : a("alice", pa, pb, watchers), b("bob", pb, pa, watchers) {}

    // Discover, pair with explicit approval on both sides.
    void pairUp() {
        ASSERT_TRUE(waitUntil([&] { return a.sees(b.id()) && b.sees(a.id()); })) << "discovery";
        auto r = a.engine->call([&](Services& s) { return s.devices.startPairing(b.id()); });
        ASSERT_TRUE(r) << r.error().message;
        ASSERT_TRUE(waitUntil([&] {
            return !a.eventsOf(EngineEvent::Kind::PairingRequested).empty() &&
                   !b.eventsOf(EngineEvent::Kind::PairingRequested).empty();
        })) << "pairing prompts";
        ASSERT_EQ(a.eventsOf(EngineEvent::Kind::PairingRequested)[0].pairing.code,
                  b.eventsOf(EngineEvent::Kind::PairingRequested)[0].pairing.code);
        a.engine->async([&](Services& s) { s.devices.decidePairing(b.id(), true); });
        b.engine->async([&](Services& s) { s.devices.decidePairing(a.id(), true); });
        ASSERT_TRUE(waitUntil([&] { return a.paired(b.id()) && b.paired(a.id()); })) << "pairing";
    }
    void share(const std::string& name = "docs") {
        a.addFolder(name, b.id());
        b.addFolder(name, a.id());
    }
};

}  // namespace

// The MVP acceptance workflow.
TEST(EndToEnd, DiscoverPairShareSyncModifyBack) {
    Pair p;
    p.pairUp();
    p.share();

    // A creates a file -> detected -> transferred -> verified on B.
    writeFile(p.a.folder("docs") / "hello.txt", "hello from alice");
    ASSERT_TRUE(waitUntil([&] { return fs::exists(p.b.folder("docs") / "hello.txt"); })) << "file never arrived";
    ASSERT_TRUE(waitUntil([&] { return readAll(p.b.folder("docs") / "hello.txt") == "hello from alice"; }));
    EXPECT_TRUE(waitUntil([&] { return p.b.historyHas("download", "success", "verified"); }));
    EXPECT_TRUE(waitUntil([&] { return p.a.historyHas("upload", "success"); }));   // completion visible on both

    // No partial/temp artifacts are left visible.
    EXPECT_FALSE(fs::exists(p.b.folder("docs") / "hello.txt.part"));

    // B modifies it -> comes back to A.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writeFile(p.b.folder("docs") / "hello.txt", "edited by bob");
    ASSERT_TRUE(waitUntil([&] { return readAll(p.a.folder("docs") / "hello.txt") == "edited by bob"; }));

    // A's replaced content was preserved as a version.
    auto snap = p.a.engine->snapshot();
    ASSERT_EQ(snap.folders.size(), 1u);
    auto versions = p.a.engine->call([&](Services& s) { return s.sync.listVersions(snap.folders[0].id, "hello.txt"); });
    ASSERT_TRUE(versions);
    EXPECT_GE(versions->size(), 1u);

    EXPECT_TRUE(waitUntil([&] { return p.a.engine->snapshot().stats.filesSynced >= 1; }));
}

TEST(EndToEnd, NestedDirectoriesDeletionAndRename) {
    Pair p;
    p.pairUp();
    p.share();
    writeFile(p.a.folder("docs") / "x/y/z/deep.txt", "deep");
    fs::create_directories(p.a.folder("docs") / "emptydir");
    ASSERT_TRUE(waitUntil([&] { return fs::exists(p.b.folder("docs") / "x/y/z/deep.txt") && fs::is_directory(p.b.folder("docs") / "emptydir"); }));

    // Rename on A -> B ends up with the new name only (content reused locally, no re-download needed).
    fs::rename(p.a.folder("docs") / "x/y/z/deep.txt", p.a.folder("docs") / "x/renamed.txt");
    ASSERT_TRUE(waitUntil([&] {
        return fs::exists(p.b.folder("docs") / "x/renamed.txt") && !fs::exists(p.b.folder("docs") / "x/y/z/deep.txt");
    }));
    EXPECT_EQ(readAll(p.b.folder("docs") / "x/renamed.txt"), "deep");

    // Delete on A -> removed on B, but kept in B's version history.
    fs::remove(p.a.folder("docs") / "x/renamed.txt");
    ASSERT_TRUE(waitUntil([&] { return !fs::exists(p.b.folder("docs") / "x/renamed.txt"); }));
    auto snap = p.b.engine->snapshot();
    auto versions = p.b.engine->call([&](Services& s) { return s.sync.listVersions(snap.folders[0].id, "x/renamed.txt"); });
    ASSERT_TRUE(versions);
    EXPECT_GE(versions->size(), 1u);
}

TEST(EndToEnd, ConcurrentEditsPreserveBothVersionsAndCanBeResolved) {
    Pair p;
    p.pairUp();
    p.share();
    writeFile(p.a.folder("docs") / "plan.txt", "original");
    ASSERT_TRUE(waitUntil([&] { return fs::exists(p.b.folder("docs") / "plan.txt"); }));
    ASSERT_TRUE(waitUntil([&] { return p.b.engine->snapshot().stats.filesSynced >= 1 && p.a.engine->snapshot().stats.filesSynced >= 1; }));

    // Edit on BOTH sides while B is paused (so neither sees the other's edit first).
    auto bFolderId = p.b.engine->snapshot().folders[0].id;
    ASSERT_TRUE(p.b.engine->call([&](Services& s) { return s.sync.setFolderPaused(bFolderId, true); }));
    writeFile(p.a.folder("docs") / "plan.txt", "alice version");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // make mtimes differ
    writeFile(p.b.folder("docs") / "plan.txt", "bob version, newer");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    ASSERT_TRUE(p.b.engine->call([&](Services& s) { return s.sync.setFolderPaused(bFolderId, false); }));

    // Both devices end with BOTH contents, nothing silently overwritten.
    auto bothKept = [&](Node& n) {
        bool original = false, other = false;
        std::string main = readAll(n.folder("docs") / "plan.txt");
        for (const auto& e : fs::directory_iterator(n.folder("docs"))) {
            if (!e.is_regular_file()) continue;
            const auto fname = e.path().filename().string();
            if (fname.find("sync-conflict") != std::string::npos) other = true;
            (void)original;
        }
        return other && (main == "alice version" || main == "bob version, newer");
    };
    if (!waitUntil([&] { return bothKept(p.a) && bothKept(p.b); }, 8000)) { dump(p.a); dump(p.b); FAIL() << "conflict copies missing"; }

    auto contents = [&](Node& n) {
        std::set<std::string> c;
        for (const auto& e : fs::directory_iterator(n.folder("docs"))) {
            if (e.is_regular_file()) c.insert(readAll(e.path()));
        }
        return c;
    };
    ASSERT_TRUE(waitUntil([&] { return contents(p.a) == std::set<std::string>{"alice version", "bob version, newer"} &&
                                       contents(p.b) == std::set<std::string>{"alice version", "bob version, newer"}; }));
    // Deterministic winner: the newer edit keeps the original name on BOTH devices.
    EXPECT_EQ(readAll(p.a.folder("docs") / "plan.txt"), "bob version, newer");
    EXPECT_EQ(readAll(p.b.folder("docs") / "plan.txt"), "bob version, newer");

    // Conflict is listed on both devices.
    ASSERT_TRUE(waitUntil([&] { return !p.a.engine->snapshot().conflicts.empty() && !p.b.engine->snapshot().conflicts.empty(); }));

    // User on A decides to keep ALICE's version; it replaces the original on both devices.
    auto conflicts = p.a.engine->snapshot().conflicts;
    const auto aliceSide = p.a.engine->call([&](Services& s) { return s.sync.resolveConflict(conflicts[0].id, "keep_local"); });
    ASSERT_TRUE(aliceSide) << aliceSide.error().message;
    ASSERT_TRUE(waitUntil([&] { return readAll(p.a.folder("docs") / "plan.txt") == "alice version"; }));
    if (!waitUntil([&] { return readAll(p.b.folder("docs") / "plan.txt") == "alice version"; }, 8000)) { dump(p.a); dump(p.b); FAIL() << "resolution did not propagate"; }
    EXPECT_TRUE(p.a.engine->snapshot().conflicts.empty());
}

TEST(EndToEnd, LargeFileTransfersIntactAndNeverShowsPartialFile) {
    Pair p;
    p.pairUp();
    p.share();
    std::string big(24 * 1024 * 1024 + 123, 'a');
    for (std::size_t i = 0; i < big.size(); i += 4099) big[i] = static_cast<char>(i * 7);
    const auto expected = crypto::sha256Hex(big);

    // Write elsewhere then move in, so the scanner never sees a half-written source.
    writeFile(p.a.folder("docs").parent_path() / "staging.bin", big);
    fs::rename(p.a.folder("docs").parent_path() / "staging.bin", p.a.folder("docs") / "big.bin");

    // While transferring, the destination name must never exist with partial content.
    bool sawPartial = false;
    const auto dest = p.b.folder("docs") / "big.bin";
    ASSERT_TRUE(waitUntil([&] {
        std::error_code ec;
        if (fs::exists(dest, ec)) {
            if (fs::file_size(dest, ec) != big.size()) sawPartial = true;
            return true;
        }
        return false;
    }, 40000));
    EXPECT_FALSE(sawPartial);
    ASSERT_TRUE(waitUntil([&] { auto h = crypto::hashFile(dest); return h && *h == expected; }, 40000));
}

TEST(EndToEnd, ResumesFromVerifiedPartFileAndRejectsCorruptOne) {
    Pair p(/*watchers=*/true);
    p.pairUp();
    // B shares the folder first, with nothing in it.
    p.b.addFolder("docs", p.a.id());
    std::string content(3 * 1024 * 1024, 'q');
    for (std::size_t i = 0; i < content.size(); i += 1013) content[i] = static_cast<char>(i);
    const auto hash = crypto::sha256Hex(content);

    // Plant a valid partial download for B (as if a transfer had been interrupted).
    const auto part = p.b.folder("docs") / ".lansync" / "tmp" /
                      (crypto::sha256Hex(std::string("big.bin:") + hash).substr(0, 24) + ".part");
    writeFile(part, content.substr(0, 1'500'000));

    writeFile(p.a.folder("docs").parent_path() / "s.bin", content);
    p.a.addFolder("docs", p.b.id());
    fs::rename(p.a.folder("docs").parent_path() / "s.bin", p.a.folder("docs") / "big.bin");

    ASSERT_TRUE(waitUntil([&] { auto h = crypto::hashFile(p.b.folder("docs") / "big.bin"); return h && *h == hash; }));
    EXPECT_TRUE(p.b.historyHas("download", "success", "resumed from 1500000"));

    // A corrupt part file is detected by the whole-file hash, discarded, and the file still arrives.
    std::string content2(2 * 1024 * 1024, 'r');
    content2[777] = 'x';
    const auto hash2 = crypto::sha256Hex(content2);
    const auto part2 = p.b.folder("docs") / ".lansync" / "tmp" /
                       (crypto::sha256Hex(std::string("second.bin:") + hash2).substr(0, 24) + ".part");
    writeFile(part2, std::string(1'000'000, 'Z'));   // wrong bytes
    writeFile(p.a.folder("docs").parent_path() / "t.bin", content2);
    fs::rename(p.a.folder("docs").parent_path() / "t.bin", p.a.folder("docs") / "second.bin");
    ASSERT_TRUE(waitUntil([&] { auto h = crypto::hashFile(p.b.folder("docs") / "second.bin"); return h && *h == hash2; }, 40000));
    EXPECT_TRUE(p.b.historyHas("download", "failure", "integrity") || p.b.historyHas("download", "failure", "hash"));
}

TEST(EndToEnd, ReconnectsAfterConnectionLossAndCatchesUp) {
    Pair p;
    p.pairUp();
    p.share();
    p.a.engine->call([&](Services& s) { if (auto* t = s.devices.trustedSession(p.b.id())) t->close(); });
    ASSERT_TRUE(waitUntil([&] { return !p.a.paired(p.b.id()) || !p.b.paired(p.a.id()); }, 3000));

    writeFile(p.a.folder("docs") / "while-offline.txt", "made while disconnected");
    p.a.engine->call([&](Services& s) { s.sync.syncNow("", false); });
    ASSERT_TRUE(waitUntil([&] { return fs::exists(p.b.folder("docs") / "while-offline.txt"); }, 30000)) << "no catch-up after reconnect";
}

TEST(EndToEnd, UnpairedDeviceCannotReceiveFoldersAndUnpairStopsSync) {
    Pair p;
    ASSERT_TRUE(waitUntil([&] { return p.a.sees(p.b.id()); }));
    fs::create_directories(p.a.folder("docs"));
    auto r = p.a.engine->call([&](Services& s) { return s.sync.addFolder(p.a.folder("docs"), "docs", p.b.id(), "bidirectional"); });
    ASSERT_FALSE(r);   // not paired: refused
    EXPECT_EQ(r.error().code, ErrorCode::SecurityError);

    p.pairUp();
    p.share();
    writeFile(p.a.folder("docs") / "one.txt", "1");
    ASSERT_TRUE(waitUntil([&] { return fs::exists(p.b.folder("docs") / "one.txt"); }));

    ASSERT_TRUE(p.b.engine->call([&](Services& s) { return s.devices.unpair(p.a.id()); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    writeFile(p.a.folder("docs") / "two.txt", "2");
    std::this_thread::sleep_for(std::chrono::seconds(4));
    EXPECT_FALSE(fs::exists(p.b.folder("docs") / "two.txt"));   // revoked: nothing flows anymore
}

TEST(EndToEnd, ReceiveOnlyFolderNeverPublishesLocalChanges) {
    Pair p;
    p.pairUp();
    p.a.addFolder("docs", p.b.id(), "send_only");
    p.b.addFolder("docs", p.a.id(), "receive_only");
    writeFile(p.a.folder("docs") / "from-a.txt", "A");
    ASSERT_TRUE(waitUntil([&] { return fs::exists(p.b.folder("docs") / "from-a.txt"); }));
    writeFile(p.b.folder("docs") / "local-only.txt", "B");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_FALSE(fs::exists(p.a.folder("docs") / "local-only.txt"));
}

TEST(EndToEnd, ForgedFileRequestsAreRefusedAndDoNotBreakSync) {
    Pair p;
    p.pairUp();
    p.share();
    writeFile(p.b.folder("docs").parent_path() / "secret.txt", "top secret");   // outside the shared folder

    // A (a paired peer) sends hostile requests directly at B.
    p.a.engine->call([&](Services& s) {
        auto* t = s.devices.trustedSession(p.b.id());
        ASSERT_TRUE(t);
        for (const char* path : {"../secret.txt", "/etc/passwd", "a/../../secret.txt", ".lansync/tmp/x", ""}) {
            t->send(protocol::type::kFileRequest, {{"folder", "docs"}, {"path", path}, {"transfer_id", 99}, {"offset", 0}});
        }
        t->send(protocol::type::kFileRequest, {{"folder", "not-shared"}, {"path", "x"}, {"transfer_id", 100}, {"offset", 0}});
        t->send(protocol::type::kFileRequest, {{"folder", "docs"}, {"path", "x"}, {"transfer_id", "bad"}, {"offset", 0}});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // The session may be closed for the malformed one; sync must recover.
    p.a.engine->call([&](Services& s) { s.sync.syncNow("", false); });
    writeFile(p.a.folder("docs") / "still-works.txt", "ok");
    ASSERT_TRUE(waitUntil([&] { return fs::exists(p.b.folder("docs") / "still-works.txt"); }, 30000));
    EXPECT_FALSE(fs::exists(p.a.folder("docs") / "secret.txt"));
}

TEST(EndToEnd, SettingsPersistAcrossRestart) {
    support::TempDir dir;
    AppConfig cfg;
    cfg.dataDir = dir.path();
    cfg.deviceName = "persist";
    std::string id;
    {
        auto application = std::move(*Application::create(cfg));
        EngineOptions o;
        o.bindAddress = "127.0.0.1";
        o.discoveryMulticast = o.discoveryBroadcast = false;
        o.adjustSettings = [](Settings& s) { s.listenPort = 0; s.discoveryPort = freeUdpPort(); };
        auto e = std::move(*Engine::create(*application, o));
        ASSERT_TRUE(e->start());
        id = e->deviceId();
        auto s = e->snapshot().settings;
        s.chunkSizeKb = 64;
        s.versionMaxCount = 3;
        ASSERT_TRUE(e->call([&](Services& sv) { return sv.engine.updateSettings(s); }));
        e->stop();
    }
    auto application = std::move(*Application::create(cfg));
    EngineOptions o;
    o.bindAddress = "127.0.0.1";
    o.discoveryMulticast = o.discoveryBroadcast = false;
    o.adjustSettings = [](Settings& s) { s.listenPort = 0; s.discoveryPort = freeUdpPort(); };
    auto e = std::move(*Engine::create(*application, o));
    ASSERT_TRUE(e->start());
    EXPECT_EQ(e->deviceId(), id);   // identity is stable across restarts
    EXPECT_EQ(e->snapshot().settings.chunkSizeKb, 64);
    EXPECT_EQ(e->snapshot().settings.versionMaxCount, 3);
    e->stop();
}
