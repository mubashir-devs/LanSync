#include "core/sync/sync_manager.hpp"

#include <algorithm>

#include "core/common/crypto.hpp"
#include "core/common/logging.hpp"
#include "core/common/path_safety.hpp"
#include "core/conflict/conflict.hpp"
#include "core/sync/local_index.hpp"
#include "filesystem/scanner/scanner.hpp"

namespace lansync::sync {

namespace fs = std::filesystem;
namespace proto = lansync::protocol;
using nlohmann::json;
using storage::FileRow;

namespace {

bool isInside(const fs::path& child, const fs::path& parent) {
    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c) {
        if (c == child.end() || *c != *p) return false;
    }
    return true;
}

bool validShareName(const std::string& n) {
    if (n.empty() || n.size() > 64 || n.front() == ' ' || n.back() == ' ') return false;
    for (const char c : n) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

// ================================================================ lifecycle

SyncManager::SyncManager(SyncDeps deps)
    : deps_(std::move(deps)),
      folderRepo_(deps_.db),
      fileRepo_(deps_.db),
      transferRepo_(deps_.db),
      conflictRepo_(deps_.db),
      historyRepo_(deps_.db),
      reconcileTimer_(deps_.io),
      tickTimer_(deps_.io) {}

SyncManager::~SyncManager() { shutdown(); }

void SyncManager::emit(app::EngineEvent::Kind k, std::string title, std::string text, bool err) {
    if (!deps_.emit) return;
    app::EngineEvent e;
    e.kind = k;
    e.title = std::move(title);
    e.text = std::move(text);
    e.isError = err;
    deps_.emit(e);
}

void SyncManager::addHistory(const std::string& deviceId, const std::string& path,
                             const std::string& op, const std::string& result,
                             const std::string& detail) {
    storage::HistoryRow h;
    h.deviceId = deviceId.empty() ? std::nullopt : std::optional<std::string>(deviceId);
    h.path = path.empty() ? std::nullopt : std::optional<std::string>(path);
    h.operation = op;
    h.result = result;
    h.detail = detail;
    h.timestamp = now();
    (void)historyRepo_.add(h);
    if (++historySinceTrim_ >= 500) {
        historySinceTrim_ = 0;
        (void)historyRepo_.trim(5000);
    }
    emit(app::EngineEvent::Kind::HistoryChanged);
}

Result<void> SyncManager::start() {
    running_ = true;
    (void)transferRepo_.failInterrupted(now());
    loadFolders();
    refreshConflictCounts();

    reconcileTick();
    tick();
    return {};
}

void SyncManager::shutdown() {
    if (!running_) return;
    running_ = false;
    cancelWork_ = true;
    *alive_ = false;
    reconcileTimer_.cancel();
    tickTimer_.cancel();
    for (auto& [id, f] : folders_) stopRuntime(*f);
    for (auto& [k, d] : downloads_) {
        if (d->writer) d->writer->abort(/*deletePart=*/false);   // parts survive for resume
        persistDownload(*d, "paused", "application shutdown");
    }
    downloads_.clear();
    uploads_.clear();
}

void SyncManager::loadFolders() {
    auto rows = folderRepo_.list();
    if (!rows) {
        LS_ERROR("cannot load folders: {}", rows.error().message);
        return;
    }
    for (const auto& row : *rows) {
        auto f = std::make_unique<FolderRt>();
        f->row = row;
        auto* raw = f.get();
        folders_[row.id] = std::move(f);
        startRuntime(*raw);
        if (row.enabled) requestScan(*raw);
    }
}

void SyncManager::startRuntime(FolderRt& f) {
    f.debounce = std::make_unique<asio::steady_timer>(deps_.io);
    if (!deps_.enableWatchers || !f.row.enabled) return;
    f.watcher = fsys::makeNativeWatcher();
    if (!f.watcher) return;   // no native watcher on this platform: periodic scans only
    const std::string id = f.row.id;
    std::weak_ptr<bool> alive = alive_;
    auto r = f.watcher->start(f.row.localPath, [this, id, alive](const fsys::WatchEvent&) {
        asio::post(deps_.io, [this, id, alive] {
            if (!alive.lock()) return;
            markDirty(id);
        });
    });
    if (!r) {
        LS_WARN("filesystem watch unavailable for '{}': {} (using periodic scans)",
                f.row.displayName, r.error().message);
        f.watcher.reset();
    }
}

void SyncManager::stopRuntime(FolderRt& f) {
    if (f.watcher) {
        f.watcher->stop();
        f.watcher.reset();
    }
    if (f.debounce) f.debounce->cancel();
}

SyncManager::FolderRt* SyncManager::folderById(const std::string& id) {
    const auto it = folders_.find(id);
    return it == folders_.end() ? nullptr : it->second.get();
}

SyncManager::FolderRt* SyncManager::folderByShare(const std::string& name, const std::string& peerId) {
    for (auto& [id, f] : folders_) {
        if (f->row.displayName == name && f->row.remoteDeviceId == peerId) return f.get();
    }
    return nullptr;
}

// ================================================================ folder management

Result<std::string> SyncManager::addFolder(const fs::path& path, const std::string& name,
                                           const std::string& remoteDeviceId,
                                           const std::string& direction) {
    if (!validShareName(name)) {
        return Error{ErrorCode::InvalidArgument,
                     "folder name must be 1-64 characters: letters, digits, space, '_', '-', '.'"};
    }
    if (direction != "bidirectional" && direction != "send_only" && direction != "receive_only") {
        return Error{ErrorCode::InvalidArgument, "unknown sync direction"};
    }
    if (!deps_.devices.isPaired(remoteDeviceId)) {
        return Error{ErrorCode::SecurityError, "folders can only be shared with paired devices"};
    }
    std::error_code ec;
    const fs::path canon = fs::canonical(path, ec);
    if (ec || !fs::is_directory(canon, ec)) {
        return Error{ErrorCode::InvalidArgument, "folder does not exist: " + path.string()};
    }
    if (canon == canon.root_path()) return Error{ErrorCode::InvalidArgument, "refusing to sync a filesystem root"};

    const fs::path dataDir = fs::weakly_canonical(deps_.dataDir, ec);
    if (isInside(canon, dataDir) || isInside(dataDir, canon)) {
        return Error{ErrorCode::InvalidArgument, "folder overlaps LanSync's own data directory"};
    }
    for (const auto& [id, other] : folders_) {
        const fs::path o = other->row.localPath;
        if (isInside(canon, o) || isInside(o, canon)) {
            return Error{ErrorCode::AlreadyExists, "folder overlaps already-synced folder '" +
                                                       other->row.displayName + "'"};
        }
        if (other->row.displayName == name && other->row.remoteDeviceId == remoteDeviceId) {
            return Error{ErrorCode::AlreadyExists, "a folder named '" + name + "' is already shared with this device"};
        }
    }

    storage::FolderRow row;
    row.id = crypto::randomHex(8);
    row.localPath = canon.string();
    row.displayName = name;
    row.remoteDeviceId = remoteDeviceId;
    row.direction = direction;
    row.enabled = true;
    row.createdAt = now();
    if (auto r = folderRepo_.insert(row); !r) return r.error();

    auto f = std::make_unique<FolderRt>();
    f->row = row;
    auto* raw = f.get();
    folders_[row.id] = std::move(f);
    startRuntime(*raw);
    requestScan(*raw);
    if (auto* s = deps_.devices.trustedSession(remoteDeviceId)) sendFullIndex(*raw, *s);
    addHistory(remoteDeviceId, "", "folder added", "info", name);
    emit(app::EngineEvent::Kind::FoldersChanged);
    return row.id;
}

Result<void> SyncManager::removeFolder(const std::string& folderId) {
    auto* f = folderById(folderId);
    if (!f) return Error{ErrorCode::NotFound, "unknown folder"};

    stopRuntime(*f);
    // Cancel anything running for this folder.
    std::vector<std::string> keys;
    for (auto& [k, d] : downloads_) if (d->folderId == folderId) keys.push_back(k);
    for (const auto& k : keys) failDownload(k, "folder removed", /*keepPart=*/false, false);
    keys.clear();
    for (auto& [k, u] : uploads_) if (u->folderId == folderId) keys.push_back(k);
    for (const auto& k : keys) closeUpload(k, false, "folder removed");

    const fs::path root = f->row.localPath;
    const std::string name = f->row.displayName;
    if (auto r = folderRepo_.remove(folderId); !r) return r;
    folders_.erase(folderId);
    deps_.versions.purgeFolder(folderId);
    std::error_code ec;
    fs::remove_all(root / pathsafety::kInternalDir, ec);   // our temp area only; user files untouched
    addHistory("", "", "folder removed", "info", name);
    refreshConflictCounts();
    emit(app::EngineEvent::Kind::FoldersChanged);
    return {};
}

Result<void> SyncManager::setFolderEnabled(const std::string& folderId, bool enabled) {
    auto* f = folderById(folderId);
    if (!f) return Error{ErrorCode::NotFound, "unknown folder"};
    if (auto r = folderRepo_.setEnabled(folderId, enabled); !r) return r;
    f->row.enabled = enabled;
    if (!enabled) {
        stopRuntime(*f);
        std::vector<std::string> keys;
        for (auto& [k, d] : downloads_) if (d->folderId == folderId) keys.push_back(k);
        for (const auto& k : keys) failDownload(k, "folder disabled", /*keepPart=*/true, false);
        f->queue.clear();
        f->queued.clear();
    } else {
        startRuntime(*f);
        requestScan(*f);
        if (auto* s = deps_.devices.trustedSession(*f->row.remoteDeviceId)) sendFullIndex(*f, *s);
        evaluateAll(*f);
    }
    recompute(*f);
    emit(app::EngineEvent::Kind::FoldersChanged);
    return {};
}

Result<void> SyncManager::setFolderPaused(const std::string& folderId, bool paused) {
    auto* f = folderById(folderId);
    if (!f) return Error{ErrorCode::NotFound, "unknown folder"};
    f->paused = paused;
    if (paused) {
        std::vector<std::string> keys;
        for (auto& [k, d] : downloads_) if (d->folderId == folderId) keys.push_back(k);
        for (const auto& k : keys) failDownload(k, "folder paused", /*keepPart=*/true, false);
        f->queue.clear();
        f->queued.clear();
    } else {
        requestScan(*f);
        evaluateAll(*f);
        pumpDownloads();
    }
    recompute(*f);
    emit(app::EngineEvent::Kind::FoldersChanged);
    return {};
}

Result<void> SyncManager::setFolderDirection(const std::string& folderId, const std::string& direction) {
    auto* f = folderById(folderId);
    if (!f) return Error{ErrorCode::NotFound, "unknown folder"};
    if (direction != "bidirectional" && direction != "send_only" && direction != "receive_only") {
        return Error{ErrorCode::InvalidArgument, "unknown sync direction"};
    }
    if (auto r = folderRepo_.setDirection(folderId, direction); !r) return r;
    f->row.direction = direction;
    if (auto* s = deps_.devices.trustedSession(*f->row.remoteDeviceId)) sendFullIndex(*f, *s);
    evaluateAll(*f);
    emit(app::EngineEvent::Kind::FoldersChanged);
    return {};
}

void SyncManager::syncNow(const std::string& folderId, bool verify) {
    for (auto& [id, f] : folders_) {
        if (!folderId.empty() && id != folderId) continue;
        if (f->row.remoteDeviceId) deps_.devices.connectNow(*f->row.remoteDeviceId);
        f->retry.clear();
        f->suppressUntil.clear();
        requestScan(*f, verify);
        if (auto* s = f->row.remoteDeviceId ? deps_.devices.trustedSession(*f->row.remoteDeviceId) : nullptr) {
            sendFullIndex(*f, *s);
        }
        evaluateAll(*f);
    }
}

// ================================================================ scanning

void SyncManager::markDirty(const std::string& folderId) {
    auto* f = folderById(folderId);
    if (!f || !f->row.enabled || !f->debounce) return;
    f->debounce->expires_after(std::chrono::milliseconds(400));
    std::weak_ptr<bool> alive = alive_;
    f->debounce->async_wait([this, folderId, alive](const boost::system::error_code& ec) {
        if (ec || !alive.lock()) return;
        if (auto* fr = folderById(folderId)) requestScan(*fr);
    });
}

void SyncManager::requestScan(FolderRt& f, bool rehash) {
    if (!running_ || !f.row.enabled || f.paused) return;
    if (f.scanning) {
        f.rescan = true;
        f.rehash = f.rehash || rehash;
        return;
    }
    auto rows = fileRepo_.listFolder(f.row.id);
    if (!rows) {
        f.error = rows.error().message;
        recompute(f);
        return;
    }
    f.scanning = true;
    f.rescan = false;
    f.rehash = false;
    recompute(f);

    auto known = knownFromIndex(*rows);
    const fs::path root = f.row.localPath;
    const std::string id = f.row.id;
    fsys::ScanOptions opts;
    opts.rehashAll = rehash;
    std::weak_ptr<bool> alive = alive_;
    deps_.workers.post([this, alive, id, root, known = std::move(known), opts]() mutable {
        auto out = fsys::scanFolder(root, known, opts, &cancelWork_);
        if (cancelWork_) return;
        asio::post(deps_.io, [this, alive, id, out = std::move(out)]() mutable {
            if (!alive.lock()) return;
            onScanDone(id, std::move(out));
        });
    });
}

void SyncManager::onScanDone(const std::string& folderId, Result<fsys::ScanOutput> out) {
    auto* f = folderById(folderId);
    if (!f) return;
    f->scanning = false;

    if (!out) {
        const bool cancelled = out.error().code == ErrorCode::Cancelled;
        if (!cancelled) {
            if (f->error != out.error().message) {
                addHistory("", "", "scan", "failure", out.error().message);
            }
            f->error = out.error().message;
        }
        recompute(*f);
        emit(app::EngineEvent::Kind::FoldersChanged);
        return;
    }
    f->error.clear();
    for (const auto& w : out->warnings) LS_DEBUG("scan '{}': {}: {}", f->row.displayName, w.path, w.message);

    auto changes = applyScan(deps_.db, folderId, deps_.localDeviceId, *out);
    if (!changes) {
        f->error = changes.error().message;
        LS_ERROR("applying scan failed: {}", f->error);
    } else if (!changes->empty()) {
        LS_INFO("folder '{}': {} new, {} modified, {} deleted", f->row.displayName,
                changes->created.size(), changes->modified.size(), changes->deleted.size());
        addHistory(deps_.localDeviceId, "", "local changes", "info",
                   f->row.displayName + ": " + std::to_string(changes->created.size()) + " new, " +
                       std::to_string(changes->modified.size()) + " modified, " +
                       std::to_string(changes->deleted.size()) + " deleted");
        const auto paths = changes->all();
        // Evaluate BEFORE announcing: a local change may immediately resolve to a
        // conflict/merge that updates our vectors, and the peer should only ever
        // see our settled state (one announcement, not a stale one followed by a fix).
        evaluatePaths(*f, paths);
        announce(*f, paths);
        f->didWork = true;
    }
    if (f->rescan) requestScan(*f, f->rehash);
    recompute(*f);
    emit(app::EngineEvent::Kind::FoldersChanged);
}

void SyncManager::reconcileTick() {
    if (!running_) return;
    for (auto& [id, f] : folders_) requestScan(*f);   // periodic reconciliation: events are only hints
    reconcileTimer_.expires_after(std::chrono::seconds(deps_.settings.reconcileIntervalSec));
    std::weak_ptr<bool> alive = alive_;
    reconcileTimer_.async_wait([this, alive](const boost::system::error_code& ec) {
        if (!ec && alive.lock()) reconcileTick();
    });
}

void SyncManager::tick() {
    if (!running_) return;
    ++tickCount_;
    const auto t = now();

    // Stalled transfers.
    std::vector<std::string> stalled;
    for (auto& [k, d] : downloads_) {
        if (t - d->lastActivityMs > 30'000 && d->phase != Download::Phase::Preparing) stalled.push_back(k);
    }
    for (const auto& k : stalled) failDownload(k, "transfer stalled (no data for 30s)", /*keepPart=*/true);
    std::vector<std::string> deadUploads;
    for (auto& [k, u] : uploads_) if (t - u->lastActivityMs > 60'000) deadUploads.push_back(k);
    for (const auto& k : deadUploads) closeUpload(k, false, "upload stalled");

    if ((!downloads_.empty() || !uploads_.empty()) && t - lastTransferEmit_ >= 250) {
        lastTransferEmit_ = t;
        emit(app::EngineEvent::Kind::TransfersChanged);
    }
    if (tickCount_ % 8 == 0) retryTick();   // every ~2 s

    tickTimer_.expires_after(std::chrono::milliseconds(250));
    std::weak_ptr<bool> alive = alive_;
    tickTimer_.async_wait([this, alive](const boost::system::error_code& ec) {
        if (!ec && alive.lock()) tick();
    });
}

// ================================================================ state

void SyncManager::recompute(FolderRt& f) {
    SyncState target = SyncState::Idle;
    if (f.paused || !f.row.enabled) target = SyncState::Paused;
    else if (f.scanning) target = SyncState::Scanning;
    else if (!f.active.empty()) target = SyncState::Transferring;
    else if (!f.queue.empty()) target = SyncState::Queued;
    else if (f.openConflicts > 0) target = SyncState::Conflict;
    else if (!f.error.empty()) target = SyncState::Failed;
    else if (f.didWork) target = SyncState::Completed;
    f.sm.moveTo(target);
}

// ================================================================ index exchange

EntryMeta SyncManager::toMeta(const FileRow& r) const {
    EntryMeta m;
    m.isDir = r.isDir;
    m.size = r.size;
    m.mtimeMs = r.modifiedAt;
    m.hash = r.hash;
    m.deleted = r.deleted;
    m.vv = VersionVector::fromString(r.versionVector);
    return m;
}

std::optional<EntryMeta> SyncManager::localMeta(const std::string& folderId, const std::string& path,
                                                FileRow* rowOut) {
    auto row = fileRepo_.get(folderId, path);
    if (!row || !*row) return std::nullopt;
    if (rowOut) *rowOut = **row;
    return toMeta(**row);
}

void SyncManager::sendEntries(device::PeerSession& s, const FolderRt& f, const std::vector<FileRow>& rows) {
    json entries = json::array();
    std::size_t bytes = 0;
    auto flush = [&] {
        if (entries.empty()) return;
        s.send(proto::type::kIndex, {{"folder", f.row.displayName}, {"entries", std::move(entries)}});
        entries = json::array();
        bytes = 0;
    };
    for (const auto& r : rows) {
        json e = {{"path", r.path},       {"dir", r.isDir},     {"size", r.size},
                  {"mtime", r.modifiedAt}, {"hash", r.hash},     {"deleted", r.deleted},
                  {"vv", VersionVector::fromString(r.versionVector).toJson()}};
        bytes += e.dump().size();
        entries.push_back(std::move(e));
        if (bytes > 200'000 || entries.size() >= 500) flush();
    }
    flush();
}

void SyncManager::sendFullIndex(FolderRt& f, device::PeerSession& s) {
    if (f.row.direction == "receive_only" || !f.row.enabled) return;
    auto rows = fileRepo_.listFolder(f.row.id);
    if (!rows) return;
    std::vector<FileRow> list;
    list.reserve(rows->size());
    for (auto& [p, r] : *rows) list.push_back(std::move(r));
    sendEntries(s, f, list);
}

void SyncManager::announce(FolderRt& f, const std::vector<std::string>& paths) {
    if (f.row.direction == "receive_only" || !f.row.remoteDeviceId) return;
    auto* s = deps_.devices.trustedSession(*f.row.remoteDeviceId);
    if (!s) return;
    std::vector<FileRow> rows;
    for (const auto& p : paths) {
        auto r = fileRepo_.get(f.row.id, p);
        if (r && *r) rows.push_back(**r);
    }
    sendEntries(*s, f, rows);
}

namespace {

Result<std::pair<std::string, EntryMeta>> parseEntry(const json& j, std::int64_t /*maxSize*/) {
    auto bad = [](const char* why) { return Error{ErrorCode::ProtocolError, why}; };
    if (!j.is_object()) return bad("entry is not an object");
    if (!j.contains("path") || !j["path"].is_string()) return bad("missing path");
    auto path = pathsafety::validateRelativePath(j["path"].get<std::string>());
    if (!path) return path.error();

    EntryMeta m;
    if (!j.contains("dir") || !j["dir"].is_boolean()) return bad("missing dir flag");
    if (!j.contains("deleted") || !j["deleted"].is_boolean()) return bad("missing deleted flag");
    m.isDir = j["dir"].get<bool>();
    m.deleted = j["deleted"].get<bool>();
    if (!j.contains("size") || !j["size"].is_number_integer()) return bad("bad size");
    if (!j.contains("mtime") || !j["mtime"].is_number_integer()) return bad("bad mtime");
    m.size = j["size"].get<std::int64_t>();
    m.mtimeMs = j["mtime"].get<std::int64_t>();
    if (m.size < 0 || m.mtimeMs < 0) return bad("negative size/mtime");
    if (!j.contains("hash") || !j["hash"].is_string()) return bad("bad hash");
    m.hash = j["hash"].get<std::string>();
    const bool needsHash = !m.isDir && !m.deleted;
    if (needsHash && (m.hash.size() != 64 || !crypto::isLowerHex(m.hash))) return bad("invalid content hash");
    if (!needsHash && !m.hash.empty()) return bad("unexpected hash");
    if (m.isDir && m.size != 0) return bad("directory with size");
    if (!j.contains("vv")) return bad("missing version vector");
    auto vv = VersionVector::fromJson(j["vv"]);
    if (!vv) return vv.error();
    if (vv->empty()) return bad("empty version vector");
    m.vv = *vv;
    return std::make_pair(*path, std::move(m));
}

}  // namespace

void SyncManager::handleIndex(device::PeerSession& s, const proto::Message& m) {
    const auto name = m.payload.value("folder", std::string{});
    auto* f = folderByShare(name, s.peerId());
    if (!f) {
        LS_DEBUG("index for unknown/unshared folder '{}' from {}", name, s.peerId().substr(0, 8));
        return;
    }
    if (f->row.direction == "send_only") return;   // we never apply remote changes here
    if (!m.payload.contains("entries") || !m.payload["entries"].is_array() ||
        m.payload["entries"].size() > 1000) {
        LS_WARN("malformed index from {}", s.peerId().substr(0, 8));
        return;
    }

    std::vector<std::string> paths;
    int rejected = 0;
    for (const auto& je : m.payload["entries"]) {
        auto parsed = parseEntry(je, maxFileBytes());
        if (!parsed) {
            ++rejected;
            LS_WARN("rejecting index entry from {}: {}", s.peerId().substr(0, 8), parsed.error().message);
            continue;
        }
        if (f->remote.size() > 5'000'000 && !f->remote.count(parsed->first)) continue;   // resource cap
        paths.push_back(parsed->first);
        f->remote[parsed->first] = std::move(parsed->second);
    }
    if (rejected > 0) addHistory(s.peerId(), "", "index", "failure", std::to_string(rejected) + " invalid entries ignored");
    if (!f->row.enabled || f->paused) return;   // remembered; evaluated on resume
    f->sm.moveTo(SyncState::Comparing);
    evaluatePaths(*f, paths);
    recompute(*f);
    emit(app::EngineEvent::Kind::FoldersChanged);
}

// ================================================================ evaluation

void SyncManager::evaluateAll(FolderRt& f) {
    if (!f.row.enabled || f.paused || f.row.direction == "send_only") return;
    std::vector<std::string> paths;
    paths.reserve(f.remote.size());
    for (const auto& [p, e] : f.remote) paths.push_back(p);
    evaluatePaths(f, paths);
}

void SyncManager::evaluatePaths(FolderRt& f, const std::vector<std::string>& paths) {
    if (!f.row.enabled || f.paused || f.row.direction == "send_only") return;

    struct Item { std::string path; Action action; std::size_t depth; };
    std::vector<Item> items;
    for (const auto& p : paths) {
        const auto it = f.remote.find(p);
        if (it == f.remote.end()) continue;
        const auto local = localMeta(f.row.id, p);
        const auto a = decide(local, it->second);
        const auto depth = static_cast<std::size_t>(std::count(p.begin(), p.end(), '/'));
        items.push_back({p, a, depth});
    }
    // Parents before children when creating; children before parents when deleting.
    auto rank = [](Action a) {
        switch (a) {
            case Action::MakeDir: return 0;
            case Action::ApplyDelete: return 2;
            default: return 1;
        }
    };
    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        if (rank(a.action) != rank(b.action)) return rank(a.action) < rank(b.action);
        return a.action == Action::ApplyDelete ? a.depth > b.depth : a.depth < b.depth;
    });

    for (const auto& item : items) {
        const auto& remote = f.remote[item.path];
        switch (item.action) {
            case Action::None: {
                const auto local = localMeta(f.row.id, item.path);
                if (local && !local->deleted && local->vv == remote.vv) markSynced(f, item.path);
                break;
            }
            case Action::Fetch: enqueue(f, item.path); break;
            case Action::MakeDir: applyMakeDir(f, item.path, remote); break;
            case Action::ApplyDelete: applyDelete(f, item.path, remote); break;
            case Action::MergeVersion: applyMerge(f, item.path, remote); break;
            case Action::KeepLocal: applyKeepLocal(f, item.path, remote); break;
            case Action::Conflict: {
                FileRow row;
                const auto local = localMeta(f.row.id, item.path, &row);
                const bool remoteWins = conflict::remoteWins(local->mtimeMs, deps_.localDeviceId,
                                                             remote.mtimeMs, *f.row.remoteDeviceId);
                if (remoteWins) enqueue(f, item.path);
                else recordConflictAsWinner(f, item.path, remote);
                break;
            }
        }
    }
    pumpDownloads();
}

void SyncManager::enqueue(FolderRt& f, const std::string& path) {
    if (f.queued.count(path) || f.active.count(path)) return;
    if (f.userPaused.count(path)) return;
    const auto t = now();
    if (const auto s = f.suppressUntil.find(path); s != f.suppressUntil.end()) {
        if (t < s->second) return;
        f.suppressUntil.erase(s);
    }
    if (const auto r = f.retry.find(path); r != f.retry.end() && t < r->second.nextAtMs) return;
    f.queue.push_back(path);
    f.queued.insert(path);
    recompute(f);
}

void SyncManager::retryTick() {
    const auto t = now();
    for (auto& [id, f] : folders_) {
        if (!f->row.enabled || f->paused) continue;
        std::vector<std::string> due;
        for (auto& [path, r] : f->retry) {
            if (t >= r.nextAtMs) due.push_back(path);
        }
        if (due.empty()) continue;
        evaluatePaths(*f, due);
        for (const auto& p : due) {
            if (!f->queued.count(p) && !f->active.count(p)) f->retry.erase(p);   // no longer needed
        }
    }
}

void SyncManager::markSynced(FolderRt& f, const std::string& path) {
    FileRow row;
    if (!localMeta(f.row.id, path, &row) || row.deleted) return;
    if (row.lastSyncedAt && row.status == "completed") return;
    row.lastSyncedAt = now();
    row.status = "completed";
    if (fileRepo_.upsert(row)) {
        f.lastSyncMs = now();
        f.didWork = true;
    }
}

}  // namespace lansync::sync
