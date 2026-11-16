#pragma once
// Deterministic conflict handling.
//
// When two devices edit the same file independently, BOTH versions are kept:
// one keeps the original name, the other becomes a "conflict copy". The choice
// is a pure function of data both sides see, so they agree without talking:
//
//   winner = the larger (mtime, device id) pair keeps the original name.
//   loser's content is preserved as   <stem>.sync-conflict-<UTC yyyymmdd-hhmmss>-<id8><ext>
//
// The user can later choose which content to keep (resolve), nothing is lost
// meanwhile.

#include <cstdint>
#include <string>

namespace lansync::conflict {

// True if the REMOTE version should keep the original name.
bool remoteWins(std::int64_t localMtimeMs, const std::string& localDeviceId,
                std::int64_t remoteMtimeMs, const std::string& remoteDeviceId);

// Name for the preserved copy of the loser's version of `path`.
std::string conflictCopyPath(const std::string& path, const std::string& loserDeviceId,
                             std::int64_t loserMtimeMs);

}  // namespace lansync::conflict
