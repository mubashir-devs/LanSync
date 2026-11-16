#include <algorithm>

#include "core/common/crypto.hpp"
#include "core/common/logging.hpp"
#include "core/common/path_safety.hpp"
#include "core/conflict/conflict.hpp"
#include "core/sync/sync_manager.hpp"
#include "filesystem/scanner/scanner.hpp"

namespace lansync::sync {

namespace fs = std::filesystem;
using storage::FileRow;

// ================================================================ simple decisions

void SyncManager::applyMerge(FolderRt& f, const std::string& path, const EntryMeta& remote) {
    FileRow row;
    if (!localMeta(f.row.id, path, &row)) return;
    auto vv = VersionVector::fromString(row.versionVector);
    vv.merge(remote.vv);
    row.versionVector = vv.toString();
    if (!row.deleted) {
        row.status = "completed";
        row.lastSyncedAt = now();
        f.lastSyncMs = now();
    }
    (void)fileRepo_.upsert(row);
}

void SyncManager::applyKeepLocal(FolderRt& f, const std::string& path, const EntryMeta& remote) {
    FileRow row;
    if (!localMeta(f.row.id, path, &row)) return;
    auto vv = VersionVector::fromString(row.versionVector);
    vv.merge(remote.vv);
    vv.bump(deps_.localDeviceId);   // now strictly newer than the peer's deletion
    row.versionVector = vv.toString();
    if (fileRepo_.upsert(row)) announce(f, {path});
    addHistory(*f.row.remoteDeviceId, path, "edit vs delete", "info",
               "kept local edit; peer deleted it concurrently");
}

void SyncManager::recordConflictAsWinner(FolderRt& f, const std::string& path, const EntryMeta& remote) {
    FileRow row;
    if (!localMeta(f.row.id, path, &row)) return;

    // Idempotent: evaluation can run several times before the peer's copy arrives.
    if (auto open = conflictRepo_.list(false)) {
        for (const auto& c : *open) {
            if (c.fileId == row.fileId && c.localHash == row.hash && c.remoteHash == remote.hash) return;
        }
    }

    storage::ConflictRow c;
    c.fileId = row.fileId;
    c.localHash = row.hash;
    c.remoteHash = remote.hash;
    c.localModifiedAt = row.modifiedAt;
    c.remoteModifiedAt = remote.mtimeMs;
    c.copyPath = conflict::conflictCopyPath(path, *f.row.remoteDeviceId, remote.mtimeMs);
    c.detectedAt = now();
    if (auto r = conflictRepo_.insert(c); !r) {
        LS_ERROR("cannot record conflict: {}", r.error().message);
        return;
    }
    // Our file keeps its name and its version vector UNCHANGED. The peer (the loser)
    // sees the same concurrency, preserves its own content as a conflict copy, and
    // adopts ours; its merged vector then dominates ours and both sides converge.
    addHistory(*f.row.remoteDeviceId, path, "conflict", "info",
               "edited on both devices; the peer's version will be kept as " + c.copyPath);
    refreshConflictCounts();
    emit(app::EngineEvent::Kind::ConflictsChanged);
    emit(app::EngineEvent::Kind::Notification, "Sync conflict", path + " was edited on both devices. Both versions are kept.");
}

void SyncManager::applyMakeDir(FolderRt& f, const std::string& path, const EntryMeta& remote) {
    auto dest = pathsafety::resolveInside(rootOf(f), path);
    if (!dest) {
        addHistory(*f.row.remoteDeviceId, path, "mkdir", "failure", dest.error().message);
        return;
    }
    FileRow row;
    const auto local = localMeta(f.row.id, path, &row);
    std::error_code ec;
    const auto st = fs::symlink_status(*dest, ec);
    if (fs::is_regular_file(st)) {
        if (!local || local->deleted || local->isDir) {
            addHistory(*f.row.remoteDeviceId, path, "mkdir", "failure", "an untracked file is in the way");
            return;
        }
        if (auto v = deps_.versions.archive(row, *dest, true); !v) {   // replace file by directory, keep the file
            addHistory(*f.row.remoteDeviceId, path, "mkdir", "failure", v.error().message);
            return;
        }
    } else if (fs::is_symlink(st)) {
        return;
    }
    fs::create_directories(*dest, ec);
    if (ec) {
        addHistory(*f.row.remoteDeviceId, path, "mkdir", "failure", ec.message());
        return;
    }
    FileRow nr;
    nr.folderId = f.row.id;
    nr.path = path;
    nr.isDir = true;
    nr.modifiedAt = fsys::fileMtimeMs(*dest).ok() ? *fsys::fileMtimeMs(*dest) : 0;
    auto vv = remote.vv;
    if (local) vv.merge(local->vv);
    nr.versionVector = vv.toString();
    nr.status = "completed";
    nr.lastSyncedAt = now();
    (void)fileRepo_.upsert(nr);
    f.lastSyncMs = now();
    f.didWork = true;
}

void SyncManager::applyDelete(FolderRt& f, const std::string& path, const EntryMeta& remote) {
    FileRow row;
    const auto local = localMeta(f.row.id, path, &row);
    if (!local || local->deleted) return;
    auto dest = pathsafety::resolveInside(rootOf(f), path);
    if (!dest) {
        addHistory(*f.row.remoteDeviceId, path, "delete", "failure", dest.error().message);
        return;
    }
    std::error_code ec;
    const auto st = fs::symlink_status(*dest, ec);
    if (fs::is_regular_file(st)) {
        const auto mt = fsys::fileMtimeMs(*dest);
        const auto size = fs::file_size(*dest, ec);
        if (!mt || ec || *mt != row.modifiedAt || static_cast<std::int64_t>(size) != row.size) {
            // Edited since our last scan: never delete unsynced edits. The next scan
            // records the edit, which then wins as "edit vs delete".
            requestScan(f);
            return;
        }
        if (auto v = deps_.versions.archive(row, *dest, /*consume=*/true); !v) {
            addHistory(*f.row.remoteDeviceId, path, "delete", "failure", v.error().message);
            return;
        }
    } else if (fs::is_directory(st)) {
        fs::remove(*dest, ec);   // non-recursive: only if already empty
        if (ec) return;          // children still present; evaluated again later
    }
    auto vv = local->vv;
    vv.merge(remote.vv);
    row.deleted = true;
    row.hash.clear();
    row.size = 0;
    row.versionVector = vv.toString();
    row.status = "completed";
    row.lastSyncedAt = now();
    (void)fileRepo_.upsert(row);
    f.lastSyncMs = now();
    f.didWork = true;
    addHistory(*f.row.remoteDeviceId, path, "delete", "success", "removed (previous version kept)");
    emit(app::EngineEvent::Kind::FoldersChanged);
}

// ================================================================ placing a downloaded file

Result<void> SyncManager::applyDownloaded(FolderRt& f, Download& d) {
    const fs::path root = rootOf(f);
    auto destR = pathsafety::resolveInside(root, d.path);
    if (!destR) return destR.error();
    const fs::path dest = *destR;

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create parent directory: " + ec.message()};

    FileRow local;
    const auto lm = localMeta(f.row.id, d.path, &local);
    // The peer may have announced a newer state for the SAME content while we
    // were downloading; adopt its (larger) version vector so we end up equal to it.
    if (const auto latest = f.remote.find(d.path); latest != f.remote.end() &&
                                                    !latest->second.deleted &&
                                                    latest->second.hash == d.remote.hash) {
        d.remote.vv.merge(latest->second.vv);
    }
    auto vv = d.remote.vv;

    // The decision that started this transfer may be stale (the local file could
    // have been edited, scanned and even declared the conflict winner meanwhile).
    // Re-decide with current state; only apply if we would still fetch it.
    {
        const auto current = decide(lm, d.remote);
        const bool stillWanted = current == Action::Fetch ||
                                 (current == Action::Conflict && d.conflict);
        if (!stillWanted) {
            return Error{ErrorCode::Busy, "state changed during transfer; re-evaluating"};
        }
    }

    bool preserved = false;
    std::string copyRel, localHash;
    std::int64_t localMtime = 0;
    fs::path copyDest;
    std::optional<storage::VersionRow> archived;

    const auto st = fs::symlink_status(dest, ec);
    if (fs::is_symlink(st)) return Error{ErrorCode::SecurityError, "destination is a symlink"};
    if (fs::is_directory(st)) return Error{ErrorCode::Conflict, "a directory exists where this file belongs"};

    if (fs::is_regular_file(st)) {
        const bool tracked = lm && !lm->deleted && !lm->isDir;
        const auto diskMtime = fsys::fileMtimeMs(dest);
        const auto diskSize = fs::file_size(dest, ec);
        const bool unchanged = tracked && diskMtime && !ec && *diskMtime == local.modifiedAt &&
                               static_cast<std::int64_t>(diskSize) == local.size;

        if (!d.conflict && !unchanged) {
            // The local file has an edit our index has not seen yet. Do not guess:
            // rescan first, then the normal decision (incl. deterministic conflict
            // handling) runs with complete information. The verified part file is kept.
            return Error{ErrorCode::Busy, "local file changed during transfer; rechecking"};
        }
        if (d.conflict || !unchanged) {
            // We lost a concurrent edit (or the file changed again): keep the local content as a copy.
            localMtime = diskMtime ? *diskMtime : 0;
            if (unchanged) {
                localHash = local.hash;
            } else {
                auto h = crypto::hashFile(dest);
                if (!h) return h.error();
                localHash = *h;
            }
            copyRel = conflict::conflictCopyPath(d.path, deps_.localDeviceId, localMtime);
            for (int n = 1;; ++n) {
                auto cd = pathsafety::resolveInside(root, copyRel);
                if (!cd) return cd.error();
                copyDest = *cd;
                if (!fs::exists(copyDest, ec)) break;
                copyRel = conflict::conflictCopyPath(d.path, deps_.localDeviceId, localMtime) + "-" + std::to_string(n);
                if (n > 99) return Error{ErrorCode::AlreadyExists, "too many conflict copies"};
            }
            fs::rename(dest, copyDest, ec);
            if (ec) return Error{ErrorCode::IoError, "cannot preserve local version: " + ec.message()};
            preserved = true;
            if (lm) vv.merge(lm->vv);
        } else {
            auto v = deps_.versions.archive(local, dest, /*consume=*/true);
            if (!v) return v.error();
            archived = *v;
        }
    }

    if (auto r = fsys::setFileMtimeMs(d.part, d.remote.mtimeMs); !r) {
        LS_DEBUG("{}", r.error().message);
    }
    fs::rename(d.part, dest, ec);
    if (ec) {
        const std::string msg = "cannot move received file into place: " + ec.message();
        std::error_code rec;   // roll back so the user's data stays where it was
        if (preserved) fs::rename(copyDest, dest, rec);
        else if (archived) fs::rename(deps_.versions.locate(*archived), dest, rec);
        return Error{ErrorCode::IoError, msg};
    }

    FileRow nr;
    nr.folderId = f.row.id;
    nr.path = d.path;
    nr.isDir = false;
    nr.size = d.remote.size;
    const auto placedMtime = fsys::fileMtimeMs(dest);
    nr.modifiedAt = placedMtime ? *placedMtime : d.remote.mtimeMs;
    nr.hash = d.remote.hash;
    nr.versionVector = vv.toString();
    nr.status = "completed";
    nr.lastSyncedAt = now();
    if (auto r = fileRepo_.upsert(nr); !r) return r.error();
    f.lastSyncMs = now();
    f.didWork = true;

    if (preserved) {
        storage::ConflictRow c;
        c.fileId = nr.fileId;
        c.localHash = localHash;
        c.remoteHash = d.remote.hash;
        c.localModifiedAt = localMtime;
        c.remoteModifiedAt = d.remote.mtimeMs;
        c.copyPath = copyRel;
        c.detectedAt = now();
        (void)conflictRepo_.insert(c);
        addHistory(d.peerId, d.path, "conflict", "info", "your version was kept as " + copyRel);
        refreshConflictCounts();
        emit(app::EngineEvent::Kind::ConflictsChanged);
        emit(app::EngineEvent::Kind::Notification, "Sync conflict",
             d.path + " was edited on both devices. Both versions are kept.");
        requestScan(f);   // pick up the new conflict copy so it syncs too
    }
    return {};
}

// ================================================================ conflicts

void SyncManager::refreshConflictCounts() {
    for (auto& [id, f] : folders_) f->openConflicts = 0;
    auto open = conflictRepo_.list(false);
    if (!open) return;
    for (const auto& c : *open) {
        auto file = fileRepo_.getById(c.fileId);
        if (!file || !*file) continue;
        if (auto* f = folderById((*file)->folderId)) ++f->openConflicts;
    }
    for (auto& [id, f] : folders_) recompute(*f);
}

Result<void> SyncManager::resolveConflict(std::int64_t conflictId, const std::string& resolution) {
    if (resolution != "keep_local" && resolution != "keep_remote" && resolution != "keep_both") {
        return Error{ErrorCode::InvalidArgument, "unknown resolution"};
    }
    auto c = conflictRepo_.get(conflictId);
    if (!c) return c.error();
    if (!*c || (*c)->resolution) return Error{ErrorCode::NotFound, "conflict not found or already resolved"};
    auto file = fileRepo_.getById((*c)->fileId);
    if (!file) return file.error();
    if (!*file) return Error{ErrorCode::NotFound, "file record is gone"};
    auto* f = folderById((*file)->folderId);
    if (!f) return Error{ErrorCode::NotFound, "folder is gone"};

    if (resolution != "keep_both") {
        const std::string& wanted = resolution == "keep_local" ? (*c)->localHash : (*c)->remoteHash;
        FileRow current = **file;
        auto dest = pathsafety::resolveInside(rootOf(*f), current.path);
        if (!dest) return dest.error();

        FileRow copyRow;
        bool haveCopy = false;
        fs::path copyDest;
        if (!(*c)->copyPath.empty()) {
            auto cr = fileRepo_.get(f->row.id, (*c)->copyPath);
            if (cr && *cr && !(*cr)->deleted) {
                copyRow = **cr;
                haveCopy = true;
                auto cd = pathsafety::resolveInside(rootOf(*f), (*c)->copyPath);
                if (!cd) return cd.error();
                copyDest = *cd;
            }
        }

        std::error_code ec;
        if (!current.deleted && current.hash == wanted) {
            // Desired content already holds the original name; drop the copy.
            if (haveCopy) {
                if (auto v = deps_.versions.archive(copyRow, copyDest, true); !v) return v.error();
            }
        } else if (haveCopy && copyRow.hash == wanted) {
            if (!current.deleted && fs::exists(*dest, ec)) {
                if (auto v = deps_.versions.archive(current, *dest, true); !v) return v.error();
            }
            fs::rename(copyDest, *dest, ec);
            if (ec) return Error{ErrorCode::IoError, "cannot put the chosen version in place: " + ec.message()};
        } else {
            // Fall back to version history.
            storage::VersionRepository vr(deps_.db);
            auto versions = vr.listForFile(current.fileId);
            if (!versions) return versions.error();
            const auto it = std::find_if(versions->begin(), versions->end(),
                                         [&](const storage::VersionRow& v) { return v.hash == wanted; });
            if (it == versions->end()) {
                return Error{ErrorCode::NotFound,
                             "that version is not available yet (the conflict copy has not arrived)"};
            }
            fs::path tmp = rootOf(*f) / pathsafety::kInternalDir / "tmp" / ("restore-" + crypto::randomHex(4));
            fs::create_directories(tmp.parent_path(), ec);
            if (auto r = deps_.versions.copyOut(*it, tmp); !r) return r;
            if (!current.deleted && fs::exists(*dest, ec)) {
                if (auto v = deps_.versions.archive(current, *dest, true); !v) return v.error();
            }
            fs::create_directories(dest->parent_path(), ec);
            fs::rename(tmp, *dest, ec);
            if (ec) return Error{ErrorCode::IoError, "cannot restore chosen version: " + ec.message()};
        }
    }

    if (auto r = conflictRepo_.resolve(conflictId, resolution, now()); !r) return r;
    addHistory("", (*file)->path, "conflict resolved", "success", resolution);
    refreshConflictCounts();
    requestScan(*f);   // the resulting changes sync like any edit
    emit(app::EngineEvent::Kind::ConflictsChanged);
    return {};
}

// ================================================================ versions

Result<std::vector<app::VersionInfo>> SyncManager::listVersions(const std::string& folderId,
                                                                const std::string& path) {
    auto row = fileRepo_.get(folderId, path);
    if (!row) return row.error();
    std::vector<app::VersionInfo> out;
    if (!*row) return out;
    auto versions = storage::VersionRepository(deps_.db).listForFile((*row)->fileId);
    if (!versions) return versions.error();
    for (const auto& v : *versions) {
        out.push_back({v.versionId, folderId, path, v.hash, v.size, v.modifiedAt, v.createdAt});
    }
    return out;
}

Result<void> SyncManager::restoreVersion(std::int64_t versionId) {
    storage::VersionRepository vr(deps_.db);
    auto v = vr.get(versionId);
    if (!v) return v.error();
    if (!*v) return Error{ErrorCode::NotFound, "version not found"};
    auto file = fileRepo_.getById((*v)->fileId);
    if (!file) return file.error();
    if (!*file) return Error{ErrorCode::NotFound, "file record is gone"};
    auto* f = folderById((*file)->folderId);
    if (!f) return Error{ErrorCode::NotFound, "folder is gone"};
    auto dest = pathsafety::resolveInside(rootOf(*f), (*file)->path);
    if (!dest) return dest.error();

    std::error_code ec;
    const fs::path tmpDir = rootOf(*f) / pathsafety::kInternalDir / "tmp";
    fs::create_directories(tmpDir, ec);
    const fs::path tmp = tmpDir / ("restore-" + crypto::randomHex(4));
    if (auto r = deps_.versions.copyOut(**v, tmp); !r) return r;   // verifies integrity

    if (fs::is_regular_file(*dest, ec)) {
        // Preserve whatever is there now (describe it from disk, not from a possibly stale record).
        auto h = crypto::hashFile(*dest);
        if (!h) { fs::remove(tmp, ec); return h.error(); }
        FileRow cur = **file;
        cur.hash = *h;
        cur.size = static_cast<std::int64_t>(fs::file_size(*dest, ec));
        cur.modifiedAt = fsys::fileMtimeMs(*dest).ok() ? *fsys::fileMtimeMs(*dest) : 0;
        if (auto a = deps_.versions.archive(cur, *dest, true); !a) { fs::remove(tmp, ec); return a.error(); }
    }
    fs::create_directories(dest->parent_path(), ec);
    fs::rename(tmp, *dest, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot place restored version: " + ec.message()};

    addHistory("", (*file)->path, "restore version", "success", f->row.displayName);
    requestScan(*f);   // detects the change, bumps the version vector, syncs it to the peer
    emit(app::EngineEvent::Kind::FoldersChanged);
    return {};
}

// ================================================================ queries

std::vector<app::FolderInfo> SyncManager::folderInfos() {
    std::vector<app::FolderInfo> out;
    for (auto& [id, f] : folders_) {
        app::FolderInfo i;
        i.id = id;
        i.name = f->row.displayName;
        i.path = f->row.localPath;
        i.remoteDeviceId = f->row.remoteDeviceId.value_or("");
        i.remoteDeviceName = f->row.remoteDeviceId ? deps_.devices.deviceName(*f->row.remoteDeviceId) : "";
        i.direction = f->row.direction;
        i.enabled = f->row.enabled;
        i.paused = f->paused;
        i.state = !f->row.enabled ? "Disabled" : std::string(toString(f->sm.state()));
        auto n = fileRepo_.countLive(id);
        i.fileCount = n ? *n : 0;
        i.pendingCount = static_cast<std::int64_t>(f->queue.size() + f->active.size());
        i.conflictCount = f->openConflicts;
        auto last = fileRepo_.lastSyncedAt(id);
        i.lastSyncMs = std::max<TimestampMs>(f->lastSyncMs, last ? *last : 0);
        i.error = f->error;
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<app::ConflictInfo> SyncManager::conflictInfos(bool includeResolved) {
    std::vector<app::ConflictInfo> out;
    auto rows = conflictRepo_.list(includeResolved);
    if (!rows) return out;
    for (const auto& c : *rows) {
        auto file = fileRepo_.getById(c.fileId);
        if (!file || !*file) continue;
        app::ConflictInfo i;
        i.id = c.id;
        i.folderId = (*file)->folderId;
        if (auto* f = folderById(i.folderId)) i.folderName = f->row.displayName;
        i.path = (*file)->path;
        i.copyPath = c.copyPath;
        i.localHash = c.localHash;
        i.remoteHash = c.remoteHash;
        i.localModifiedMs = c.localModifiedAt;
        i.remoteModifiedMs = c.remoteModifiedAt;
        i.detectedMs = c.detectedAt;
        i.resolution = c.resolution.value_or("");
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<app::HistoryInfo> SyncManager::historyInfos(int limit) {
    std::vector<app::HistoryInfo> out;
    auto rows = historyRepo_.recent(limit);
    if (!rows) return out;
    for (const auto& h : *rows) {
        app::HistoryInfo i;
        i.id = h.id;
        i.timestampMs = h.timestamp;
        i.deviceName = h.deviceId ? deps_.devices.deviceName(*h.deviceId) : "";
        i.path = h.path.value_or("");
        i.operation = h.operation;
        i.result = h.result;
        i.detail = h.detail;
        out.push_back(std::move(i));
    }
    return out;
}

app::Stats SyncManager::stats() {
    app::Stats s;
    for (const auto& [id, f] : folders_) {
        if (f->row.enabled && !f->paused) ++s.activeFolders;
        s.pendingTransfers += static_cast<int>(f->queue.size());
        s.openConflicts += static_cast<int>(f->openConflicts);
    }
    s.activeTransfers = static_cast<int>(downloads_.size() + uploads_.size());
    auto n = fileRepo_.countSyncedLive();
    s.filesSynced = n ? *n : 0;
    return s;
}

}  // namespace lansync::sync
