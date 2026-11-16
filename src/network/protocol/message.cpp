#include "network/protocol/message.hpp"

#include <array>

#include "core/common/crypto.hpp"
#include "core/common/version.hpp"

namespace lansync::protocol {

using nlohmann::json;

bool isKnownType(std::string_view t) {
    static constexpr std::array<std::string_view, 15> kTypes = {
        type::kHello,       type::kPing,        type::kPong,      type::kError,
        type::kPairRequest, type::kPairCommit, type::kPairReveal, type::kPairDecision, type::kIndex,     type::kFileRequest,
        type::kFileBegin,   type::kFileEnd,     type::kFileError, type::kFileCancel,
        type::kFileDone};
    for (auto k : kTypes) {
        if (k == t) return true;
    }
    return false;
}

Message make(std::string_view type, std::string deviceId, json payload) {
    Message m;
    m.protocolVersion = kProtocolVersion;
    m.type = std::string(type);
    m.requestId = crypto::randomHex(8);
    m.deviceId = std::move(deviceId);
    m.payload = std::move(payload);
    return m;
}

std::string serialize(const Message& m) {
    json j;
    j["protocol_version"] = m.protocolVersion;
    j["message_type"] = m.type;
    j["request_id"] = m.requestId;
    j["device_id"] = m.deviceId;
    j["payload"] = m.payload;
    return j.dump();
}

Result<Message> parse(std::string_view text) {
    auto bad = [](std::string why) {
        return Error{ErrorCode::ProtocolError, std::move(why)};
    };
    if (text.size() > kMaxJsonBytes) return bad("message too large");

    json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return bad("not a JSON object");

    const auto ver = j.find("protocol_version");
    if (ver == j.end() || !ver->is_number_integer()) return bad("missing protocol_version");
    if (ver->get<long long>() != kProtocolVersion) {
        return Error{ErrorCode::UnsupportedVersion,
                     "unsupported protocol version " + std::to_string(ver->get<long long>())};
    }

    auto str = [&](const char* key, std::size_t maxLen, std::string& out) -> bool {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_string()) return false;
        out = it->get<std::string>();
        return out.size() <= maxLen;
    };

    Message m;
    m.protocolVersion = kProtocolVersion;
    if (!str("message_type", kMaxTypeLength, m.type) || !isKnownType(m.type)) {
        return bad("unknown or invalid message_type");
    }
    if (!str("request_id", kMaxIdLength, m.requestId)) return bad("invalid request_id");
    if (!str("device_id", kMaxIdLength, m.deviceId) || m.deviceId.empty() ||
        !crypto::isLowerHex(m.deviceId)) {
        return bad("invalid device_id");
    }
    const auto payload = j.find("payload");
    if (payload == j.end() || !payload->is_object()) return bad("payload must be an object");
    m.payload = std::move(*payload);
    return m;
}

}  // namespace lansync::protocol
