#include <gtest/gtest.h>

#include "core/conflict/conflict.hpp"
#include "core/sync/diff.hpp"
#include "core/sync/sync_state.hpp"

using namespace lansync::sync;

// ---------------------------------------------------------------- state machine

TEST(SyncState, HappyPathIsLegal) {
    SyncStateMachine sm;
    for (auto s : {SyncState::Scanning, SyncState::Comparing, SyncState::Queued, SyncState::Transferring,
                   SyncState::Verifying, SyncState::Completed, SyncState::Idle}) {
        ASSERT_TRUE(sm.transition(s)) << toString(s);
        EXPECT_EQ(sm.state(), s);
    }
}

TEST(SyncState, IllegalTransitionsAreRejectedAndStateKept) {
    SyncStateMachine sm;
    EXPECT_FALSE(sm.transition(SyncState::Transferring));   // Idle -> Transferring skips the pipeline
    EXPECT_FALSE(sm.transition(SyncState::Verifying));
    EXPECT_EQ(sm.state(), SyncState::Idle);
    ASSERT_TRUE(sm.transition(SyncState::Scanning));
    EXPECT_FALSE(sm.transition(SyncState::Transferring));
    EXPECT_EQ(sm.state(), SyncState::Scanning);
}

TEST(SyncState, PauseIsAlwaysReachableAndResumeGoesThroughIdle) {
    for (auto from : {SyncState::Idle, SyncState::Scanning, SyncState::Transferring, SyncState::Conflict}) {
        EXPECT_TRUE(canTransition(from, SyncState::Paused));
    }
    SyncStateMachine sm;
    ASSERT_TRUE(sm.transition(SyncState::Paused));
    EXPECT_FALSE(sm.transition(SyncState::Transferring));
    EXPECT_FALSE(sm.transition(SyncState::Scanning));
    EXPECT_TRUE(sm.transition(SyncState::Idle));
}

TEST(SyncState, MoveToAlwaysSucceedsViaIdle) {
    SyncStateMachine sm;
    sm.moveTo(SyncState::Paused);
    sm.moveTo(SyncState::Scanning);    // Paused -> Idle -> Scanning
    EXPECT_EQ(sm.state(), SyncState::Scanning);
    sm.moveTo(SyncState::Transferring);   // Scanning -> Idle -> (Idle->Transferring illegal) stays Idle
    EXPECT_TRUE(sm.state() == SyncState::Transferring || sm.state() == SyncState::Idle);
}

// ---------------------------------------------------------------- diff

namespace {
EntryMeta file(const char* hash, std::initializer_list<std::pair<const char*, int>> vv, bool deleted = false) {
    EntryMeta e;
    e.hash = hash;
    e.deleted = deleted;
    for (auto [k, n] : vv) for (int i = 0; i < n; ++i) e.vv.bump(k);
    return e;
}
EntryMeta dir(std::initializer_list<std::pair<const char*, int>> vv, bool deleted = false) {
    auto e = file("", vv, deleted);
    e.isDir = true;
    return e;
}
}  // namespace

TEST(Diff, NewRemoteFileIsFetched) {
    EXPECT_EQ(decide(std::nullopt, file("h", {{"bb", 1}})), Action::Fetch);
    EXPECT_EQ(decide(std::nullopt, dir({{"bb", 1}})), Action::MakeDir);
    EXPECT_EQ(decide(std::nullopt, file("", {{"bb", 2}}, true)), Action::None);   // tombstone for unknown file
}

TEST(Diff, EqualOrLocalNewerDoesNothing) {
    EXPECT_EQ(decide(file("h", {{"aa", 1}}), file("h", {{"aa", 1}})), Action::None);
    EXPECT_EQ(decide(file("h2", {{"aa", 2}}), file("h", {{"aa", 1}})), Action::None);
}

