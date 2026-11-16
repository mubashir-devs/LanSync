#pragma once
// Hashing and randomness built on OpenSSL (EVP). No custom cryptography.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/common/result.hpp"

namespace lansync::crypto {

// Incremental SHA-256.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const void* data, std::size_t size);
    void update(std::string_view data) { update(data.data(), data.size()); }
    // Finishes the hash; the object must not be updated afterwards.
    std::array<std::uint8_t, 32> finish();
    std::string finishHex();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string toHex(std::span<const std::uint8_t> bytes);
std::optional<std::vector<std::uint8_t>> fromHex(std::string_view hex);
bool isLowerHex(std::string_view s);

std::string sha256Hex(std::string_view data);

// Streams the file through SHA-256 in `bufferSize` blocks (never loads it whole).
// `cancel`, if set, aborts with ErrorCode::Cancelled.
Result<std::string> hashFile(const std::filesystem::path& path,
                             std::size_t bufferSize = 1 << 20,
                             const std::atomic<bool>* cancel = nullptr);

// Hash of the first `length` bytes of a file; used to resume partial transfers.
// Feeds `hasher` and returns the number of bytes consumed (<= length).
Result<std::uint64_t> hashFilePrefix(const std::filesystem::path& path, std::uint64_t length,
                                     Sha256& hasher, const std::atomic<bool>* cancel = nullptr);

// Cryptographically secure random bytes, hex encoded (2 chars per byte).
std::string randomHex(std::size_t bytes);

}  // namespace lansync::crypto
