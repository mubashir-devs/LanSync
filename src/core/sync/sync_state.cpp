#include "core/sync/sync_state.hpp"

#include <array>

namespace lansync::sync {

std::string_view toString(SyncState s) {
    switch (s) {
        case SyncState::Idle: return "Idle";
        case SyncState::Scanning: return "Scanning";
        case SyncState::Comparing: return "Comparing";
        case SyncState::Queued: return "Queued";
        case SyncState::Transferring: return "Transferring";
        case SyncState::Verifying: return "Verifying";
        case SyncState::Completed: return "Completed";
        case SyncState::Conflict: return "Conflict";
        case SyncState::Failed: return "Failed";
        case SyncState::Paused: return "Paused";
    }
    return "?";
}

bool canTransition(SyncState from, SyncState to) {
    using S = SyncState;
    if (from == to) return true;
    if (to == S::Paused) return true;   // pausing is always allowed
    switch (from) {
        case S::Idle: return to == S::Scanning || to == S::Comparing || to == S::Queued;
        case S::Scanning: return to == S::Comparing || to == S::Idle || to == S::Failed;
        case S::Comparing:
            return to == S::Queued || to == S::Idle || to == S::Conflict || to == S::Completed ||
                   to == S::Failed;
        case S::Queued:
            return to == S::Transferring || to == S::Idle || to == S::Completed || to == S::Failed ||
                   to == S::Conflict;
        case S::Transferring:
            return to == S::Verifying || to == S::Queued || to == S::Failed || to == S::Completed ||
                   to == S::Idle;
        case S::Verifying:
            return to == S::Completed || to == S::Transferring || to == S::Queued ||
                   to == S::Failed || to == S::Conflict;
        case S::Completed:
            return to == S::Idle || to == S::Scanning || to == S::Comparing || to == S::Queued;
        case S::Conflict:
            return to == S::Idle || to == S::Scanning || to == S::Comparing || to == S::Queued ||
                   to == S::Completed;
        case S::Failed:
            return to == S::Idle || to == S::Scanning || to == S::Comparing || to == S::Queued;
        case S::Paused: return to == S::Idle;   // resume re-evaluates from Idle
    }
    return false;
}

Result<void> SyncStateMachine::transition(SyncState to) {
    if (!canTransition(state_, to)) {
        return Error{ErrorCode::InvalidArgument,
                     std::string("illegal sync transition ") + std::string(toString(state_)) +
                         " -> " + std::string(toString(to))};
    }
    state_ = to;
    return {};
}

void SyncStateMachine::moveTo(SyncState to) {
    if (canTransition(state_, to)) {
        state_ = to;
        return;
    }
    state_ = SyncState::Idle;   // every state can reach Idle (Paused->Idle included)
    if (canTransition(state_, to)) state_ = to;
}

}  // namespace lansync::sync
