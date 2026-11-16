#pragma once
// Walks a sync folder and produces file metadata, hashing only when needed.
//
// "Needed" means: no known record, or size/mtime differ from the known record.
// A file whose size and mtime match the record keeps its recorded hash (this is
// the metadata optimisation); `rehashAll` forces content verification.
//
// Runs on a worker thread; it touches only the filesystem and the immutable
// `known` snapshot it was given.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common/result.hpp"

namespace lansync::fsys {

struct KnownFile {
    std::int64_t size{0};
    std::int64_t mtimeMs{0};
    std::string hash;
};
using KnownMap = std::unordered_map<std::string, KnownFile>;

struct FileMeta {
    std::string path;            // relative, '/'-separated
    bool isDir{false};
    std::int64_t size{0};
    std::int64_t mtimeMs{0};
    std::string hash;            // empty for directories
    bool unstable{false};        // changed while being read; try again next scan
    bool rehashed{false};
};

struct ScanWarning {
    std::string path;
    std::string message;
};

struct ScanOutput {
    std::vector<FileMeta> entries;
    std::vector<ScanWarning> warnings;
    // Subtrees that could not be listed. Files below them must NOT be treated
    // as deleted just because they were not seen.
    std::vector<std::string> unreadablePrefixes;
    std::uint64_t bytesHashed{0};
};

struct ScanOptions {
    bool rehashAll{false};
    std::size_t hashBufferBytes{1 << 20};
};

// Fails (ErrorCode::NotFound/IoError) if the root is missing or unreadable -
// the caller must then change nothing, so an unmounted folder never looks like
// "everything was deleted". Symlinks and special files are skipped with a warning.
Result<ScanOutput> scanFolder(const std::filesystem::path& root, const KnownMap& known,
                              const ScanOptions& options = {},
                              const std::atomic<bool>* cancel = nullptr);

// Filesystem mtime in UTC milliseconds / setter (used after applying a remote file).
Result<std::int64_t> fileMtimeMs(const std::filesystem::path& p);
Result<void> setFileMtimeMs(const std::filesystem::path& p, std::int64_t ms);

}  // namespace lansync::fsys
