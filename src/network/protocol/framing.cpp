#include "network/protocol/framing.hpp"

#include <cstring>

namespace lansync::protocol {

namespace {
void putU32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}
std::uint32_t getU32(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (static_cast<std::uint32_t>(u[0]) << 24) | (static_cast<std::uint32_t>(u[1]) << 16) |
           (static_cast<std::uint32_t>(u[2]) << 8) | static_cast<std::uint32_t>(u[3]);
}
}  // namespace

std::string encodeFrame(FrameKind kind, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 5);
    putU32(out, static_cast<std::uint32_t>(payload.size() + 1));
    out.push_back(static_cast<char>(kind));
    out.append(payload);
    return out;
}

std::string encodeJsonFrame(std::string_view json) { return encodeFrame(FrameKind::Json, json); }

std::string encodeChunkFrame(std::uint32_t streamId, std::string_view data) {
    std::string out;
    out.reserve(data.size() + 9);
    putU32(out, static_cast<std::uint32_t>(data.size() + 5));
    out.push_back(static_cast<char>(FrameKind::Chunk));
    putU32(out, streamId);
    out.append(data);
    return out;
}

Result<ChunkView> parseChunk(const Frame& frame) {
    if (frame.kind != FrameKind::Chunk || frame.payload.size() < 4) {
        return Error{ErrorCode::ProtocolError, "malformed chunk frame"};
    }
    return ChunkView{getU32(frame.payload.data()),
                     std::string_view(frame.payload).substr(4)};
}

void FrameDecoder::feed(const char* data, std::size_t size) {
    if (failed_) return;
    // Compact consumed bytes occasionally so the buffer cannot grow unbounded.
    if (offset_ > 0 && offset_ >= buffer_.size() / 2) {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
    buffer_.append(data, size);
}

Result<std::optional<Frame>> FrameDecoder::next() {
    if (failed_) return Error{ErrorCode::ProtocolError, "decoder in failed state"};
    const std::size_t avail = buffer_.size() - offset_;
    if (avail < 4) return std::optional<Frame>{};

    const std::uint32_t len = getU32(buffer_.data() + offset_);
    if (len == 0 || len > maxFrame_) {
        failed_ = true;
        return Error{ErrorCode::ProtocolError,
                     "invalid frame length " + std::to_string(len) + " (max " +
                         std::to_string(maxFrame_) + ")"};
    }
    if (avail < 4 + static_cast<std::size_t>(len)) return std::optional<Frame>{};

    const auto kindByte = static_cast<std::uint8_t>(buffer_[offset_ + 4]);
    if (kindByte > static_cast<std::uint8_t>(FrameKind::Chunk)) {
        failed_ = true;
        return Error{ErrorCode::ProtocolError, "unknown frame kind"};
    }
    Frame f;
    f.kind = static_cast<FrameKind>(kindByte);
    f.payload.assign(buffer_, offset_ + 5, len - 1);
    offset_ += 4 + static_cast<std::size_t>(len);
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
    return std::optional<Frame>{std::move(f)};
}

}  // namespace lansync::protocol
