#include "core/sync/diff.hpp"

namespace lansync::sync {

std::string_view toString(Action a) {
    switch (a) {
        case Action::None: return "none";
        case Action::Fetch: return "fetch";
        case Action::MakeDir: return "mkdir";
        case Action::ApplyDelete: return "delete";
        case Action::MergeVersion: return "merge";
        case Action::KeepLocal: return "keep-local";
        case Action::Conflict: return "conflict";
    }
    return "?";
}

namespace {
bool sameContent(const EntryMeta& a, const EntryMeta& b) {
    if (a.deleted || b.deleted || a.isDir != b.isDir) return false;
    return a.isDir || a.hash == b.hash;
}
}  // namespace

Action decide(const std::optional<EntryMeta>& local, const EntryMeta& remote) {
    if (!local) {
        if (remote.deleted) return Action::None;
        return remote.isDir ? Action::MakeDir : Action::Fetch;
    }

    using Order = VersionVector::Order;
    switch (local->vv.compare(remote.vv)) {
        case Order::Equal:
        case Order::Greater:
            return Action::None;

        case Order::Less:   // remote strictly newer
            if (remote.deleted) return local->deleted ? Action::MergeVersion : Action::ApplyDelete;
            if (sameContent(*local, remote)) return Action::MergeVersion;
            return remote.isDir ? Action::MakeDir : Action::Fetch;

        case Order::Concurrent:
            if (remote.deleted && local->deleted) return Action::MergeVersion;
            if (remote.deleted) return Action::KeepLocal;           // we edited, they deleted
            if (local->deleted) return remote.isDir ? Action::MakeDir : Action::Fetch;   // they edited, we deleted
            if (sameContent(*local, remote)) return Action::MergeVersion;
            return Action::Conflict;
    }
    return Action::None;
}

}  // namespace lansync::sync
