#pragma once
// Application-level protocol messages (JSON envelope).
//
//   { "protocol_version": 1, "message_type": "...", "request_id": "...",
//     "device_id": "<hex>", "payload": { ... } }
//
// All fields are validated on parse; nothing from the network is trusted.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/common/result.hpp"

namespace lansync::protocol {

namespace type {
inline constexpr std::string_view kHello = "hello";
inline constexpr std::string_view kPing = "ping";
inline constexpr std::string_view kPong = "pong";
inline constexpr std::string_view kError = "error";
inline constexpr std::string_view kPairRequest = "pair_request";
inline constexpr std::string_view kPairCommit = "pair_commit";
inline constexpr std::string_view kPairReveal = "pair_reveal";
inline constexpr std::string_view kPairDecision = "pair_decision";
inline constexpr std::string_view kIndex = "index";
inline constexpr std::string_view kFileRequest = "file_request";
inline constexpr std::string_view kFileBegin = "file_begin";
inline constexpr std::string_view kFileEnd = "file_end";
inline constexpr std::string_view kFileError = "file_error";
inline constexpr std::string_view kFileCancel = "file_cancel";
inline constexpr std::string_view kFileDone = "file_done";
}  // namespace type

// Hard limits on untrusted input.
inline constexpr std::size_t kMaxJsonBytes = 1 * 1024 * 1024;
inline constexpr std::size_t kMaxIdLength = 64;
inline constexpr std::size_t kMaxTypeLength = 32;

struct Message {
    int protocolVersion{0};   // set to kProtocolVersion by make()
    std::string type;
    std::string requestId;
    std::string deviceId;
    nlohmann::json payload = nlohmann::json::object();
};

bool isKnownType(std::string_view t);

// Builds an outgoing message with the current protocol version and a fresh
// request id.
Message make(std::string_view type, std::string deviceId,
             nlohmann::json payload = nlohmann::json::object());

std::string serialize(const Message& m);

// Fails with UnsupportedVersion for a different protocol version and with
// ProtocolError for anything malformed or oversized.
Result<Message> parse(std::string_view json);

}  // namespace lansync::protocol
