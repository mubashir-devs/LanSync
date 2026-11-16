#pragma once
// User-configurable settings, persisted in the `settings` table.

#include <cstdint>
#include <map>
#include <string>

namespace lansync::app {

struct Settings {
    // Network
    std::string deviceName;                 // empty = hostname
    std::uint16_t listenPort{45455};        // TCP; 0 = ephemeral
    bool allowPublicNetworks{false};        // accept non-private peers (not recommended)
    // Discovery
    bool discoveryEnabled{true};
    std::uint16_t discoveryPort{45454};     // UDP
    int announceIntervalSec{5};
    // Transfers
    int maxConcurrentTransfers{3};
    int chunkSizeKb{256};
    std::int64_t maxFileSizeMb{1024 * 1024};   // 1 TiB
    // Sync
    int reconcileIntervalSec{60};
    // Versioning
    int versionMaxCount{10};      // per file; 0 = unlimited by count
    int versionMaxAgeDays{30};    // 0 = unlimited by age
    // Logging
    std::string logLevel{"info"};

    // Clamps every value into its supported range (defence against bad input).
    void sanitize();

    static Settings fromMap(const std::map<std::string, std::string>& m);
    [[nodiscard]] std::map<std::string, std::string> toMap() const;

    [[nodiscard]] std::size_t chunkSizeBytes() const {
        return static_cast<std::size_t>(chunkSizeKb) * 1024;
    }
};

}  // namespace lansync::app
