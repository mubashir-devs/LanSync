#pragma once
// Native filesystem change notification.
//
// Events are HINTS: they can be duplicated, delayed, coalesced or missed (queue
// overflow, watch limits, network filesystems). The sync layer therefore treats
// them only as "something may have changed - scan soon" and always runs periodic
// reconciliation scans as the source of truth.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "core/common/result.hpp"

namespace lansync::fsys {

enum class WatchEventType {
    FileCreated,
    FileModified,
    FileDeleted,
    DirCreated,
    DirDeleted,
    Renamed,    // path = new, oldPath = old
    Overflow,   // events were lost: a full rescan is required
};

struct WatchEvent {
    WatchEventType type{WatchEventType::Overflow};
    std::string path;      // relative to the watched root, '/'-separated
    std::string oldPath;   // Renamed only
};

class IWatcher {
public:
    virtual ~IWatcher() = default;
    // `callback` is invoked on the watcher's own thread.
    virtual Result<void> start(const std::filesystem::path& root,
                               std::function<void(const WatchEvent&)> callback) = 0;
    virtual void stop() = 0;
};

// Native watcher for this platform, or nullptr when none is implemented (the
// caller then relies on periodic scans alone).
//   Linux: inotify.   macOS (FSEvents) / Windows (ReadDirectoryChangesW): not implemented.
std::unique_ptr<IWatcher> makeNativeWatcher();

}  // namespace lansync::fsys
