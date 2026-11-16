#pragma once
// Explicit message framing over a byte stream (TCP/TLS).
//
//   +----------------+-----------+------------------+
//   | length (u32 BE)| kind (u8) | payload          |
//   +----------------+-----------+------------------+
//   length = 1 + payload size. kind 0 = JSON message, kind 1 = file chunk
//   (payload = u32 BE stream id followed by raw bytes).
//
// A single read never corresponds to one frame; FrameDecoder reassembles frames
// from arbitrary fragments and rejects oversized frames from the header alone,
// before buffering their body.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/common/result.hpp"

namespace lansync::protocol {

enum class FrameKind : std::uint8_t { Json = 0, Chunk = 1 };

struct Frame {
    FrameKind kind{FrameKind::Json};
    std::string payload;
};

struct ChunkView {
    std::uint32_t streamId{0};
    std::string_view data;
};

std::string encodeFrame(FrameKind kind, std::string_view payload);
std::string encodeJsonFrame(std::string_view json);
std::string encodeChunkFrame(std::uint32_t streamId, std::string_view data);

// Valid only for Chunk frames; the view aliases `frame.payload`.
Result<ChunkView> parseChunk(const Frame& frame);

class FrameDecoder {
public:
    // maxFrameBytes bounds `length` (kind byte + payload).
    explicit FrameDecoder(std::size_t maxFrameBytes) : maxFrame_(maxFrameBytes) {}

    void feed(const char* data, std::size_t size);

    // Returns the next complete frame, nullopt if more bytes are needed, or an
    // error (sticky: once failed the stream must be closed).
    Result<std::optional<Frame>> next();

    [[nodiscard]] std::size_t bufferedBytes() const { return buffer_.size() - offset_; }

private:
    std::size_t maxFrame_;
    std::string buffer_;
    std::size_t offset_{0};
    bool failed_{false};
};

}  // namespace lansync::protocol
