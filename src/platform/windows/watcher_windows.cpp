// NOTE: not compiled or tested. ReadDirectoryChangesW is not implemented yet;
// LanSync falls back to periodic reconciliation scans on Windows.
#include "filesystem/watcher/watcher.hpp"

namespace lansync::fsys {
std::unique_ptr<IWatcher> makeNativeWatcher() { return nullptr; }
}  // namespace lansync::fsys
