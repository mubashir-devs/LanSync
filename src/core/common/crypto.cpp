#include "core/common/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <fstream>

namespace lansync::crypto {

struct Sha256::Impl {
    EVP_MD_CTX* ctx{EVP_MD_CTX_new()};
    ~Impl() { EVP_MD_CTX_free(ctx); }
};

Sha256::Sha256() : impl_(std::make_unique<Impl>()) {
    EVP_DigestInit_ex(impl_->ctx, EVP_sha256(), nullptr);
}
Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::update(const void* data, std::size_t size) {
    EVP_DigestUpdate(impl_->ctx, data, size);
}

std::array<std::uint8_t, 32> Sha256::finish() {
    std::array<std::uint8_t, 32> out{};
    unsigned int len = 0;
    EVP_DigestFinal_ex(impl_->ctx, out.data(), &len);
    return out;
}

std::string Sha256::finishHex() {
    const auto digest = finish();
    return toHex(digest);
}

std::string toHex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kDigits[bytes[i] >> 4];
        out[2 * i + 1] = kDigits[bytes[i] & 0x0F];
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> fromHex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}

bool isLowerHex(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string sha256Hex(std::string_view data) {
    Sha256 h;
    h.update(data);
    return h.finishHex();
}

Result<std::uint64_t> hashFilePrefix(const std::filesystem::path& path, std::uint64_t length,
                                     Sha256& hasher, const std::atomic<bool>* cancel) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{ErrorCode::IoError, "cannot open '" + path.string() + "'"};
    std::vector<char> buf(1 << 20);
    std::uint64_t done = 0;
    while (done < length) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            return Error{ErrorCode::Cancelled, "hashing cancelled"};
        }
        const auto want = static_cast<std::streamsize>(
            std::min<std::uint64_t>(buf.size(), length - done));
        in.read(buf.data(), want);
        const auto got = in.gcount();
        if (got <= 0) break;
        hasher.update(buf.data(), static_cast<std::size_t>(got));
        done += static_cast<std::uint64_t>(got);
    }
    if (in.bad()) return Error{ErrorCode::IoError, "read error on '" + path.string() + "'"};
    return done;
}

Result<std::string> hashFile(const std::filesystem::path& path, std::size_t bufferSize,
                             const std::atomic<bool>* cancel) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{ErrorCode::IoError, "cannot open '" + path.string() + "'"};
    Sha256 hasher;
    std::vector<char> buf(std::max<std::size_t>(bufferSize, 4096));
    while (in) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            return Error{ErrorCode::Cancelled, "hashing cancelled"};
        }
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = in.gcount();
        if (got > 0) hasher.update(buf.data(), static_cast<std::size_t>(got));
    }
    if (in.bad()) return Error{ErrorCode::IoError, "read error on '" + path.string() + "'"};
    return hasher.finishHex();
}

std::string randomHex(std::size_t bytes) {
    std::vector<std::uint8_t> buf(bytes);
    if (bytes > 0 && RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) {
        // RAND_bytes failing means the system CSPRNG is unusable; never fall back.
        throw std::runtime_error("OpenSSL RAND_bytes failed");
    }
    return toHex(buf);
}

}  // namespace lansync::crypto