TEST(Diff, RemoteStrictlyNewerFetchesOrDeletes) {
    EXPECT_EQ(decide(file("h1", {{"aa", 1}}), file("h2", {{"aa", 1}, {"bb", 1}})), Action::Fetch);
    EXPECT_EQ(decide(file("h1", {{"aa", 1}}), file("", {{"aa", 1}, {"bb", 1}}, true)), Action::ApplyDelete);
    EXPECT_EQ(decide(file("", {{"aa", 2}}, true), file("", {{"aa", 2}, {"bb", 1}}, true)), Action::MergeVersion);
}

TEST(Diff, RemoteNewerButSameContentJustMerges) {
    EXPECT_EQ(decide(file("same", {{"aa", 1}}), file("same", {{"aa", 1}, {"bb", 1}})), Action::MergeVersion);
}

TEST(Diff, ConcurrentDifferentContentIsConflict) {
    EXPECT_EQ(decide(file("h1", {{"aa", 2}}), file("h2", {{"aa", 1}, {"bb", 1}})), Action::Conflict);
}

TEST(Diff, ConcurrentIdenticalContentMergesWithoutConflict) {
    EXPECT_EQ(decide(file("same", {{"aa", 1}}), file("same", {{"bb", 1}})), Action::MergeVersion);
    EXPECT_EQ(decide(dir({{"aa", 1}}), dir({{"bb", 1}})), Action::MergeVersion);
}

TEST(Diff, EditVersusDeleteNeverLosesTheEdit) {
    // We edited, they deleted concurrently -> keep our edit.
    EXPECT_EQ(decide(file("h", {{"aa", 2}}), file("", {{"aa", 1}, {"bb", 1}}, true)), Action::KeepLocal);
    // We deleted, they edited concurrently -> their edit comes back.
    EXPECT_EQ(decide(file("", {{"aa", 2}}, true), file("h", {{"aa", 1}, {"bb", 1}})), Action::Fetch);
    EXPECT_EQ(decide(file("", {{"aa", 2}}, true), file("", {{"bb", 1}}, true)), Action::MergeVersion);
}

// ---------------------------------------------------------------- conflict naming

TEST(ConflictNaming, DeterministicAndKeepsExtension) {
    // 2023-11-14 22:13:20 UTC
    const auto n = lansync::conflict::conflictCopyPath("docs/report.final.txt", "abcdef0123456789", 1'700'000'000'000);
    EXPECT_EQ(n, "docs/report.final.sync-conflict-20231114-221320-abcdef01.txt");
    EXPECT_EQ(n, lansync::conflict::conflictCopyPath("docs/report.final.txt", "abcdef0123456789", 1'700'000'000'000));
}

TEST(ConflictNaming, HandlesNoExtensionDotfilesAndRoot) {
    using lansync::conflict::conflictCopyPath;
    EXPECT_EQ(conflictCopyPath("Makefile", "aa", 0), "Makefile.sync-conflict-19700101-000000-aa");
    EXPECT_EQ(conflictCopyPath(".bashrc", "aa", 0), ".bashrc.sync-conflict-19700101-000000-aa");
    EXPECT_EQ(conflictCopyPath("a.b/c", "aa", 0), "a.b/c.sync-conflict-19700101-000000-aa");
}

TEST(ConflictNaming, BothSidesPickTheSameWinner) {
    using lansync::conflict::remoteWins;
    // Device A sees (local=A, remote=B); device B sees (local=B, remote=A).
    for (auto [ma, mb] : {std::pair{100, 200}, {200, 100}, {150, 150}}) {
        const bool aThinksBWins = remoteWins(ma, "aa", mb, "bb");
        const bool bThinksAWins = remoteWins(mb, "bb", ma, "aa");
        EXPECT_NE(aThinksBWins, bThinksAWins) << "exactly one side must win";
    }
    EXPECT_TRUE(remoteWins(100, "aa", 200, "bb"));    // newer mtime wins
    EXPECT_TRUE(remoteWins(100, "aa", 100, "bb"));    // tie -> larger device id
}
