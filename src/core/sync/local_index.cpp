#include "core/sync/local_index.hpp"

#include <algorithm>
#include <set>

#include "core/sync/version_vector.hpp"

namespace lansync::sync {

using storage::FileRow;

std::vector<std::string> ChangeSet::all() const {
    std::vector<std::string> out;
    out.insert(out.end(), created.begin(), created.end());
    out.insert(out.end(), modified.begin(), modified.end());
    out.insert(out.end(), deleted.begin(), deleted.end());
    return out;
}

fsys::KnownMap knownFromIndex(const std::map<std::string, FileRow>& rows) {
    fsys::KnownMap known;
    for (const auto& [path, r] : rows) {
        if (r.deleted || r.isDir) continue;
        known[path] = {r.size, r.modifiedAt, r.hash};
    }
    return known;
}

namespace {
bool underPrefix(const std::string& path, const std::vector<std::string>& prefixes) {
    for (const auto& p : prefixes) {
        if (p.empty() || path == p || (path.size() > p.size() && path.compare(0, p.size(), p) == 0 &&
                                       path[p.size()] == '/')) {
            return true;
        }
    }
    return false;
}
}  // namespace

Result<ChangeSet> applyScan(storage::Database& db, const std::string& folderId,
                            const std::string& localDeviceId, const fsys::ScanOutput& scan) {
    storage::FileRepository files(db);
    auto rows = files.listFolder(folderId);
    if (!rows) return rows.error();

    auto tx = storage::Transaction::begin(db);
    if (!tx) return tx.error();

    ChangeSet changes;
    std::set<std::string> seen;

    for (const auto& e : scan.entries) {
        seen.insert(e.path);
        if (e.unstable) continue;   // being written right now; keep the old record

        const auto it = rows->find(e.path);
        const bool known = it != rows->end();
        const FileRow* old = known ? &it->second : nullptr;

        FileRow row;
        row.folderId = folderId;
        row.path = e.path;
        row.isDir = e.isDir;
        row.size = e.size;
        row.modifiedAt = e.mtimeMs;
        row.hash = e.hash;
        row.deleted = false;
        row.status = "idle";

        VersionVector vv = old ? VersionVector::fromString(old->versionVector) : VersionVector{};

        if (!old || old->deleted) {
            vv.bump(localDeviceId);
            changes.created.push_back(e.path);
        } else if (old->isDir != e.isDir || (!e.isDir && old->hash != e.hash)) {
            vv.bump(localDeviceId);
            changes.modified.push_back(e.path);
        } else {
            // Same content. Only refresh metadata if it differs (e.g. `touch`).
            if (old->size == e.size && old->modifiedAt == e.mtimeMs) continue;
            row.status = old->status;
            row.lastSyncedAt = old->lastSyncedAt;
        }
        if (old && row.lastSyncedAt == std::nullopt) row.lastSyncedAt = old->lastSyncedAt;
        row.versionVector = vv.toString();
        if (auto r = files.upsert(row); !r) return r.error();
    }

    for (const auto& [path, old] : *rows) {
        if (old.deleted || seen.count(path) || underPrefix(path, scan.unreadablePrefixes)) continue;
        FileRow row = old;
        VersionVector vv = VersionVector::fromString(old.versionVector);
        vv.bump(localDeviceId);
        row.deleted = true;
        row.hash.clear();
        row.size = 0;
        row.versionVector = vv.toString();
        row.status = "idle";
        if (auto r = files.upsert(row); !r) return r.error();
        changes.deleted.push_back(path);
    }

    if (auto r = tx->commit(); !r) return r.error();
    return changes;
}

}  // namespace lansync::sync
