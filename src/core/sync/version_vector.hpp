#pragma once
// Version vectors distinguish "remote is strictly newer" from "edited
// independently on both sides" (a conflict), which timestamps cannot do safely.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/common/result.hpp"

namespace lansync::sync {

class VersionVector {
public:
    enum class Order {
        Equal,
        Greater,      // *this strictly dominates `other`
        Less,         // `other` strictly dominates *this
        Concurrent,
    };

    static constexpr std::size_t kMaxEntries = 64;

    // Strict parser for untrusted input: object of hex-device-id -> non-negative int.
    static Result<VersionVector> fromJson(const nlohmann::json& j);
    static VersionVector fromString(std::string_view s);   // lenient: invalid -> empty

    [[nodiscard]] nlohmann::json toJson() const;
    [[nodiscard]] std::string toString() const { return toJson().dump(); }

    void bump(const std::string& deviceId) { ++counters_[deviceId]; }
    void merge(const VersionVector& other);   // component-wise max
    [[nodiscard]] std::uint64_t get(const std::string& deviceId) const;
    [[nodiscard]] Order compare(const VersionVector& other) const;
    [[nodiscard]] bool empty() const { return counters_.empty(); }
    bool operator==(const VersionVector&) const = default;

private:
    std::map<std::string, std::uint64_t> counters_;
};

}  // namespace lansync::sync
