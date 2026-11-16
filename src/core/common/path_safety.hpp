#pragma once
// Strict validation of paths received from peers (untrusted input) and
// containment checks against a sync root.

#include <filesystem>
#include <string>
#include <string_view>

#include "core/common/result.hpp"

namespace lansync::pathsafety {

inline constexpr std::string_view kInternalDir = ".lansync";   // tmp files, versions, etc.
inline constexpr std::size_t kMaxPathBytes = 4096;
inline constexpr std::size_t kMaxComponentBytes = 255;

// Accepts only canonical relative paths: '/'-separated, no empty/"."/".."
// components, no leading '/', no drive prefix, no backslashes, no control
// characters or NUL, bounded lengths, and nothing inside the internal dir.
Result<std::string> validateRelativePath(std::string_view rel);

// True for ".lansync" or anything below it.
bool isInternalPath(std::string_view rel);

// Validates `rel`, then returns root/rel provided it stays inside `root` after
// resolving symlinks in every EXISTING component. A symlinked ancestor is
// rejected (we never follow links out of a sync folder).
Result<std::filesystem::path> resolveInside(const std::filesystem::path& root,
                                            std::string_view rel);

// '/'-separated path of `absolute` relative to `root` (no validation).
std::string toRelative(const std::filesystem::path& root, const std::filesystem::path& absolute);

}  // namespace lansync::pathsafety
