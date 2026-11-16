#include "core/application/settings.hpp"

#include <algorithm>
#include <charconv>

#include "core/common/logging.hpp"

namespace lansync::app {

namespace {
template <typename T>
void readInt(const std::map<std::string, std::string>& m, const char* key, T& out) {
    const auto it = m.find(key);
    if (it == m.end()) return;
    long long v = 0;
    const auto& s = it->second;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc() && p == s.data() + s.size()) out = static_cast<T>(v);
}
void readBool(const std::map<std::string, std::string>& m, const char* key, bool& out) {
    const auto it = m.find(key);
    if (it != m.end()) out = (it->second == "1" || it->second == "true");
}
void readStr(const std::map<std::string, std::string>& m, const char* key, std::string& out) {
    const auto it = m.find(key);
    if (it != m.end()) out = it->second;
}
}  // namespace

void Settings::sanitize() {
    deviceName = deviceName.substr(0, 100);
    announceIntervalSec = std::clamp(announceIntervalSec, 1, 300);
    maxConcurrentTransfers = std::clamp(maxConcurrentTransfers, 1, 16);
    chunkSizeKb = std::clamp(chunkSizeKb, 16, 4096);
    maxFileSizeMb = std::clamp<std::int64_t>(maxFileSizeMb, 1, 16LL * 1024 * 1024);
    reconcileIntervalSec = std::clamp(reconcileIntervalSec, 5, 24 * 3600);
    versionMaxCount = std::clamp(versionMaxCount, 0, 1000);
    versionMaxAgeDays = std::clamp(versionMaxAgeDays, 0, 3650);
    spdlog::level::level_enum lvl{};
    if (!log::parseLevel(logLevel, lvl)) logLevel = "info";
}

Settings Settings::fromMap(const std::map<std::string, std::string>& m) {
    Settings s;
    readStr(m, "device_name", s.deviceName);
    readInt(m, "listen_port", s.listenPort);
    readBool(m, "allow_public_networks", s.allowPublicNetworks);
    readBool(m, "discovery_enabled", s.discoveryEnabled);
    readInt(m, "discovery_port", s.discoveryPort);
    readInt(m, "announce_interval_sec", s.announceIntervalSec);
    readInt(m, "max_concurrent_transfers", s.maxConcurrentTransfers);
    readInt(m, "chunk_size_kb", s.chunkSizeKb);
    readInt(m, "max_file_size_mb", s.maxFileSizeMb);
    readInt(m, "reconcile_interval_sec", s.reconcileIntervalSec);
    readInt(m, "version_max_count", s.versionMaxCount);
    readInt(m, "version_max_age_days", s.versionMaxAgeDays);
    readStr(m, "log_level", s.logLevel);
    s.sanitize();
    return s;
}

std::map<std::string, std::string> Settings::toMap() const {
    return {
        {"device_name", deviceName},
        {"listen_port", std::to_string(listenPort)},
        {"allow_public_networks", allowPublicNetworks ? "1" : "0"},
        {"discovery_enabled", discoveryEnabled ? "1" : "0"},
        {"discovery_port", std::to_string(discoveryPort)},
        {"announce_interval_sec", std::to_string(announceIntervalSec)},
        {"max_concurrent_transfers", std::to_string(maxConcurrentTransfers)},
        {"chunk_size_kb", std::to_string(chunkSizeKb)},
        {"max_file_size_mb", std::to_string(maxFileSizeMb)},
        {"reconcile_interval_sec", std::to_string(reconcileIntervalSec)},
        {"version_max_count", std::to_string(versionMaxCount)},
        {"version_max_age_days", std::to_string(versionMaxAgeDays)},
        {"log_level", logLevel},
    };
}

}  // namespace lansync::app
