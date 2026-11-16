#include "filesystem/scanner/scanner.hpp"

#include <algorithm>
#include <chrono>

#include "core/common/crypto.hpp"
#include "core/common/path_safety.hpp"

namespace lansync::fsys {

namespace fs = std::filesystem;

namespace {
std::int64_t toMs(fs::file_time_type t) {
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}
}  // namespace

Result<std::int64_t> fileMtimeMs(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot stat '" + p.string() + "': " + ec.message()};
    return toMs(t);
}

Result<void> setFileMtimeMs(const fs::path& p, std::int64_t ms) {
    std::error_code ec;
    const auto sys = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
    fs::last_write_time(p, std::chrono::clock_cast<fs::file_time_type::clock>(sys), ec);
    if (ec) return Error{ErrorCode::IoError, "cannot set mtime on '" + p.string() + "': " + ec.message()};
    return {};
}

Result<ScanOutput> scanFolder(const fs::path& root, const KnownMap& known,
                              const ScanOptions& options, const std::atomic<bool>* cancel) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return Error{(!ec || ec == std::errc::no_such_file_or_directory) ? ErrorCode::NotFound
                                                                      : ErrorCode::IoError,
                     "sync folder is missing or not a directory: " + root.string()};
    }
    // Make sure we can list it at all.
    {
        fs::directory_iterator probe(root, ec);
        if (ec) return Error{ErrorCode::IoError, "cannot read sync folder: " + ec.message()};
    }

    ScanOutput out;
    std::vector<fs::path> stack{root};

    while (!stack.empty()) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            return Error{ErrorCode::Cancelled, "scan cancelled"};
        }
        const fs::path dir = stack.back();
        stack.pop_back();

        std::error_code iec;
        fs::directory_iterator it(dir, fs::directory_options::none, iec);
        if (iec) {
            const auto rel = pathsafety::toRelative(root, dir);
            out.warnings.push_back({rel, "cannot list directory: " + iec.message()});
            out.unreadablePrefixes.push_back(rel);
            continue;
        }
        for (const fs::directory_iterator end; it != end; it.increment(iec)) {
            if (iec) {
                out.warnings.push_back({pathsafety::toRelative(root, dir), "directory read error: " + iec.message()});
                out.unreadablePrefixes.push_back(pathsafety::toRelative(root, dir));
                break;
            }
            const fs::path& p = it->path();
            const std::string rel = pathsafety::toRelative(root, p);
            if (pathsafety::isInternalPath(rel)) continue;
            if (!pathsafety::validateRelativePath(rel)) {
                out.warnings.push_back({rel, "skipped: name not syncable"});
                continue;
            }

            std::error_code sec;
            const auto st = it->symlink_status(sec);
            if (sec) {
                out.warnings.push_back({rel, "cannot stat: " + sec.message()});
                continue;
            }
            if (fs::is_symlink(st)) {
                out.warnings.push_back({rel, "skipped: symlink"});
                continue;
            }
            if (fs::is_directory(st)) {
                const auto mt = fileMtimeMs(p);
                out.entries.push_back({rel, true, 0, mt ? *mt : 0, {}, false, false});
                stack.push_back(p);
                continue;
            }
            if (!fs::is_regular_file(st)) {
                out.warnings.push_back({rel, "skipped: not a regular file"});
                continue;
            }

            FileMeta meta;
            meta.path = rel;
            std::error_code fec;
            const auto size = fs::file_size(p, fec);
            const auto mt = fileMtimeMs(p);
            if (fec || !mt) {
                out.warnings.push_back({rel, "cannot stat file"});
                out.entries.push_back({rel, false, 0, 0, {}, /*unstable=*/true, false});
                continue;
            }
            meta.size = static_cast<std::int64_t>(size);
            meta.mtimeMs = *mt;

            const auto k = known.find(rel);
            if (!options.rehashAll && k != known.end() && !k->second.hash.empty() &&
                k->second.size == meta.size && k->second.mtimeMs == meta.mtimeMs) {
                meta.hash = k->second.hash;   // metadata unchanged: trust recorded hash
            } else {
                auto h = crypto::hashFile(p, options.hashBufferBytes, cancel);
                if (!h) {
                    if (h.error().code == ErrorCode::Cancelled) return h.error();
                    out.warnings.push_back({rel, "cannot read: " + h.error().message});
                    meta.unstable = true;   // keep the previous record; do not delete
                } else {
                    // Did it change while we were hashing?
                    std::error_code e2;
                    const auto size2 = fs::file_size(p, e2);
                    const auto mt2 = fileMtimeMs(p);
                    if (e2 || !mt2 || static_cast<std::int64_t>(size2) != meta.size || *mt2 != meta.mtimeMs) {
                        meta.unstable = true;
                    } else {
                        meta.hash = std::move(*h);
                        meta.rehashed = true;
                        out.bytesHashed += static_cast<std::uint64_t>(meta.size);
                    }
                }
            }
            out.entries.push_back(std::move(meta));
        }
    }
    std::sort(out.entries.begin(), out.entries.end(),
              [](const FileMeta& a, const FileMeta& b) { return a.path < b.path; });
    return out;
}

}  // namespace lansync::fsys
