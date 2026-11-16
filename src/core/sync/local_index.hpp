#pragma once
// Reconciles scan results with the recorded index ("change detection").
//
// The `files` table is this device's view of a folder, including tombstones for
// deleted entries. Every real local change (new/modified/deleted content) bumps
// this device's counter in the entry's version vector; that is what peers
// compare to decide who is newer.

#include <string>
#include <vector>

#include "core/common/clock.hpp"
#include "filesystem/scanner/scanner.hpp"
#include "storage/repositories/repositories.hpp"

namespace lansync::sync {

struct ChangeSet {
    std::vector<std::string> created, modified, deleted;
    [[nodiscard]] bool empty() const { return created.empty() && modified.empty() && deleted.empty(); }
    [[nodiscard]] std::vector<std::string> all() const;
};

// Snapshot of recorded metadata, handed to the scanner thread.
fsys::KnownMap knownFromIndex(const std::map<std::string, storage::FileRow>& rows);

// Applies `scan` to the index inside one transaction.
Result<ChangeSet> applyScan(storage::Database& db, const std::string& folderId,
                            const std::string& localDeviceId, const fsys::ScanOutput& scan);

}  // namespace lansync::sync
