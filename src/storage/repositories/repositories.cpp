#include "storage/repositories/repositories.hpp"

#include "storage/database/sql.hpp"

namespace lansync::storage {

// ================================================================ devices
namespace {
constexpr const char* kDeviceCols =
    "device_id,name,public_key_pem,cert_fingerprint,address,port,pairing_status,last_seen_at,created_at";

DeviceRow readDevice(const Statement& s) {
    DeviceRow d;
    d.id = s.columnText(0);
    d.name = s.columnText(1);
    d.publicKeyPem = optText(s, 2);
    d.fingerprint = optText(s, 3);
    d.address = optText(s, 4);
    d.port = optInt(s, 5);
    d.pairingStatus = s.columnText(6);
    d.lastSeenAt = optInt(s, 7);
    d.createdAt = s.columnInt(8);
    return d;
}
}  // namespace

Result<void> DeviceRepository::upsert(const DeviceRow& d) {
    return run(db_,
               "INSERT INTO devices(device_id,name,public_key_pem,cert_fingerprint,address,port,"
               "pairing_status,last_seen_at,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
               "ON CONFLICT(device_id) DO UPDATE SET name=excluded.name,"
               "public_key_pem=excluded.public_key_pem,cert_fingerprint=excluded.cert_fingerprint,"
               "address=excluded.address,port=excluded.port,pairing_status=excluded.pairing_status,"
               "last_seen_at=excluded.last_seen_at;",
               {d.id, d.name, d.publicKeyPem, d.fingerprint, d.address, d.port, d.pairingStatus,
                d.lastSeenAt, d.createdAt});
}

Result<std::optional<DeviceRow>> DeviceRepository::get(const std::string& id) {
    std::optional<DeviceRow> out;
    auto r = query(db_, std::string("SELECT ") + kDeviceCols + " FROM devices WHERE device_id=?1;",
                   {id}, [&](Statement& s) { out = readDevice(s); });
    if (!r) return r.error();
    return out;
}

Result<std::vector<DeviceRow>> DeviceRepository::list() {
    std::vector<DeviceRow> out;
    auto r = query(db_, std::string("SELECT ") + kDeviceCols + " FROM devices ORDER BY name;", {},
                   [&](Statement& s) { out.push_back(readDevice(s)); });
    if (!r) return r.error();
    return out;
}

Result<void> DeviceRepository::setAddress(const std::string& id, const std::string& address,
                                          int port, TimestampMs lastSeen) {
    return run(db_, "UPDATE devices SET address=?2,port=?3,last_seen_at=?4 WHERE device_id=?1;",
               {id, address, port, lastSeen});
}

Result<void> DeviceRepository::revoke(const std::string& id) {
    return run(db_,
               "UPDATE devices SET pairing_status='revoked',public_key_pem=NULL,"
               "cert_fingerprint=NULL WHERE device_id=?1;",
               {id});
}

Result<void> DeviceRepository::remove(const std::string& id) {
    return run(db_, "DELETE FROM devices WHERE device_id=?1;", {id});
}

// ================================================================ folders
namespace {
constexpr const char* kFolderCols =
    "folder_id,local_path,display_name,remote_device_id,direction,enabled,created_at";
FolderRow readFolder(const Statement& s) {
    FolderRow f;
    f.id = s.columnText(0);
    f.localPath = s.columnText(1);
    f.displayName = s.columnText(2);
    f.remoteDeviceId = optText(s, 3);
    f.direction = s.columnText(4);
    f.enabled = s.columnInt(5) != 0;
    f.createdAt = s.columnInt(6);
    return f;
}
}  // namespace

Result<void> FolderRepository::insert(const FolderRow& f) {
    return run(db_,
               "INSERT INTO sync_folders(folder_id,local_path,display_name,remote_device_id,"
               "direction,enabled,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7);",
               {f.id, f.localPath, f.displayName, f.remoteDeviceId, f.direction, f.enabled,
                f.createdAt});
}

Result<std::vector<FolderRow>> FolderRepository::list() {
    std::vector<FolderRow> out;
    auto r = query(db_, std::string("SELECT ") + kFolderCols + " FROM sync_folders ORDER BY created_at;",
                   {}, [&](Statement& s) { out.push_back(readFolder(s)); });
    if (!r) return r.error();
    return out;
}

Result<std::optional<FolderRow>> FolderRepository::get(const std::string& id) {
    std::optional<FolderRow> out;
    auto r = query(db_, std::string("SELECT ") + kFolderCols + " FROM sync_folders WHERE folder_id=?1;",
                   {id}, [&](Statement& s) { out = readFolder(s); });
    if (!r) return r.error();
    return out;
}

Result<std::optional<FolderRow>> FolderRepository::findByShare(const std::string& name,
                                                              const std::string& remote) {
    std::optional<FolderRow> out;
    auto r = query(db_,
                   std::string("SELECT ") + kFolderCols +
                       " FROM sync_folders WHERE display_name=?1 AND remote_device_id=?2;",
                   {name, remote}, [&](Statement& s) { out = readFolder(s); });
    if (!r) return r.error();
    return out;
}

Result<void> FolderRepository::setEnabled(const std::string& id, bool enabled) {
    return run(db_, "UPDATE sync_folders SET enabled=?2 WHERE folder_id=?1;", {id, enabled});
}
Result<void> FolderRepository::setDirection(const std::string& id, const std::string& dir) {
    return run(db_, "UPDATE sync_folders SET direction=?2 WHERE folder_id=?1;", {id, dir});
}
Result<void> FolderRepository::remove(const std::string& id) {
    return run(db_, "DELETE FROM sync_folders WHERE folder_id=?1;", {id});
}

// ================================================================ files
namespace {
constexpr const char* kFileCols =
    "file_id,folder_id,relative_path,is_directory,size,modified_at,content_hash,deleted,"
    "version_vector,sync_status,last_synced_at";
FileRow readFile(const Statement& s) {
    FileRow f;
    f.fileId = s.columnInt(0);
    f.folderId = s.columnText(1);
    f.path = s.columnText(2);
    f.isDir = s.columnInt(3) != 0;
    f.size = s.columnInt(4);
    f.modifiedAt = s.columnInt(5);
    f.hash = optText(s, 6).value_or("");
    f.deleted = s.columnInt(7) != 0;
    f.versionVector = s.columnText(8);
    f.status = s.columnText(9);
    f.lastSyncedAt = optInt(s, 10);
    return f;
}
}  // namespace

Result<std::optional<FileRow>> FileRepository::get(const std::string& folderId,
                                                   const std::string& path) {
    std::optional<FileRow> out;
    auto r = query(db_,
                   std::string("SELECT ") + kFileCols +
                       " FROM files WHERE folder_id=?1 AND relative_path=?2;",
                   {folderId, path}, [&](Statement& s) { out = readFile(s); });
    if (!r) return r.error();
    return out;
}

Result<std::optional<FileRow>> FileRepository::getById(std::int64_t id) {
    std::optional<FileRow> out;
    auto r = query(db_, std::string("SELECT ") + kFileCols + " FROM files WHERE file_id=?1;", {id},
                   [&](Statement& s) { out = readFile(s); });
    if (!r) return r.error();
    return out;
}

Result<void> FileRepository::upsert(FileRow& row) {
    const std::optional<std::string> hash =
        row.hash.empty() ? std::nullopt : std::optional<std::string>(row.hash);
    auto r = run(db_,
                 "INSERT INTO files(folder_id,relative_path,is_directory,size,modified_at,"
                 "content_hash,deleted,version_vector,sync_status,last_synced_at) "
                 "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
                 "ON CONFLICT(folder_id,relative_path) DO UPDATE SET is_directory=excluded.is_directory,"
                 "size=excluded.size,modified_at=excluded.modified_at,content_hash=excluded.content_hash,"
                 "deleted=excluded.deleted,version_vector=excluded.version_vector,"
                 "sync_status=excluded.sync_status,last_synced_at=excluded.last_synced_at;",
                 {row.folderId, row.path, row.isDir, row.size, row.modifiedAt, hash, row.deleted,
                  row.versionVector, row.status, row.lastSyncedAt});
    if (!r) return r;
    // Fetch the id with a parameterised query (paths are untrusted).
    auto got = get(row.folderId, row.path);
    if (!got) return got.error();
    if (*got) row.fileId = (*got)->fileId;
    return {};
}

Result<std::map<std::string, FileRow>> FileRepository::listFolder(const std::string& folderId) {
    std::map<std::string, FileRow> out;
    auto r = query(db_, std::string("SELECT ") + kFileCols + " FROM files WHERE folder_id=?1;",
                   {folderId}, [&](Statement& s) {
                       auto f = readFile(s);
                       out.emplace(f.path, std::move(f));
                   });
    if (!r) return r.error();
    return out;
}

Result<std::optional<FileRow>> FileRepository::findLiveByHash(const std::string& folderId,
                                                              const std::string& hash) {
    std::optional<FileRow> out;
    auto r = query(db_,
                   std::string("SELECT ") + kFileCols +
                       " FROM files WHERE folder_id=?1 AND content_hash=?2 AND deleted=0 "
                       "AND is_directory=0 LIMIT 1;",
                   {folderId, hash}, [&](Statement& s) { out = readFile(s); });
    if (!r) return r.error();
    return out;
}

Result<void> FileRepository::setStatus(const std::string& folderId, const std::string& path,
                                       const std::string& status) {
    return run(db_, "UPDATE files SET sync_status=?3 WHERE folder_id=?1 AND relative_path=?2;",
               {folderId, path, status});
}

Result<std::int64_t> FileRepository::countByStatus(const std::string& folderId,
                                                   const std::string& status) {
    std::int64_t n = 0;
    auto r = query(db_, "SELECT COUNT(*) FROM files WHERE folder_id=?1 AND sync_status=?2;",
                   {folderId, status}, [&](Statement& s) { n = s.columnInt(0); });
    if (!r) return r.error();
    return n;
}

Result<std::int64_t> FileRepository::countLive(const std::string& folderId) {
    std::int64_t n = 0;
    auto r = query(db_, "SELECT COUNT(*) FROM files WHERE folder_id=?1 AND deleted=0 AND is_directory=0;",
                   {folderId}, [&](Statement& s) { n = s.columnInt(0); });
    if (!r) return r.error();
    return n;
}

Result<std::int64_t> FileRepository::countSyncedLive() {
    std::int64_t n = 0;
    auto r = query(db_,
                   "SELECT COUNT(*) FROM files WHERE deleted=0 AND is_directory=0 AND "
                   "last_synced_at IS NOT NULL;",
                   {}, [&](Statement& s) { n = s.columnInt(0); });
    if (!r) return r.error();
    return n;
}

Result<std::int64_t> FileRepository::lastSyncedAt(const std::string& folderId) {
    std::int64_t t = 0;
    auto r = query(db_, "SELECT COALESCE(MAX(last_synced_at),0) FROM files WHERE folder_id=?1;", {folderId},
                   [&](Statement& s) { t = s.columnInt(0); });
    if (!r) return r.error();
    return t;
}

// ================================================================ transfers
namespace {
TransferRow readTransfer(const Statement& s) {
    TransferRow t;
    t.id = s.columnText(0);
    t.folderId = s.columnText(1);
    t.path = s.columnText(2);
    t.fileId = optInt(s, 3);
    t.direction = s.columnText(4);
    t.sourceDeviceId = s.columnText(5);
    t.destDeviceId = s.columnText(6);
    t.status = s.columnText(7);
    t.bytesTransferred = s.columnInt(8);
    t.totalBytes = s.columnInt(9);
    t.hash = optText(s, 10).value_or("");
    t.error = optText(s, 11).value_or("");
    t.startedAt = optInt(s, 12);
    t.completedAt = optInt(s, 13);
    return t;
}
}  // namespace

Result<void> TransferRepository::upsert(const TransferRow& t) {
    return run(db_,
               "INSERT INTO transfers(transfer_id,folder_id,relative_path,file_id,direction,"
               "source_device_id,dest_device_id,status,bytes_transferred,total_bytes,content_hash,"
               "error,started_at,completed_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) "
               "ON CONFLICT(transfer_id) DO UPDATE SET status=excluded.status,"
               "bytes_transferred=excluded.bytes_transferred,total_bytes=excluded.total_bytes,"
               "error=excluded.error,started_at=excluded.started_at,completed_at=excluded.completed_at,"
               "file_id=excluded.file_id;",
               {t.id, t.folderId, t.path, t.fileId, t.direction, t.sourceDeviceId, t.destDeviceId,
                t.status, t.bytesTransferred, t.totalBytes, t.hash, t.error, t.startedAt,
                t.completedAt});
}

Result<std::vector<TransferRow>> TransferRepository::recent(int limit) {
    std::vector<TransferRow> out;
    auto r = query(db_,
                   "SELECT transfer_id,folder_id,relative_path,file_id,direction,source_device_id,"
                   "dest_device_id,status,bytes_transferred,total_bytes,content_hash,error,"
                   "started_at,completed_at FROM transfers ORDER BY COALESCE(started_at,0) DESC "
                   "LIMIT ?1;",
                   {limit}, [&](Statement& s) { out.push_back(readTransfer(s)); });
    if (!r) return r.error();
    return out;
}

Result<int> TransferRepository::failInterrupted(TimestampMs now) {
    auto r = run(db_,
                 "UPDATE transfers SET status='failed',error='interrupted by shutdown',"
                 "completed_at=?1 WHERE status IN ('queued','transferring','verifying');",
                 {now});
    if (!r) return r.error();
    return db_.changes();
}

// ================================================================ versions
namespace {
VersionRow readVersion(const Statement& s) {
    VersionRow v;
    v.versionId = s.columnInt(0);
    v.fileId = s.columnInt(1);
    v.hash = s.columnText(2);
    v.size = s.columnInt(3);
    v.modifiedAt = s.columnInt(4);
    v.storageLocation = s.columnText(5);
    v.createdAt = s.columnInt(6);
    return v;
}
constexpr const char* kVersionCols =
    "version_id,file_id,content_hash,size,modified_at,storage_location,created_at";
}  // namespace

Result<void> VersionRepository::insert(VersionRow& v) {
    auto r = run(db_,
                 "INSERT INTO file_versions(file_id,content_hash,size,modified_at,storage_location,"
                 "created_at) VALUES(?1,?2,?3,?4,?5,?6);",
                 {v.fileId, v.hash, v.size, v.modifiedAt, v.storageLocation, v.createdAt});
    if (!r) return r;
    v.versionId = db_.lastInsertRowId();
    return {};
}

Result<std::optional<VersionRow>> VersionRepository::get(std::int64_t id) {
    std::optional<VersionRow> out;
    auto r = query(db_, std::string("SELECT ") + kVersionCols + " FROM file_versions WHERE version_id=?1;",
                   {id}, [&](Statement& s) { out = readVersion(s); });
    if (!r) return r.error();
    return out;
}

Result<std::vector<VersionRow>> VersionRepository::listForFile(std::int64_t fileId) {
    std::vector<VersionRow> out;
    auto r = query(db_,
                   std::string("SELECT ") + kVersionCols +
                       " FROM file_versions WHERE file_id=?1 ORDER BY created_at DESC, version_id DESC;",
                   {fileId}, [&](Statement& s) { out.push_back(readVersion(s)); });
    if (!r) return r.error();
    return out;
}

Result<std::vector<VersionRow>> VersionRepository::listAll() {
    std::vector<VersionRow> out;
    auto r = query(db_,
                   std::string("SELECT ") + kVersionCols +
                       " FROM file_versions ORDER BY file_id, created_at DESC, version_id DESC;",
                   {}, [&](Statement& s) { out.push_back(readVersion(s)); });
    if (!r) return r.error();
    return out;
}

Result<void> VersionRepository::remove(std::int64_t id) {
    return run(db_, "DELETE FROM file_versions WHERE version_id=?1;", {id});
}

// ================================================================ conflicts
namespace {
constexpr const char* kConflictCols =
    "conflict_id,file_id,local_hash,remote_hash,local_modified_at,remote_modified_at,"
    "conflict_copy_path,resolution,detected_at,resolved_at";
ConflictRow readConflict(const Statement& s) {
    ConflictRow c;
    c.id = s.columnInt(0);
    c.fileId = s.columnInt(1);
    c.localHash = s.columnText(2);
    c.remoteHash = s.columnText(3);
    c.localModifiedAt = s.columnInt(4);
    c.remoteModifiedAt = s.columnInt(5);
    c.copyPath = optText(s, 6).value_or("");
    c.resolution = optText(s, 7);
    c.detectedAt = s.columnInt(8);
    c.resolvedAt = optInt(s, 9);
    return c;
}
}  // namespace

Result<void> ConflictRepository::insert(ConflictRow& c) {
    auto r = run(db_,
                 "INSERT INTO conflicts(file_id,local_hash,remote_hash,local_modified_at,"
                 "remote_modified_at,conflict_copy_path,detected_at) VALUES(?1,?2,?3,?4,?5,?6,?7);",
                 {c.fileId, c.localHash, c.remoteHash, c.localModifiedAt, c.remoteModifiedAt,
                  c.copyPath, c.detectedAt});
    if (!r) return r;
    c.id = db_.lastInsertRowId();
    return {};
}

Result<std::optional<ConflictRow>> ConflictRepository::get(std::int64_t id) {
    std::optional<ConflictRow> out;
    auto r = query(db_, std::string("SELECT ") + kConflictCols + " FROM conflicts WHERE conflict_id=?1;",
                   {id}, [&](Statement& s) { out = readConflict(s); });
    if (!r) return r.error();
    return out;
}

Result<std::vector<ConflictRow>> ConflictRepository::list(bool includeResolved) {
    std::vector<ConflictRow> out;
    auto r = query(db_,
                   std::string("SELECT ") + kConflictCols + " FROM conflicts " +
                       (includeResolved ? "" : "WHERE resolution IS NULL ") +
                       "ORDER BY detected_at DESC;",
                   {}, [&](Statement& s) { out.push_back(readConflict(s)); });
    if (!r) return r.error();
    return out;
}

Result<void> ConflictRepository::resolve(std::int64_t id, const std::string& resolution,
                                         TimestampMs now) {
    return run(db_, "UPDATE conflicts SET resolution=?2,resolved_at=?3 WHERE conflict_id=?1;",
               {id, resolution, now});
}

// ================================================================ history
Result<void> HistoryRepository::add(const HistoryRow& h) {
    return run(db_,
               "INSERT INTO sync_history(device_id,file_path,operation,result,detail,timestamp) "
               "VALUES(?1,?2,?3,?4,?5,?6);",
               {h.deviceId, h.path, h.operation, h.result, h.detail, h.timestamp});
}

Result<std::vector<HistoryRow>> HistoryRepository::recent(int limit) {
    std::vector<HistoryRow> out;
    auto r = query(db_,
                   "SELECT event_id,device_id,file_path,operation,result,detail,timestamp "
                   "FROM sync_history ORDER BY event_id DESC LIMIT ?1;",
                   {limit}, [&](Statement& s) {
                       HistoryRow h;
                       h.id = s.columnInt(0);
                       h.deviceId = optText(s, 1);
                       h.path = optText(s, 2);
                       h.operation = s.columnText(3);
                       h.result = s.columnText(4);
                       h.detail = optText(s, 5).value_or("");
                       h.timestamp = s.columnInt(6);
                       out.push_back(std::move(h));
                   });
    if (!r) return r.error();
    return out;
}

Result<void> HistoryRepository::trim(int keep) {
    return run(db_,
               "DELETE FROM sync_history WHERE event_id <= "
               "(SELECT COALESCE(MAX(event_id),0) FROM sync_history) - ?1;",
               {keep});
}

// ================================================================ settings
Result<std::map<std::string, std::string>> SettingsRepository::all() {
    std::map<std::string, std::string> out;
    auto r = query(db_, "SELECT key,value FROM settings;", {},
                   [&](Statement& s) { out[s.columnText(0)] = s.columnText(1); });
    if (!r) return r.error();
    return out;
}

Result<void> SettingsRepository::set(const std::string& key, const std::string& value) {
    return run(db_,
               "INSERT INTO settings(key,value) VALUES(?1,?2) "
               "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
               {key, value});
}

}  // namespace lansync::storage
