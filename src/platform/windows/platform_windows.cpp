// NOTE: written but NOT compiled or tested - no Windows toolchain available yet.
#include "platform/platform.hpp"

#include <cstdlib>

namespace lansync::platform {

std::filesystem::path defaultDataDir() {
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
        return std::filesystem::path(local) / "LanSync";
    }
    return std::filesystem::temp_directory_path() / "LanSync";
}

std::string hostName() {
    if (const char* name = std::getenv("COMPUTERNAME"); name && *name) return name;
    return "lansync-device";
}

}  // namespace lansync::platform

namespace lansync::platform {
// Not implemented on Windows yet: discovery falls back to the global broadcast
// address and the default multicast interface. (GetAdaptersAddresses would go here.)
std::vector<NetInterface> networkInterfaces() { return {}; }
}  // namespace lansync::platform
