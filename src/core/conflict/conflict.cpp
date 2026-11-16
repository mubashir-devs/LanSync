#include "core/conflict/conflict.hpp"

#include <ctime>

namespace lansync::conflict {

bool remoteWins(std::int64_t lm, const std::string& ld, std::int64_t rm, const std::string& rd) {
    if (rm != lm) return rm > lm;
    return rd > ld;
}

std::string conflictCopyPath(const std::string& path, const std::string& loserId,
                             std::int64_t loserMtimeMs) {
    const auto slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);

    // Extension = from the last '.', unless the dot is the first character
    // (dotfiles such as ".bashrc" have no extension).
    const auto dot = name.find_last_of('.');
    const bool hasExt = dot != std::string::npos && dot > 0;
    const std::string stem = hasExt ? name.substr(0, dot) : name;
    const std::string ext = hasExt ? name.substr(dot) : "";

    const std::time_t secs = static_cast<std::time_t>(loserMtimeMs / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

    return dir + stem + ".sync-conflict-" + stamp + "-" + loserId.substr(0, 8) + ext;
}

}  // namespace lansync::conflict
