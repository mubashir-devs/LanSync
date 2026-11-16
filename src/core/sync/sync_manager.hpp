#pragma once
// SyncManager: the synchronization engine.
//
// Per registered folder it keeps the local index current (scan + native watch +
// periodic reconciliation), exchanges indexes with the paired peer, decides per
// path what to do (see diff.hpp), runs bounded concurrent chunked transfers,
// preserves both versions on conflicts, and archives replaced content.
//
// THREADING: every method runs on the engine's io_context thread. Blocking disk
// work (scans, hashing, resume, local copies) runs on the WorkerPool and posts
// its result back; handlers re-validate state because the world may have changed.
//
// This file declares the class; implementation is split across
//   sync_manager.cpp   folders, scanning, index exchange, evaluation
//   sync_transfers.cpp downloads/uploads and peer protocol handlers
//   sync_apply.cpp     applying decisions to disk (+conflicts, versions, queries)

#include <atomic>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/application/engine_types.hpp"
#include "core/common/clock.hpp"
#include "core/common/worker_pool.hpp"
#include "core/device/device_manager.hpp"
#include "core/sync/diff.hpp"
#include "core/sync/sync_state.hpp"
#include "core/transfer/chunk_writer.hpp"
#include "core/versioning/version_store.hpp"
#include "filesystem/scanner/scanner.hpp"
#include "filesystem/watcher/watcher.hpp"
#include "storage/repositories/repositories.hpp"

namespace lansync::sync {

namespace asio = boost::asio;

struct SyncDeps {
    asio::io_context& io;
    storage::Database& db;
    std::string localDeviceId;
    std::filesystem::path dataDir;
    std::shared_ptr<const IClock> clock;
    device::DeviceManager& devices;
    versioning::VersionStore& versions;
    WorkerPool& workers;
    const app::Settings& settings;
    app::EventHandler emit;
    bool enableWatchers{true};
};

class SyncManager : public device::IPeerSink {
public:
    explicit SyncManager(SyncDeps deps);
    ~SyncManager() override;

    Result<void> start();
    void shutdown();

    // ---- folders
    Result<std::string> addFolder(const std::filesystem::path& path, const std::string& name,
                                  const std::string& remoteDeviceId, const std::string& direction);
    Result<void> removeFolder(const std::string& folderId);
    Result<void> setFolderEnabled(const std::string& folderId, bool enabled);
    Result<void> setFolderPaused(const std::string& folderId, bool paused);
    Result<void> setFolderDirection(const std::string& folderId, const std::string& direction);
    // Rescan (optionally verifying content) and re-evaluate; empty id = all folders.
    void syncNow(const std::string& folderId, bool verifyContent = false);

    // ---- transfers (ids are TransferInfo::id)
    Result<void> pauseTransfer(const std::string& id);
    Result<void> resumeTransfer(const std::string& id);
    Result<void> cancelTransfer(const std::string& id);

    // ---- conflicts / versions
    Result<void> resolveConflict(std::int64_t conflictId, const std::string& resolution);
    Result<std::vector<app::VersionInfo>> listVersions(const std::string& folderId,
                                                       const std::string& path);
    Result<void> restoreVersion(std::int64_t versionId);

    // ---- queries
    std::vector<app::FolderInfo> folderInfos();
    std::vector<app::TransferInfo> transferInfos();
    std::vector<app::ConflictInfo> conflictInfos(bool includeResolved = false);
    std::vector<app::HistoryInfo> historyInfos(int limit);
    app::Stats stats();

    // ---- IPeerSink
    void onPeerReady(device::PeerSession& s) override;
    void onPeerMessage(device::PeerSession& s, const protocol::Message& m) override;
    void onPeerChunk(device::PeerSession& s, std::uint32_t streamId, std::string_view data) override;
    void onPeerClosed(const std::string& peerId) override;

private:
    // ------------------------------------------------------------ data
    struct Retry {
        int fails{0};
        TimestampMs nextAtMs{0};
    };

    struct FolderRt {
        storage::FolderRow row;
        SyncStateMachine sm;
        bool paused{false};
        std::unique_ptr<fsys::IWatcher> watcher;
        bool scanning{false};
        bool rescan{false};
        bool rehash{false};
        bool didWork{false};
        std::unique_ptr<asio::steady_timer> debounce;
        std::map<std::string, EntryMeta> remote;     // last index received from the peer
        std::deque<std::string> queue;               // paths waiting for download
        std::set<std::string> queued;
        std::set<std::string> active;                // paths currently downloading
        std::map<std::string, Retry> retry;
        std::set<std::string> userPaused;            // paused individually from the UI
        std::map<std::string, TimestampMs> suppressUntil;   // cancelled from the UI
        std::string error;
        TimestampMs lastSyncMs{0};
        std::int64_t openConflicts{0};
    };

    struct Download {
        std::string key;            // = transfer id shown to the UI / stored in DB
        std::string folderId, path, peerId;
        std::uint32_t streamId{0};
        EntryMeta remote;
        bool conflict{false};
        std::filesystem::path part;
        std::unique_ptr<transfer::ChunkWriter> writer;
        enum class Phase { Preparing, Requested, Receiving, Finishing } phase{Phase::Preparing};
        bool localCopy{false};      // satisfied from an identical local file (no network)
        bool cancelRequested{false};
        std::string endState;       // "paused"/"cancelled" when the user stopped it
        std::int64_t bytes{0};
        std::uint64_t resumedFrom{0};   // bytes already on disk when this attempt began
        TimestampMs startedMs{0};
        TimestampMs lastActivityMs{0};
        TimestampMs sampleMs{0};
        std::int64_t sampleBytes{0};
        double speedBps{0};
    };

