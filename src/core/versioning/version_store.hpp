#pragma once
// Keeps replaced/deleted file contents so nothing is lost silently.
//
// Layout: <versionsRoot>/<folderId>/<createdMs>-<rand>-<hash16>. Rows in
// `file_versions` describe each stored blob. Pruning happens ONLY through
// prune() according to an explicit RetentionPolicy; nothing else deletes history.

#include <filesystem>
#include <memory>

#include "core/common/clock.hpp"
#include "storage/repositories/repositories.hpp"

namespace lansync::versioning {

struct RetentionPolicy {
    int maxVersionsPerFile{10};   // 0 = no count limit
    int maxAgeDays{30};           // 0 = no age limit
    // With both 0 nothing is ever deleted.
};

class VersionStore {
public:
    VersionStore(std::filesystem::path root, storage::Database& db,
                 std::shared_ptr<const IClock> clock);

    // Preserves `file`'s current content as a version of `row`. With
    // consume=true the file is moved (falls back to copy+delete across
    // filesystems) because the caller is about to replace/delete it.
    Result<storage::VersionRow> archive(const storage::FileRow& row,
                                        const std::filesystem::path& file, bool consume);

    // Copies a stored version to `dest` (overwriting) after verifying its hash.
    Result<void> copyOut(const storage::VersionRow& v, const std::filesystem::path& dest);

    // Applies the policy; returns how many versions were removed.
    Result<int> prune(const RetentionPolicy& policy);

    // Removes everything stored for a folder (called when the folder is removed).
    void purgeFolder(const std::string& folderId);

    [[nodiscard]] std::filesystem::path locate(const storage::VersionRow& v) const {
        return root_ / v.storageLocation;
    }

private:
    std::filesystem::path root_;
    storage::Database& db_;
    std::shared_ptr<const IClock> clock_;
};

}  // namespace lansync::versioning
