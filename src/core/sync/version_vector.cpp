#include "core/sync/version_vector.hpp"

#include <algorithm>

#include "core/common/crypto.hpp"

namespace lansync::sync {

Result<VersionVector> VersionVector::fromJson(const nlohmann::json& j) {
    if (!j.is_object() || j.size() > kMaxEntries) {
        return Error{ErrorCode::ProtocolError, "invalid version vector"};
    }
    VersionVector v;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key().empty() || it.key().size() > 64 || !crypto::isLowerHex(it.key()) ||
            !it.value().is_number_unsigned()) {
            return Error{ErrorCode::ProtocolError, "invalid version vector entry"};
        }
        v.counters_[it.key()] = it.value().get<std::uint64_t>();
    }
    return v;
}

VersionVector VersionVector::fromString(std::string_view s) {
    auto j = nlohmann::json::parse(s.begin(), s.end(), nullptr, false);
    if (j.is_discarded()) return {};
    auto v = fromJson(j);
    return v ? *v : VersionVector{};
}

nlohmann::json VersionVector::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : counters_) j[k] = v;
    return j;
}

void VersionVector::merge(const VersionVector& o) {
    for (const auto& [k, v] : o.counters_) {
        auto& mine = counters_[k];
        mine = std::max(mine, v);
    }
}

std::uint64_t VersionVector::get(const std::string& id) const {
    const auto it = counters_.find(id);
    return it == counters_.end() ? 0 : it->second;
}

VersionVector::Order VersionVector::compare(const VersionVector& o) const {
    bool greater = false, less = false;
    auto check = [&](const std::string& key) {
        const auto a = get(key), b = o.get(key);
        if (a > b) greater = true;
        if (a < b) less = true;
    };
    for (const auto& [k, v] : counters_) check(k);
    for (const auto& [k, v] : o.counters_) check(k);
    if (greater && less) return Order::Concurrent;
    if (greater) return Order::Greater;
    if (less) return Order::Less;
    return Order::Equal;
}

}  // namespace lansync::sync
