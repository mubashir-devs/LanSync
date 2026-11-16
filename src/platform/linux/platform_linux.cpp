#include "platform/platform.hpp"

#include <unistd.h>

#include <cstdlib>

namespace lansync::platform {

std::filesystem::path defaultDataDir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "lansync";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "lansync";
    }
    return std::filesystem::temp_directory_path() / "lansync";
}

std::string hostName() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') return buf;
    return "lansync-device";
}

}  // namespace lansync::platform
