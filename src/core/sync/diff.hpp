#pragma once
// Decides what to do with one remote index entry given our local record.
// Pure function: no I/O, fully unit-tested.

#include <optional>
#include <string>

#include "core/sync/version_vector.hpp"

namespace lansync::sync {

struct EntryMeta {
    bool isDir{false};
    std::int64_t size{0};
    std::int64_t mtimeMs{0};
    std::string hash;
    bool deleted{false};
    VersionVector vv;
};

enum class Action {
    None,           // nothing to do (equal, or we are newer: the peer will pull from us)
    Fetch,          // download the remote content
    MakeDir,        // create the directory
    ApplyDelete,    // remote deleted it (we hold the older state)
    MergeVersion,   // same state reached independently: just record merged vector
    KeepLocal,      // concurrent: remote deleted, local edited -> keep ours, resurrect
    Conflict,       // concurrent edits with different content
};

std::string_view toString(Action a);

Action decide(const std::optional<EntryMeta>& local, const EntryMeta& remote);

}  // namespace lansync::sync