    struct Upload {
        std::string key;
        std::string folderId, path, peerId;
        std::uint32_t streamId{0};
        std::ifstream in;
        std::uint64_t total{0}, sent{0};
        TimestampMs startedMs{0}, lastActivityMs{0};
        double speedBps{0};
        std::int64_t sampleBytes{0};
        TimestampMs sampleMs{0};
        bool cancelled{false};
        bool endSent{false};
    };

    // ------------------------------------------------------------ helpers
    FolderRt* folderById(const std::string& id);
    FolderRt* folderByShare(const std::string& name, const std::string& peerId);
    std::filesystem::path rootOf(const FolderRt& f) const { return f.row.localPath; }
    void emit(app::EngineEvent::Kind k, std::string title = {}, std::string text = {}, bool err = false);
    void addHistory(const std::string& deviceId, const std::string& path, const std::string& op,
                    const std::string& result, const std::string& detail = {});
    void recompute(FolderRt& f);
    TimestampMs now() const { return deps_.clock->nowMs(); }
    [[nodiscard]] std::int64_t maxFileBytes() const {
        return deps_.settings.maxFileSizeMb * 1024 * 1024;
    }

    // ------------------------------------------------------------ folders / scanning
    void loadFolders();
    void startRuntime(FolderRt& f);
    void stopRuntime(FolderRt& f);
    void markDirty(const std::string& folderId);
    void requestScan(FolderRt& f, bool rehash = false);
    void onScanDone(const std::string& folderId, Result<fsys::ScanOutput> out);
    void reconcileTick();

    // ------------------------------------------------------------ index exchange
    void sendFullIndex(FolderRt& f, device::PeerSession& s);
    void announce(FolderRt& f, const std::vector<std::string>& paths);
    void sendEntries(device::PeerSession& s, const FolderRt& f, const std::vector<storage::FileRow>& rows);
    void handleIndex(device::PeerSession& s, const protocol::Message& m);
    EntryMeta toMeta(const storage::FileRow& r) const;
    std::optional<EntryMeta> localMeta(const std::string& folderId, const std::string& path,
                                       storage::FileRow* rowOut = nullptr);

    // ------------------------------------------------------------ evaluation
    void evaluateAll(FolderRt& f);
    void evaluatePaths(FolderRt& f, const std::vector<std::string>& paths);
    void enqueue(FolderRt& f, const std::string& path);
    void retryTick();
    void tick();
    void refreshConflictCounts();

    // ------------------------------------------------------------ applying decisions
    void applyMakeDir(FolderRt& f, const std::string& path, const EntryMeta& remote);
    void applyDelete(FolderRt& f, const std::string& path, const EntryMeta& remote);
    void applyMerge(FolderRt& f, const std::string& path, const EntryMeta& remote);
    void applyKeepLocal(FolderRt& f, const std::string& path, const EntryMeta& remote);
    void recordConflictAsWinner(FolderRt& f, const std::string& path, const EntryMeta& remote);
    Result<void> applyDownloaded(FolderRt& f, Download& d);
    void markSynced(FolderRt& f, const std::string& path);

    // ------------------------------------------------------------ transfers
    void pumpDownloads();
    void startDownload(FolderRt& f, const std::string& path);
    void beginNetworkDownload(const std::string& key);
    void beginLocalCopy(const std::string& key, const storage::FileRow& source);
    void sendFileRequest(Download& d, device::PeerSession& s);
    void finishDownload(const std::string& key);
    void failDownload(const std::string& key, const std::string& reason, bool keepPart,
                      bool countAsFailure = true);
    void completeDownload(const std::string& key);
    void onWriterReady(const std::string& key, Result<std::unique_ptr<transfer::ChunkWriter>> w);
    Download* downloadByStream(const std::string& peerId, std::uint32_t streamId);
    void persistDownload(const Download& d, const std::string& status, const std::string& error = {});
    void handleFileRequest(device::PeerSession& s, const protocol::Message& m);
    void pumpUpload(const std::string& key);
    void closeUpload(const std::string& key, bool success, const std::string& detail,
                     bool notifyPeer = false);
    void sendFileError(device::PeerSession& s, std::uint32_t streamId, const std::string& code,
                       const std::string& msg);
    std::filesystem::path partPathFor(const FolderRt& f, const std::string& path,
                                      const std::string& hash) const;
    void updateSpeed(std::int64_t bytes, TimestampMs t, std::int64_t& sampleBytes,
                     TimestampMs& sampleMs, double& speed);

    SyncDeps deps_;
    storage::FolderRepository folderRepo_;
    storage::FileRepository fileRepo_;
    storage::TransferRepository transferRepo_;
    storage::ConflictRepository conflictRepo_;
    storage::HistoryRepository historyRepo_;

    std::map<std::string, std::unique_ptr<FolderRt>> folders_;
    std::map<std::string, std::unique_ptr<Download>> downloads_;
    std::map<std::string, std::unique_ptr<Upload>> uploads_;
    std::uint32_t nextStream_{1};
    std::uint64_t nextKey_{1};

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::atomic<bool> cancelWork_{false};
    asio::steady_timer reconcileTimer_;
    asio::steady_timer tickTimer_;
    int tickCount_{0};
    bool running_{false};
    TimestampMs lastTransferEmit_{0};
    int historySinceTrim_{0};
};

}  // namespace lansync::sync
