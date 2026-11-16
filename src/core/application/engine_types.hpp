#pragma once
// Plain data types exchanged between the Engine and its front-ends (CLI, Qt).
// No behaviour, no Qt: front-ends convert these as they see fit.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/application/settings.hpp"
#include "core/common/clock.hpp"

namespace lansync::app {

struct DeviceInfo {
    std::string id;
    std::string name;
    std::string address;
    int port{0};
    std::string pairingStatus;      // unpaired|pending|paired|revoked
    bool discovered{false};         // currently visible on the LAN
    bool connected{false};          // authenticated session is up
    TimestampMs lastSeenMs{0};
    std::string fingerprint;
    std::string appVersion;
};

struct FolderInfo {
    std::string id;
    std::string name;
    std::string path;
    std::string remoteDeviceId;
    std::string remoteDeviceName;
    std::string direction;
    bool enabled{true};
    bool paused{false};
    std::string state;              // Idle|Scanning|Comparing|Queued|Transferring|...
    std::int64_t fileCount{0};
    std::int64_t pendingCount{0};
    std::int64_t conflictCount{0};
    TimestampMs lastSyncMs{0};
    std::string error;
};

struct TransferInfo {
    std::string id;
    std::string folderId;
    std::string folderName;
    std::string path;
    std::string direction;          // download|upload
    std::string peerId;
    std::string peerName;
    std::string status;             // queued|transferring|verifying|completed|failed|cancelled|paused
    std::int64_t bytes{0};
    std::int64_t total{0};
    double speedBps{0};
    std::int64_t etaSec{-1};
    std::string error;
    TimestampMs startedMs{0};
};

struct ConflictInfo {
    std::int64_t id{0};
    std::string folderId;
    std::string folderName;
    std::string path;
    std::string copyPath;
    std::string localHash;
    std::string remoteHash;
    TimestampMs localModifiedMs{0};
    TimestampMs remoteModifiedMs{0};
    TimestampMs detectedMs{0};
    std::string resolution;         // empty while open
};

struct HistoryInfo {
    std::int64_t id{0};
    TimestampMs timestampMs{0};
    std::string deviceName;
    std::string path;
    std::string operation;
    std::string result;
    std::string detail;
};

struct VersionInfo {
    std::int64_t id{0};
    std::string folderId;
    std::string path;
    std::string hash;
    std::int64_t size{0};
    TimestampMs modifiedMs{0};
    TimestampMs createdMs{0};
};

struct Stats {
    int pairedDevices{0};
    int activeFolders{0};
    std::int64_t filesSynced{0};
    int pendingTransfers{0};
    int activeTransfers{0};
    int openConflicts{0};
};

struct Snapshot {
    std::vector<DeviceInfo> devices;
    std::vector<FolderInfo> folders;
    std::vector<TransferInfo> transfers;
    std::vector<ConflictInfo> conflicts;
    std::vector<HistoryInfo> history;
    Stats stats;
    Settings settings;
    std::string deviceId;
    std::string deviceName;
    std::uint16_t listenPort{0};
};

struct PairingPrompt {
    std::string deviceId;
    std::string deviceName;
    std::string code;               // 6-digit string both users must compare
    bool incoming{false};
};

struct EngineEvent {
    enum class Kind {
        DevicesChanged,
        FoldersChanged,
        TransfersChanged,
        ConflictsChanged,
        HistoryChanged,
        PairingRequested,
        PairingFinished,     // text = outcome
        Notification,
    };
    Kind kind{Kind::DevicesChanged};
    std::string title;
    std::string text;
    bool isError{false};
    PairingPrompt pairing;
};

using EventHandler = std::function<void(const EngineEvent&)>;

}  // namespace lansync::app
