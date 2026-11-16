// NOTE: written but NOT compiled or tested - no macOS toolchain available yet.
#include "platform/platform.hpp"

#include <unistd.h>

#include <cstdlib>

namespace lansync::platform {

std::filesystem::path defaultDataDir() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "LanSync";
    }
    return std::filesystem::temp_directory_path() / "LanSync";
}

std::string hostName() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') return buf;
    return "lansync-device";
}

}  // namespace lansync::platform
