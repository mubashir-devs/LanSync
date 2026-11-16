#pragma once
// Platform abstraction. Business logic calls only these functions; the OS
// specific implementation is chosen at build time (see src/CMakeLists.txt).
// Later phases add: file watcher factory, network interface enumeration,
// notifications.

#include <filesystem>
#include <string>
#include <vector>

namespace lansync::platform {

// Per-user directory for the database, certificates and logs.
//   Linux:   $XDG_DATA_HOME/lansync or ~/.local/share/lansync
//   macOS:   ~/Library/Application Support/LanSync
//   Windows: %LOCALAPPDATA%\LanSync
std::filesystem::path defaultDataDir();

// Human-readable name for this machine, used as the default device name.
std::string hostName();

struct NetInterface {
    std::string name;
    std::string ipv4;            // dotted quad
    std::string broadcast;       // empty if none
    bool loopback{false};
    bool multicast{false};
};

// Active IPv4 interfaces (up, with an address). Empty if enumeration is not
// supported on this platform; callers must then fall back to defaults.
std::vector<NetInterface> networkInterfaces();

}  // namespace lansync::platform
