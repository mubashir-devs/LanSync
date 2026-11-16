#pragma once
// Typed access to each table. Repositories are thin: they own SQL, not policy.
// Like Database, they are confined to the engine thread.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/common/clock.hpp"
#include "storage/database/database.hpp"

namespace lansync::storage {

// ---------------------------------------------------------------- devices
struct DeviceRow {
    std::string id;
    std::string name;
    std::optional<std::string> publicKeyPem;
    std::optional<std::string> fingerprint;
    std::optional<std::string> address;
    std::optional<std::int64_t> port;
    std::string pairingStatus{"unpaired"};   // unpaired|paired|revoked (+ transient)
    std::optional<std::int64_t> lastSeenAt;
    TimestampMs createdAt{0};
};

class DeviceRepository {
public:
    explicit DeviceRepository(Database& db) : db_(db) {}
    Result<void> upsert(const DeviceRow& d);
    Result<std::optional<DeviceRow>> get(const std::string& id);
    Result<std::vector<DeviceRow>> list();
    Result<void> setAddress(const std::string& id, const std::string& address, int port,
                            TimestampMs lastSeen);
    // Revokes trust: clears the pinned key/fingerprint and marks the row revoked.
    Result<void> revoke(const std::string& id);
    Result<void> remove(const std::string& id);

private:
    Database& db_;
};

// ---------------------------------------------------------------- folders
struct FolderRow {
    std::string id;
    std::string localPath;
    std::string displayName;   // also the share name matched on both devices
    std::optional<std::string> remoteDeviceId;
    std::string direction{"bidirectional"};   // bidirectional|send_only|receive_only
    bool enabled{true};
    TimestampMs createdAt{0};
};

class FolderRepository {
public:
    explicit FolderRepository(Database& db) : db_(db) {}
    Result<void> insert(const FolderRow& f);
    Result<std::vector<FolderRow>> list();
    Result<std::optional<FolderRow>> get(const std::string& id);
    Result<std::optional<FolderRow>> findByShare(const std::string& name,
                                                 const std::string& remoteDeviceId);
    Result<void> setEnabled(const std::string& id, bool enabled);
    Result<void> setDirection(const std::string& id, const std::string& direction);
    Result<void> remove(const std::string& id);

private:
    Database& db_;
};

// ---------------------------------------------------------------- files
struct FileRow {
    std::int64_t fileId{0};
    std::string folderId;
    std::string path;          // relative, '/'-separated
    bool isDir{false};
    std::int64_t size{0};
    std::int64_t modifiedAt{0};
    std::string hash;          // hex SHA-256; empty for directories
    bool deleted{false};
    std::string versionVector{"{}"};   // JSON object
    std::string status{"idle"};
    std::optional<std::int64_t> lastSyncedAt;
};

class FileRepository {
public:
    explicit FileRepository(Database& db) : db_(db) {}
    Result<std::optional<FileRow>> get(const std::string& folderId, const std::string& path);
    Result<std::optional<FileRow>> getById(std::int64_t fileId);
    // Inserts or updates by (folder, path); sets row.fileId.
    Result<void> upsert(FileRow& row);
    Result<std::map<std::string, FileRow>> listFolder(const std::string& folderId);
    // A live (non-deleted) file in the folder with the given content hash.
    Result<std::optional<FileRow>> findLiveByHash(const std::string& folderId,
                                                  const std::string& hash);
    Result<void> setStatus(const std::string& folderId, const std::string& path,
                           const std::string& status);
    Result<std::int64_t> countByStatus(const std::string& folderId, const std::string& status);
    Result<std::int64_t> countLive(const std::string& folderId);
    Result<std::int64_t> countSyncedLive();
    // Newest last_synced_at in the folder (0 if never synced).
    Result<std::int64_t> lastSyncedAt(const std::string& folderId);

private:
    Database& db_;
};

// ---------------------------------------------------------------- transfers
struct TransferRow {
    std::string id;
    std::string folderId;
    std::string path;
    std::optional<std::int64_t> fileId;
    std::string direction;   // download|upload
    std::string sourceDeviceId;
    std::string destDeviceId;
    std::string status{"queued"};
    std::int64_t bytesTransferred{0};
    std::int64_t totalBytes{0};
    std::string hash;
    std::string error;
    std::optional<std::int64_t> startedAt;
    std::optional<std::int64_t> completedAt;
};

class TransferRepository {
public:
    explicit TransferRepository(Database& db) : db_(db) {}
    Result<void> upsert(const TransferRow& t);
    Result<std::vector<TransferRow>> recent(int limit);
    // After a crash: anything still active becomes 'failed: interrupted'.
    Result<int> failInterrupted(TimestampMs now);

private:
    Database& db_;
};

// ---------------------------------------------------------------- versions
struct VersionRow {
    std::int64_t versionId{0};
    std::int64_t fileId{0};
    std::string hash;
    std::int64_t size{0};
    std::int64_t modifiedAt{0};
    std::string storageLocation;
    TimestampMs createdAt{0};
};

class VersionRepository {
public:
    explicit VersionRepository(Database& db) : db_(db) {}
    Result<void> insert(VersionRow& v);
    Result<std::optional<VersionRow>> get(std::int64_t versionId);
    // Newest first.
    Result<std::vector<VersionRow>> listForFile(std::int64_t fileId);
    Result<std::vector<VersionRow>> listAll();
    Result<void> remove(std::int64_t versionId);

private:
    Database& db_;
};

// ---------------------------------------------------------------- conflicts
struct ConflictRow {
    std::int64_t id{0};
    std::int64_t fileId{0};
    std::string localHash;
    std::string remoteHash;
    std::int64_t localModifiedAt{0};
    std::int64_t remoteModifiedAt{0};
    std::string copyPath;                    // relative path of the preserved loser
    std::optional<std::string> resolution;   // keep_local|keep_remote|keep_both
    TimestampMs detectedAt{0};
    std::optional<std::int64_t> resolvedAt;
};

class ConflictRepository {
public:
    explicit ConflictRepository(Database& db) : db_(db) {}
    Result<void> insert(ConflictRow& c);
    Result<std::optional<ConflictRow>> get(std::int64_t id);
    Result<std::vector<ConflictRow>> list(bool includeResolved);
    Result<void> resolve(std::int64_t id, const std::string& resolution, TimestampMs now);

private:
    Database& db_;
};

// ---------------------------------------------------------------- history
struct HistoryRow {
    std::int64_t id{0};
    std::optional<std::string> deviceId;
    std::optional<std::string> path;
    std::string operation;
    std::string result;   // success|failure|info
    std::string detail;
    TimestampMs timestamp{0};
};

class HistoryRepository {
public:
    explicit HistoryRepository(Database& db) : db_(db) {}
    Result<void> add(const HistoryRow& h);
    Result<std::vector<HistoryRow>> recent(int limit);
    // Keeps only the newest `keep` rows.
    Result<void> trim(int keep);

private:
    Database& db_;
};

// ---------------------------------------------------------------- settings
class SettingsRepository {
public:
    explicit SettingsRepository(Database& db) : db_(db) {}
    Result<std::map<std::string, std::string>> all();
    Result<void> set(const std::string& key, const std::string& value);

private:
    Database& db_;
};

}  // namespace lansync::storage
