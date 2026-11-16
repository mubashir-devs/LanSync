#pragma once
// Explicit per-folder synchronization state machine.
//
//   Idle -> Scanning -> Comparing -> Queued -> Transferring -> Verifying
//        -> Completed | Conflict | Failed        (Paused is reachable from anywhere)
//
// Transitions are validated; the SyncManager drives it from observable facts
// (scan running, transfers active, conflicts open...) via moveTo().

#include <string_view>

#include "core/common/result.hpp"

namespace lansync::sync {

enum class SyncState {
    Idle,
    Scanning,
    Comparing,
    Queued,
    Transferring,
    Verifying,
    Completed,
    Conflict,
    Failed,
    Paused,
};

std::string_view toString(SyncState s);

// True if `from -> to` is a legal single step.
bool canTransition(SyncState from, SyncState to);

class SyncStateMachine {
public:
    [[nodiscard]] SyncState state() const { return state_; }

    // Single legal step or an error (state unchanged).
    Result<void> transition(SyncState to);

    // Moves to `to` using at most one intermediate Idle step. Always succeeds
    // for any pair of states (Idle is reachable from every state except via
    // Paused->Idle which is also legal).
    void moveTo(SyncState to);

private:
    SyncState state_{SyncState::Idle};
};

}  // namespace lansync::sync
