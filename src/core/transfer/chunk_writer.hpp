#pragma once
// Receives a file into a private part file, hashing as it goes.
//
// * The destination is never touched until finish() has verified size AND
//   SHA-256; the caller then renames the part file into place atomically.
// * A part file is only ever resumed if its existing prefix is re-hashed here,
//   so resumed transfers are verified end-to-end exactly like fresh ones.
// * Memory use is O(chunk): nothing is buffered beyond the current write.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include "core/common/crypto.hpp"
#include "core/common/result.hpp"

namespace lansync::transfer {

class ChunkWriter {
public:
    // Truncates/creates the part file.
    static Result<std::unique_ptr<ChunkWriter>> createFresh(const std::filesystem::path& part,
                                                            std::uint64_t expectedSize,
                                                            std::string expectedHash);

    // Continues an existing part file after re-hashing its prefix (BLOCKING:
    // run on a worker thread for big files). If the part is unusable (larger
    // than expected) it is discarded and a fresh writer is returned.
    static Result<std::unique_ptr<ChunkWriter>> resume(const std::filesystem::path& part,
                                                       std::uint64_t expectedSize,
                                                       std::string expectedHash);

    ~ChunkWriter();

    Result<void> write(std::string_view data);

    [[nodiscard]] std::uint64_t received() const { return received_; }
    [[nodiscard]] std::uint64_t expectedSize() const { return expectedSize_; }
    [[nodiscard]] const std::filesystem::path& partPath() const { return part_; }
    [[nodiscard]] bool complete() const { return received_ == expectedSize_; }

    // Verifies size and hash and closes the file. On mismatch the part file is
    // deleted (corrupt data must never be resumed) and an error returned.
    Result<void> finish();

    // Stops writing; keeps the part file for a later resume unless asked not to.
    void abort(bool deletePart);

private:
    ChunkWriter(std::filesystem::path part, std::uint64_t expectedSize, std::string expectedHash)
        : part_(std::move(part)), expectedSize_(expectedSize), expectedHash_(std::move(expectedHash)) {}

    std::filesystem::path part_;
    std::uint64_t expectedSize_;
    std::string expectedHash_;
    std::uint64_t received_{0};
    crypto::Sha256 hasher_;
    std::ofstream out_;
    bool closed_{false};
};

}  // namespace lansync::transfer
