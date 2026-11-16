#include <algorithm>
#include <fstream>

#include "core/common/crypto.hpp"
#include "core/common/logging.hpp"
#include "core/common/path_safety.hpp"
#include "core/conflict/conflict.hpp"
#include "core/sync/sync_manager.hpp"
#include "filesystem/scanner/scanner.hpp"

namespace lansync::sync {

namespace fs = std::filesystem;
namespace proto = lansync::protocol;
using nlohmann::json;
using storage::FileRow;

namespace {
constexpr TimestampMs kMaxBackoffMs = 300'000;
constexpr TimestampMs kUserCancelSuppressMs = 10 * 60 * 1000;
constexpr std::uint64_t kDiskReserveBytes = 16ull * 1024 * 1024;
}  // namespace

// ================================================================ peer events

void SyncManager::onPeerReady(device::PeerSession& s) {
    for (auto& [id, f] : folders_) {
        if (f->row.remoteDeviceId != s.peerId() || !f->row.enabled) continue;
        f->retry.clear();
        sendFullIndex(*f, s);
        requestScan(*f);      // catches changes made while disconnected
        evaluateAll(*f);      // we may already know the peer's index
    }
    emit(app::EngineEvent::Kind::FoldersChanged);
}

void SyncManager::onPeerClosed(const std::string& peerId) {
    if (!running_) return;
    std::vector<std::string> keys;
    for (auto& [k, d] : downloads_) if (d->peerId == peerId) keys.push_back(k);
    for (const auto& k : keys) failDownload(k, "connection lost", /*keepPart=*/true, /*countAsFailure=*/false);
    keys.clear();
    for (auto& [k, u] : uploads_) if (u->peerId == peerId) keys.push_back(k);
    for (const auto& k : keys) closeUpload(k, false, "connection lost");
    emit(app::EngineEvent::Kind::FoldersChanged);
}

void SyncManager::onPeerMessage(device::PeerSession& s, const proto::Message& m) {
    const auto& t = m.type;
    const auto id = [&]() -> std::optional<std::uint32_t> {
        const auto it = m.payload.find("transfer_id");
        if (it == m.payload.end() || !it->is_number_unsigned()) return std::nullopt;
        const auto v = it->get<std::uint64_t>();
        if (v > 0xFFFFFFFFull) return std::nullopt;
        return static_cast<std::uint32_t>(v);
    }();

    if (t == proto::type::kIndex) {
        handleIndex(s, m);
    } else if (t == proto::type::kFileRequest) {
        handleFileRequest(s, m);
    } else if (t == proto::type::kFileBegin && id) {
        auto* d = downloadByStream(s.peerId(), *id);
        if (!d || d->phase != Download::Phase::Requested) return;
        const auto size = m.payload.value("size", std::int64_t{-1});
        const auto hash = m.payload.value("hash", std::string{});
        const auto offset = m.payload.value("offset", std::int64_t{-1});
        if (size != d->remote.size || hash != d->remote.hash ||
            offset != static_cast<std::int64_t>(d->writer->received())) {
            failDownload(d->key, "remote file changed during request", /*keepPart=*/true);
            return;
        }
        d->phase = Download::Phase::Receiving;
        d->bytes = offset;
        d->lastActivityMs = now();
        d->sampleBytes = offset;
        d->sampleMs = now();
        persistDownload(*d, "transferring");
    } else if (t == proto::type::kFileEnd && id) {
        auto* d = downloadByStream(s.peerId(), *id);
        if (!d || d->phase != Download::Phase::Receiving) return;
        if (!d->writer->complete()) {
            failDownload(d->key, "peer ended the transfer early", /*keepPart=*/true);
            return;
        }
        finishDownload(d->key);
    } else if (t == proto::type::kFileError && id) {
        auto* d = downloadByStream(s.peerId(), *id);
        if (!d) return;
        failDownload(d->key, m.payload.value("code", std::string{"error"}) + ": " +
                                 m.payload.value("message", std::string{}),
                     /*keepPart=*/true);
    } else if (t == proto::type::kFileCancel && id) {
        for (auto& [k, u] : uploads_) {
            if (u->peerId == s.peerId() && u->streamId == *id) {
                closeUpload(k, false, "cancelled by peer");
                break;
            }
        }
    } else if (t == proto::type::kFileDone && id) {
        for (auto& [k, u] : uploads_) {
            if (u->peerId == s.peerId() && u->streamId == *id) {
                const bool ok = m.payload.value("ok", false);
                if (ok && m.payload.value("applied", true)) {   // the receiver verified and stored it: it is synced from our side too
                    if (auto* f = folderById(u->folderId)) markSynced(*f, u->path);
                }
                closeUpload(k, ok, ok ? "" : m.payload.value("reason", std::string{"rejected by receiver"}));
                break;
            }
        }
    }
}

void SyncManager::onPeerChunk(device::PeerSession& s, std::uint32_t streamId, std::string_view data) {
    auto* d = downloadByStream(s.peerId(), streamId);
    if (!d || d->phase != Download::Phase::Receiving) return;   // stale chunk after cancel
    if (auto r = d->writer->write(data); !r) {
        const bool protocolViolation = r.error().code == ErrorCode::ProtocolError;
        failDownload(d->key, r.error().message, /*keepPart=*/!protocolViolation);
        return;
    }
    d->bytes = static_cast<std::int64_t>(d->writer->received());
    d->lastActivityMs = now();
    updateSpeed(d->bytes, now(), d->sampleBytes, d->sampleMs, d->speedBps);
}

void SyncManager::updateSpeed(std::int64_t bytes, TimestampMs t, std::int64_t& sampleBytes,
                              TimestampMs& sampleMs, double& speed) {
    const auto dt = t - sampleMs;
    if (dt < 500) return;
    const double inst = static_cast<double>(bytes - sampleBytes) * 1000.0 / static_cast<double>(dt);
    speed = speed == 0 ? inst : speed * 0.6 + inst * 0.4;
    sampleBytes = bytes;
    sampleMs = t;
}

SyncManager::Download* SyncManager::downloadByStream(const std::string& peerId, std::uint32_t streamId) {
    for (auto& [k, d] : downloads_) {
        if (d->peerId == peerId && d->streamId == streamId) return d.get();
    }
    return nullptr;
}

// ================================================================ downloads

fs::path SyncManager::partPathFor(const FolderRt& f, const std::string& path, const std::string& hash) const {
    // Name depends on (path, content hash): a part file can only ever be resumed for
    // exactly the same target content.
    return rootOf(f) / pathsafety::kInternalDir / "tmp" /
           (crypto::sha256Hex(path + ":" + hash).substr(0, 24) + ".part");
}

void SyncManager::pumpDownloads() {
    if (!running_) return;
    const auto maxN = static_cast<std::size_t>(deps_.settings.maxConcurrentTransfers);
    bool progress = true;
    while (downloads_.size() < maxN && progress) {
        progress = false;
        for (auto& [id, f] : folders_) {
            if (downloads_.size() >= maxN) break;
            if (!f->row.enabled || f->paused || f->queue.empty()) continue;
            const std::string path = f->queue.front();
            f->queue.pop_front();
            f->queued.erase(path);
            startDownload(*f, path);
            progress = true;
        }
    }
}

void SyncManager::startDownload(FolderRt& f, const std::string& path) {
    const auto it = f.remote.find(path);
    if (it == f.remote.end()) return;
    const EntryMeta remote = it->second;   // copy: the map may change while we run

    FileRow row;
    const auto local = localMeta(f.row.id, path, &row);
    const auto action = decide(local, remote);
    bool conflictLoser = false;
    if (action == Action::Conflict) {
        if (!conflict::remoteWins(local->mtimeMs, deps_.localDeviceId, remote.mtimeMs, *f.row.remoteDeviceId)) {
            recordConflictAsWinner(f, path, remote);
            return;
        }
        conflictLoser = true;
    } else if (action != Action::Fetch) {
        std::weak_ptr<bool> alive = alive_;
        asio::post(deps_.io, [this, alive, id = f.row.id, path] {   // state moved on: re-decide
            if (!alive.lock()) return;
            if (auto* fr = folderById(id)) evaluatePaths(*fr, {path});
        });
        return;
    }

    auto* session = deps_.devices.trustedSession(*f.row.remoteDeviceId);
    if (!session) return;   // re-evaluated when the peer reconnects

    auto scheduleRetry = [&](const std::string& why, TimestampMs delayMs, bool permanentish) {
        auto& r = f.retry[path];
        ++r.fails;
        r.nextAtMs = now() + delayMs;
        addHistory(*f.row.remoteDeviceId, path, "download", "failure", why);
        (void)permanentish;
        recompute(f);
    };
    if (remote.size > maxFileBytes()) {
        scheduleRetry("file exceeds the configured size limit", kMaxBackoffMs, true);
        return;
    }
    std::error_code ec;
    const auto space = fs::space(rootOf(f), ec);
    if (!ec && space.available < static_cast<std::uint64_t>(remote.size) + kDiskReserveBytes) {
        scheduleRetry("not enough free disk space", 30'000, false);
        emit(app::EngineEvent::Kind::Notification, "Low disk space",
             "Cannot download " + path + ": not enough free space.", true);
        return;
    }

    auto d = std::make_unique<Download>();
    d->key = "dl-" + crypto::randomHex(6);
    d->folderId = f.row.id;
    d->path = path;
    d->peerId = *f.row.remoteDeviceId;
    d->streamId = nextStream_++;
    d->remote = remote;
    d->conflict = conflictLoser;
    d->part = partPathFor(f, path, remote.hash);
    d->startedMs = d->lastActivityMs = d->sampleMs = now();
    const std::string key = d->key;
    f.active.insert(path);
    persistDownload(*d, "queued");
    downloads_[key] = std::move(d);
    f.sm.moveTo(SyncState::Queued);
    recompute(f);
    emit(app::EngineEvent::Kind::TransfersChanged);

    if (!remote.isDir && remote.size > 0) {
        auto same = fileRepo_.findLiveByHash(f.row.id, remote.hash);
        if (same && *same && (*same)->path != path && (*same)->size == remote.size) {
            beginLocalCopy(key, **same);   // rename/duplicate: no network needed
            return;
        }
    }
    beginNetworkDownload(key);
}

void SyncManager::beginNetworkDownload(const std::string& key) {
    auto it = downloads_.find(key);
    if (it == downloads_.end()) return;
    Download& d = *it->second;
    auto* f = folderById(d.folderId);
    if (!f) return;

    std::error_code ec;
    fs::create_directories(d.part.parent_path(), ec);
    if (ec) {
        failDownload(key, "cannot create temp directory: " + ec.message(), false);
        return;
    }
    const bool hasPart = fs::exists(d.part, ec) && fs::file_size(d.part, ec) > 0;
    if (!hasPart) {
        onWriterReady(key, transfer::ChunkWriter::createFresh(d.part, static_cast<std::uint64_t>(d.remote.size), d.remote.hash));
        return;
    }
    // Resume: re-hash the existing prefix off the network thread.
    std::weak_ptr<bool> alive = alive_;
    const auto part = d.part;
    const auto size = static_cast<std::uint64_t>(d.remote.size);
    const auto hash = d.remote.hash;
    deps_.workers.post([this, alive, key, part, size, hash] {
        auto w = transfer::ChunkWriter::resume(part, size, hash);
        if (cancelWork_) return;
        asio::post(deps_.io, [this, alive, key, w = std::move(w)]() mutable {
            if (!alive.lock()) return;
            onWriterReady(key, std::move(w));
        });
    });
}

void SyncManager::onWriterReady(const std::string& key, Result<std::unique_ptr<transfer::ChunkWriter>> w) {
    auto it = downloads_.find(key);
    if (it == downloads_.end()) {
        if (w) (*w)->abort(false);   // download was cancelled while preparing; keep part
        return;
    }
    Download& d = *it->second;
    if (!w) {
        failDownload(key, w.error().message, false);
        return;
    }
    d.writer = std::move(*w);
    d.bytes = static_cast<std::int64_t>(d.writer->received());
    d.resumedFrom = d.writer->received();
    auto* session = deps_.devices.trustedSession(d.peerId);
    if (!session) {
        failDownload(key, "peer not connected", true, false);
        return;
    }
    sendFileRequest(d, *session);
}

void SyncManager::sendFileRequest(Download& d, device::PeerSession& s) {
    auto* f = folderById(d.folderId);
    d.phase = Download::Phase::Requested;
    d.lastActivityMs = now();
    s.send(proto::type::kFileRequest,
           {{"folder", f->row.displayName},
            {"path", d.path},
            {"transfer_id", d.streamId},
            {"offset", d.writer->received()},
            {"hash", d.remote.hash}});
}

void SyncManager::beginLocalCopy(const std::string& key, const FileRow& source) {
    auto it = downloads_.find(key);
    if (it == downloads_.end()) return;
    Download& d = *it->second;
    auto* f = folderById(d.folderId);
    d.localCopy = true;
    d.phase = Download::Phase::Receiving;

    auto srcPath = pathsafety::resolveInside(rootOf(*f), source.path);
    std::error_code ec;
    fs::create_directories(d.part.parent_path(), ec);
    if (!srcPath || ec) {
        d.localCopy = false;
        beginNetworkDownload(key);
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    const auto part = d.part;
    const auto expected = d.remote.hash;
    const auto src = *srcPath;
    deps_.workers.post([this, alive, key, part, expected, src] {
        bool ok = false;
        {
            std::ifstream in(src, std::ios::binary);
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            crypto::Sha256 h;
            std::string buf(1 << 20, '\0');
            while (in && out && !cancelWork_) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const auto n = in.gcount();
                if (n <= 0) break;
                h.update(buf.data(), static_cast<std::size_t>(n));
                out.write(buf.data(), n);
            }
            out.flush();
            ok = !in.bad() && static_cast<bool>(out) && h.finishHex() == expected;
        }
        if (cancelWork_) return;
        asio::post(deps_.io, [this, alive, key, ok, part] {
            if (!alive.lock()) return;
            auto it2 = downloads_.find(key);
            if (it2 == downloads_.end()) {
                std::error_code ec2;
                fs::remove(part, ec2);
                return;
            }
            if (ok) {
                it2->second->bytes = it2->second->remote.size;
                completeDownload(key);
            } else {   // source changed underneath us: fetch from the peer instead
                std::error_code ec2;
                fs::remove(part, ec2);
                it2->second->localCopy = false;
                it2->second->phase = Download::Phase::Preparing;
                beginNetworkDownload(key);
            }
        });
    });
}

void SyncManager::finishDownload(const std::string& key) {
    auto it = downloads_.find(key);
    if (it == downloads_.end()) return;
    Download& d = *it->second;
    d.phase = Download::Phase::Finishing;
    if (auto* f = folderById(d.folderId)) f->sm.moveTo(SyncState::Verifying);
    persistDownload(d, "verifying");
    auto r = d.writer->finish();   // size + SHA-256 verification
    if (!r) {
        const bool corrupt = r.error().code == ErrorCode::SecurityError;
        if (corrupt) {
            addHistory(d.peerId, d.path, "download", "failure", "integrity check failed; discarded");
            emit(app::EngineEvent::Kind::Notification, "Integrity check failed",
                 d.path + " arrived corrupted and was discarded; it will be retried.", true);
        }
        failDownload(key, r.error().message, /*keepPart=*/!corrupt);
        return;
    }
    d.writer.reset();
    completeDownload(key);
}

void SyncManager::completeDownload(const std::string& key) {
    auto it = downloads_.find(key);
    if (it == downloads_.end()) return;
    Download& d = *it->second;
    auto* f = folderById(d.folderId);
    if (!f) {
        downloads_.erase(it);
        return;
    }
    auto r = applyDownloaded(*f, d);
    if (!r && r.error().code == ErrorCode::Busy) {
        // Not a failure: verified data is kept, we rescan and decide again shortly.
        if (auto* s = deps_.devices.trustedSession(d.peerId); s && !d.localCopy) {
            s->send(proto::type::kFileDone, {{"transfer_id", d.streamId}, {"ok", true}, {"applied", false}});
        }
        persistDownload(d, "paused", r.error().message);
        f->active.erase(d.path);
        f->retry[d.path].nextAtMs = now() + 1500;
        const auto path = d.path;
        downloads_.erase(it);
        (void)path;
        requestScan(*f);
        recompute(*f);
        emit(app::EngineEvent::Kind::TransfersChanged);
        pumpDownloads();
        return;
    }
    if (!r) {
        if (auto* s = deps_.devices.trustedSession(d.peerId); s && !d.localCopy) {
            s->send(proto::type::kFileDone, {{"transfer_id", d.streamId}, {"ok", false}, {"reason", r.error().message}});
        }
        failDownload(key, r.error().message, /*keepPart=*/false);
        return;
    }
    if (auto* s = deps_.devices.trustedSession(d.peerId); s && !d.localCopy) {
        s->send(proto::type::kFileDone, {{"transfer_id", d.streamId}, {"ok", true}});
    }
    d.bytes = d.remote.size;
    persistDownload(d, "completed");
    addHistory(d.peerId, d.path, "download", "success",
               d.localCopy ? "copied from an identical local file"
                       : std::to_string(d.remote.size) + " bytes, verified" +
                             (d.resumedFrom > 0 ? " (resumed from " + std::to_string(d.resumedFrom) + ")" : ""));
    f->active.erase(d.path);
    f->retry.erase(d.path);
    f->didWork = true;
    const std::string donePath = d.path;
    downloads_.erase(it);
    announce(*f, {donePath});   // tell the peer our new state so both sides converge on equal vectors
    recompute(*f);
    emit(app::EngineEvent::Kind::TransfersChanged);
    emit(app::EngineEvent::Kind::FoldersChanged);
    pumpDownloads();
}

void SyncManager::failDownload(const std::string& key, const std::string& reason, bool keepPart,
                               bool countAsFailure) {
    auto it = downloads_.find(key);
    if (it == downloads_.end()) return;
    Download& d = *it->second;
    auto* f = folderById(d.folderId);

    if (d.writer) {
        d.writer->abort(!keepPart);
    } else if (!keepPart) {
        std::error_code ec;
        fs::remove(d.part, ec);
    }
    if (!d.localCopy && (d.phase == Download::Phase::Requested || d.phase == Download::Phase::Receiving)) {
        if (auto* s = deps_.devices.trustedSession(d.peerId)) {
            s->send(proto::type::kFileCancel, {{"transfer_id", d.streamId}});
        }
    }
    const std::string status = !d.endState.empty() ? d.endState : (countAsFailure ? "failed" : "paused");
    persistDownload(d, status, reason);
    if (countAsFailure && d.endState.empty()) {
        addHistory(d.peerId, d.path, "download", "failure", reason);
        if (f) {
            auto& r = f->retry[d.path];
            ++r.fails;
            const TimestampMs delay = std::min<TimestampMs>(kMaxBackoffMs, 2000LL << std::min(r.fails, 8));
            r.nextAtMs = now() + delay;
        }
    }
    if (f) {
        f->active.erase(d.path);
        recompute(*f);
    }
    downloads_.erase(it);
    emit(app::EngineEvent::Kind::TransfersChanged);
    std::weak_ptr<bool> alive = alive_;
    asio::post(deps_.io, [this, alive] { if (alive.lock()) pumpDownloads(); });
}

void SyncManager::persistDownload(const Download& d, const std::string& status, const std::string& error) {
    storage::TransferRow t;
    t.id = d.key;
    t.folderId = d.folderId;
    t.path = d.path;
    t.direction = "download";
    t.sourceDeviceId = d.peerId;
    t.destDeviceId = deps_.localDeviceId;
    t.status = status;
    t.bytesTransferred = d.bytes;
    t.totalBytes = d.remote.size;
    t.hash = d.remote.hash;
    t.error = error;
    t.startedAt = d.startedMs;
    if (status == "completed" || status == "failed" || status == "cancelled" || status == "paused") {
        t.completedAt = now();
    }
    (void)transferRepo_.upsert(t);
}

// ================================================================ uploads

void SyncManager::sendFileError(device::PeerSession& s, std::uint32_t streamId, const std::string& code,
                                const std::string& msg) {
    s.send(proto::type::kFileError, {{"transfer_id", streamId}, {"code", code}, {"message", msg}});
}

void SyncManager::handleFileRequest(device::PeerSession& s, const proto::Message& m) {
    const auto& p = m.payload;
    if (!p.contains("transfer_id") || !p["transfer_id"].is_number_unsigned() || !p.contains("path") ||
        !p["path"].is_string() || !p.contains("folder") || !p["folder"].is_string() ||
        !p.contains("offset") || !p["offset"].is_number_unsigned()) {
        s.close(Error{ErrorCode::ProtocolError, "malformed file_request"});
        return;
    }
    const auto streamId = static_cast<std::uint32_t>(p["transfer_id"].get<std::uint64_t>());
    const auto offset = p["offset"].get<std::uint64_t>();
    const auto wantHash = p.value("hash", std::string{});

    // Every refusal below is silent about WHY a path exists or not beyond a coarse code.
    auto* f = folderByShare(p["folder"].get<std::string>(), s.peerId());
    if (!f || !f->row.enabled || f->paused || f->row.direction == "receive_only") {
        sendFileError(s, streamId, "forbidden", "folder is not shared with this device");
        return;
    }
    auto path = pathsafety::validateRelativePath(p["path"].get<std::string>());
    if (!path) {
        LS_WARN("peer {} requested an invalid path", s.peerId().substr(0, 8));
        sendFileError(s, streamId, "forbidden", "invalid path");
        return;
    }
    auto row = fileRepo_.get(f->row.id, *path);
    if (!row || !*row || (*row)->deleted || (*row)->isDir) {
        sendFileError(s, streamId, "not_found", "no such file");
        return;
    }
    if (!wantHash.empty() && wantHash != (*row)->hash) {
        sendFileError(s, streamId, "changed", "file has a different version now");
        return;
    }
    auto abs = pathsafety::resolveInside(rootOf(*f), *path);
    if (!abs) {
        sendFileError(s, streamId, "forbidden", "path not allowed");
        return;
    }
    std::error_code ec;
    const auto size = fs::file_size(*abs, ec);
    const auto mt = fsys::fileMtimeMs(*abs);
    if (ec || !mt || static_cast<std::int64_t>(size) != (*row)->size || *mt != (*row)->modifiedAt) {
        requestScan(*f);   // our index is stale; refresh it and let the peer retry
        sendFileError(s, streamId, "changed", "file changed on disk");
        return;
    }
    if (offset > size) {
        sendFileError(s, streamId, "bad_offset", "offset beyond end of file");
        return;
    }
    std::size_t activeForPeer = 0;
    for (const auto& [k, u] : uploads_) {
        if (u->peerId == s.peerId()) ++activeForPeer;
        if (u->peerId == s.peerId() && u->streamId == streamId) {
            sendFileError(s, streamId, "busy", "duplicate transfer id");
            return;
        }
    }
    if (activeForPeer >= static_cast<std::size_t>(deps_.settings.maxConcurrentTransfers) * 2) {
        sendFileError(s, streamId, "busy", "too many concurrent transfers");
        return;
    }

    auto u = std::make_unique<Upload>();
    u->key = "ul-" + crypto::randomHex(6);
    u->folderId = f->row.id;
    u->path = *path;
    u->peerId = s.peerId();
    u->streamId = streamId;
    u->total = size;
    u->sent = offset;
    u->in.open(*abs, std::ios::binary);
    if (!u->in) {
        sendFileError(s, streamId, "io_error", "cannot open file");
        return;
    }
    u->in.seekg(static_cast<std::streamoff>(offset));
    u->startedMs = u->lastActivityMs = u->sampleMs = now();
    u->sampleBytes = static_cast<std::int64_t>(offset);
    const std::string key = u->key;
    uploads_[key] = std::move(u);

    s.send(proto::type::kFileBegin, {{"transfer_id", streamId},
                                     {"size", size},
                                     {"hash", (*row)->hash},
                                     {"mtime", (*row)->modifiedAt},
                                     {"offset", offset}});
    emit(app::EngineEvent::Kind::TransfersChanged);
    pumpUpload(key);
}

void SyncManager::pumpUpload(const std::string& key) {
    auto it = uploads_.find(key);
    if (it == uploads_.end()) return;
    Upload& u = *it->second;
    auto* s = deps_.devices.trustedSession(u.peerId);
    if (!s) {
        closeUpload(key, false, "peer disconnected");
        return;
    }
    if (u.cancelled) {
        closeUpload(key, false, "cancelled");
        return;
    }
    if (u.sent >= u.total) {
        if (!u.endSent) {
            u.endSent = true;
            s->send(proto::type::kFileEnd, {{"transfer_id", u.streamId}});
        }
        return;   // completion is reported by the receiver's file_done
    }
    const auto chunk = static_cast<std::uint64_t>(std::min<std::size_t>(deps_.settings.chunkSizeBytes(), 4 * 1024 * 1024));
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, u.total - u.sent));
    std::string buf(n, '\0');
    u.in.read(buf.data(), static_cast<std::streamsize>(n));
    if (static_cast<std::size_t>(u.in.gcount()) != n) {
        sendFileError(*s, u.streamId, "io_error", "read failed");
        closeUpload(key, false, "read failed");
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    s->sendChunk(u.streamId, buf, [this, alive, key, n](bool ok) {
        if (!alive.lock()) return;
        auto it2 = uploads_.find(key);
        if (it2 == uploads_.end()) return;
        if (!ok) {
            closeUpload(key, false, "connection error");
            return;
        }
        Upload& up = *it2->second;
        up.sent += n;
        up.lastActivityMs = now();
        updateSpeed(static_cast<std::int64_t>(up.sent), now(), up.sampleBytes, up.sampleMs, up.speedBps);
        pumpUpload(key);   // next chunk only after this one left our queue: bounded memory
    });
}

void SyncManager::closeUpload(const std::string& key, bool success, const std::string& detail, bool notifyPeer) {
    auto it = uploads_.find(key);
    if (it == uploads_.end()) return;
    Upload& u = *it->second;
    if (notifyPeer && !success) {
        if (auto* s = deps_.devices.trustedSession(u.peerId)) sendFileError(*s, u.streamId, "aborted", detail);
    }
    addHistory(u.peerId, u.path, "upload", success ? "success" : "failure",
               success ? std::to_string(u.total) + " bytes" : detail);
    storage::TransferRow t;
    t.id = u.key;
    t.folderId = u.folderId;
    t.path = u.path;
    t.direction = "upload";
    t.sourceDeviceId = deps_.localDeviceId;
    t.destDeviceId = u.peerId;
    t.status = success ? "completed" : "failed";
    t.bytesTransferred = static_cast<std::int64_t>(u.sent);
    t.totalBytes = static_cast<std::int64_t>(u.total);
    t.error = success ? "" : detail;
    t.startedAt = u.startedMs;
    t.completedAt = now();
    (void)transferRepo_.upsert(t);
    uploads_.erase(it);
    emit(app::EngineEvent::Kind::TransfersChanged);
}

// ================================================================ user actions on transfers

namespace {
// Queued/paused-by-user entries are addressed as "q|<folderId>|<path>".
bool parseQueued(const std::string& id, std::string& folderId, std::string& path) {
    if (id.rfind("q|", 0) != 0) return false;
    const auto second = id.find('|', 2);
    if (second == std::string::npos) return false;
    folderId = id.substr(2, second - 2);
    path = id.substr(second + 1);
    return true;
}
}  // namespace

Result<void> SyncManager::pauseTransfer(const std::string& id) {
    std::string folderId, path;
    if (parseQueued(id, folderId, path)) {
        auto* f = folderById(folderId);
        if (!f) return Error{ErrorCode::NotFound, "unknown transfer"};
        f->userPaused.insert(path);
        f->queue.erase(std::remove(f->queue.begin(), f->queue.end(), path), f->queue.end());
        f->queued.erase(path);
        recompute(*f);
        emit(app::EngineEvent::Kind::TransfersChanged);
        return {};
    }
    const auto it = downloads_.find(id);
    if (it == downloads_.end()) return Error{ErrorCode::NotFound, "only downloads can be paused"};
    auto* f = folderById(it->second->folderId);
    if (f) f->userPaused.insert(it->second->path);
    it->second->endState = "paused";
    failDownload(id, "paused by user", /*keepPart=*/true, false);   // part file kept for resume
    return {};
}

Result<void> SyncManager::resumeTransfer(const std::string& id) {
    std::string folderId, path;
    if (!parseQueued(id, folderId, path)) return Error{ErrorCode::NotFound, "unknown transfer"};
    auto* f = folderById(folderId);
    if (!f) return Error{ErrorCode::NotFound, "unknown transfer"};
    f->userPaused.erase(path);
    f->suppressUntil.erase(path);
    f->retry.erase(path);
    evaluatePaths(*f, {path});
    emit(app::EngineEvent::Kind::TransfersChanged);
    return {};
}

Result<void> SyncManager::cancelTransfer(const std::string& id) {
    std::string folderId, path;
    if (parseQueued(id, folderId, path)) {
        auto* f = folderById(folderId);
        if (!f) return Error{ErrorCode::NotFound, "unknown transfer"};
        f->userPaused.erase(path);
        f->queue.erase(std::remove(f->queue.begin(), f->queue.end(), path), f->queue.end());
        f->queued.erase(path);
        f->suppressUntil[path] = now() + kUserCancelSuppressMs;
        recompute(*f);
        emit(app::EngineEvent::Kind::TransfersChanged);
        return {};
    }
    if (const auto it = downloads_.find(id); it != downloads_.end()) {
        if (auto* f = folderById(it->second->folderId)) f->suppressUntil[it->second->path] = now() + kUserCancelSuppressMs;
        it->second->endState = "cancelled";
        failDownload(id, "cancelled by user", /*keepPart=*/false, false);
        return {};
    }
    if (uploads_.count(id)) {
        closeUpload(id, false, "cancelled by user", /*notifyPeer=*/true);
        return {};
    }
    return Error{ErrorCode::NotFound, "unknown transfer"};
}

std::vector<app::TransferInfo> SyncManager::transferInfos() {
    std::vector<app::TransferInfo> out;
    auto peerName = [&](const std::string& id) { return deps_.devices.deviceName(id); };
    auto folderName = [&](const std::string& id) {
        auto* f = folderById(id);
        return f ? f->row.displayName : std::string{};
    };

    for (const auto& [k, d] : downloads_) {
        app::TransferInfo t;
        t.id = k;
        t.folderId = d->folderId;
        t.folderName = folderName(d->folderId);
        t.path = d->path;
        t.direction = "download";
        t.peerId = d->peerId;
        t.peerName = peerName(d->peerId);
        t.status = d->phase == Download::Phase::Finishing ? "verifying"
                   : d->phase == Download::Phase::Receiving ? "transferring" : "queued";
        t.bytes = d->bytes;
        t.total = d->remote.size;
        t.speedBps = d->speedBps;
        if (d->speedBps > 1 && t.total > t.bytes) {
            t.etaSec = static_cast<std::int64_t>(static_cast<double>(t.total - t.bytes) / d->speedBps);
        }
        t.startedMs = d->startedMs;
        out.push_back(std::move(t));
    }
    for (const auto& [k, u] : uploads_) {
        app::TransferInfo t;
        t.id = k;
        t.folderId = u->folderId;
        t.folderName = folderName(u->folderId);
        t.path = u->path;
        t.direction = "upload";
        t.peerId = u->peerId;
        t.peerName = peerName(u->peerId);
        t.status = "transferring";
        t.bytes = static_cast<std::int64_t>(u->sent);
        t.total = static_cast<std::int64_t>(u->total);
        t.speedBps = u->speedBps;
        if (u->speedBps > 1 && t.total > t.bytes) {
            t.etaSec = static_cast<std::int64_t>(static_cast<double>(t.total - t.bytes) / u->speedBps);
        }
        t.startedMs = u->startedMs;
        out.push_back(std::move(t));
    }

    int queuedShown = 0;
    for (const auto& [id, f] : folders_) {
        auto add = [&](const std::string& path, const char* status) {
            app::TransferInfo t;
            t.id = "q|" + id + "|" + path;
            t.folderId = id;
            t.folderName = f->row.displayName;
            t.path = path;
            t.direction = "download";
            t.peerId = f->row.remoteDeviceId.value_or("");
            t.peerName = peerName(t.peerId);
            t.status = status;
            const auto r = f->remote.find(path);
            if (r != f->remote.end()) t.total = r->second.size;
            out.push_back(std::move(t));
        };
        for (const auto& p : f->userPaused) add(p, "paused");
        for (const auto& p : f->queue) {
            if (queuedShown++ >= 100) break;
            add(p, "queued");
        }
    }

    auto recent = transferRepo_.recent(40);
    if (recent) {
        for (const auto& r : *recent) {
            if (r.status != "completed" && r.status != "failed" && r.status != "cancelled") continue;
            if (downloads_.count(r.id) || uploads_.count(r.id)) continue;
            app::TransferInfo t;
            t.id = r.id;
            t.folderId = r.folderId;
            t.folderName = folderName(r.folderId);
            t.path = r.path;
            t.direction = r.direction;
            t.peerId = r.direction == "download" ? r.sourceDeviceId : r.destDeviceId;
            t.peerName = peerName(t.peerId);
            t.status = r.status;
            t.bytes = r.bytesTransferred;
            t.total = r.totalBytes;
            t.error = r.error;
            t.startedMs = r.startedAt.value_or(0);
            out.push_back(std::move(t));
        }
    }
    return out;
}

}  // namespace lansync::sync
