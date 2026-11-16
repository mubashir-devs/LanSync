#include "core/versioning/version_store.hpp"

#include "core/common/crypto.hpp"
#include "core/common/logging.hpp"

namespace lansync::versioning {

namespace fs = std::filesystem;

VersionStore::VersionStore(fs::path root, storage::Database& db, std::shared_ptr<const IClock> clock)
    : root_(std::move(root)), db_(db), clock_(std::move(clock)) {}

Result<storage::VersionRow> VersionStore::archive(const storage::FileRow& row, const fs::path& file,
                                                  bool consume) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return Error{ErrorCode::NotFound, "nothing to archive at '" + file.string() + "'"};
    }

    storage::VersionRow v;
    v.fileId = row.fileId;
    v.hash = row.hash;
    v.size = row.size;
    v.modifiedAt = row.modifiedAt;
    v.createdAt = clock_->nowMs();
    v.storageLocation = row.folderId + "/" + std::to_string(v.createdAt) + "-" +
                        crypto::randomHex(4) + "-" + row.hash.substr(0, 16);

    const fs::path dest = root_ / v.storageLocation;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create versions dir: " + ec.message()};

    bool moved = false;
    if (consume) {
        fs::rename(file, dest, ec);
        moved = !ec;
    }
    if (!moved) {
        fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) return Error{ErrorCode::IoError, "cannot archive '" + file.string() + "': " + ec.message()};
        if (consume) {
            fs::remove(file, ec);
            if (ec) {
                fs::remove(dest, ec);   // keep the original authoritative; drop the copy
                return Error{ErrorCode::IoError, "archived copy made but original could not be removed"};
            }
        }
    }

    if (auto r = storage::VersionRepository(db_).insert(v); !r) {
        // Row failed: put the content back if we moved it, never lose data.
        if (consume) fs::rename(dest, file, ec);
        else fs::remove(dest, ec);
        return r.error();
    }
    return v;
}

Result<void> VersionStore::copyOut(const storage::VersionRow& v, const fs::path& dest) {
    const fs::path src = locate(v);
    auto h = crypto::hashFile(src);
    if (!h) return Error{ErrorCode::IoError, "stored version is unreadable: " + h.error().message};
    if (*h != v.hash) return Error{ErrorCode::SecurityError, "stored version failed integrity check"};
    std::error_code ec;
    fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot restore version: " + ec.message()};
    return {};
}

Result<int> VersionStore::prune(const RetentionPolicy& p) {
    if (p.maxVersionsPerFile <= 0 && p.maxAgeDays <= 0) return 0;   // keep everything
    auto all = storage::VersionRepository(db_).listAll();   // grouped by file, newest first
    if (!all) return all.error();

    const TimestampMs cutoff = p.maxAgeDays > 0
        ? clock_->nowMs() - static_cast<TimestampMs>(p.maxAgeDays) * 24 * 3600 * 1000
        : -1;
    int removed = 0;
    std::int64_t currentFile = -1;
    int indexInFile = 0;
    storage::VersionRepository repo(db_);
    for (const auto& v : *all) {
        if (v.fileId != currentFile) {
            currentFile = v.fileId;
            indexInFile = 0;
        }
        const bool tooMany = p.maxVersionsPerFile > 0 && indexInFile >= p.maxVersionsPerFile;
        const bool tooOld = cutoff >= 0 && v.createdAt < cutoff;
        ++indexInFile;
        if (!tooMany && !tooOld) continue;
        std::error_code ec;
        fs::remove(locate(v), ec);
        if (ec) {
            LS_WARN("cannot delete old version {}: {}", v.versionId, ec.message());
            continue;
        }
        if (repo.remove(v.versionId)) ++removed;
    }
    return removed;
}

void VersionStore::purgeFolder(const std::string& folderId) {
    std::error_code ec;
    fs::remove_all(root_ / folderId, ec);
}

}  // namespace lansync::versioning
