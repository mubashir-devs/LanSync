#pragma once
// Owns every peer session and decides who is trusted.
//
// Trust rule (the only way a session becomes `trusted`):
//   the peer's certificate-derived device id is a *paired* device in the DB AND
//   its certificate fingerprint equals the one pinned at pairing time.
// Discovery results never grant trust; they only say where a peer might be.

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/application/engine_types.hpp"
#include "core/common/clock.hpp"
#include "core/device/pairing_state.hpp"
#include "core/device/peer_session.hpp"
#include "network/discovery/discovery.hpp"
#include "network/security/identity.hpp"
#include "storage/repositories/repositories.hpp"

namespace lansync::device {

using app::DeviceInfo;
using app::EngineEvent;
using app::EventHandler;

// Receives traffic from trusted peers (implemented by the sync layer).
class IPeerSink {
public:
    virtual ~IPeerSink() = default;
    virtual void onPeerReady(PeerSession& s) = 0;
    virtual void onPeerMessage(PeerSession& s, const protocol::Message& m) = 0;
    virtual void onPeerChunk(PeerSession& s, std::uint32_t streamId, std::string_view data) = 0;
    virtual void onPeerClosed(const std::string& peerId) = 0;
};

class DeviceManager : public ISessionListener {
public:
    struct Deps {
        asio::io_context& io;
        const security::Identity& identity;
        std::string deviceName;
        storage::DeviceRepository& devices;
        std::shared_ptr<const IClock> clock;
        std::shared_ptr<asio::ssl::context> clientCtx;
        transport::ConnectionLimits limits;
        EventHandler emit;
    };

    explicit DeviceManager(Deps deps);
    ~DeviceManager() override;

    void setSink(IPeerSink* sink) { sink_ = sink; }
    void setDeviceName(std::string name) { deps_.deviceName = std::move(name); }

    // Lifecycle
    void start();
    void shutdown();   // closes every session and timer

    // Discovery input (engine thread).
    void onDiscovered(const discovery::DiscoveredDevice& d);
    void onLost(const std::string& deviceId);

    // Transport input.
    void onInboundConnection(std::shared_ptr<transport::Connection> conn);

    // Commands.
    Result<void> startPairing(const std::string& deviceId);
    void decidePairing(const std::string& deviceId, bool accepted);
    Result<void> unpair(const std::string& deviceId);
    // Deletes the record of a device that is not currently paired (revoked/unknown).
    Result<void> forget(const std::string& deviceId);
    void connectNow(const std::string& deviceId);

    // Queries.
    [[nodiscard]] PeerSession* trustedSession(const std::string& deviceId) const;
    [[nodiscard]] std::vector<DeviceInfo> deviceList() const;
    [[nodiscard]] std::string deviceName(const std::string& deviceId) const;
    [[nodiscard]] bool isPaired(const std::string& deviceId) const;

    // ISessionListener
    void onSessionHello(PeerSession& s) override;
    void onSessionMessage(PeerSession& s, const protocol::Message& m) override;
    void onSessionChunk(PeerSession& s, std::uint32_t streamId, std::string_view data) override;
    void onSessionClosed(PeerSession& s, const Error& reason) override;

private:
    struct Attempt {
        PairingState state;
        std::shared_ptr<PeerSession> session;
        std::string code;                // empty until both nonces are revealed
        std::string peerName;
        std::string myNonce, peerCommit, peerNonce;
        bool incoming{false};
        bool revealSent{false};
        std::unique_ptr<asio::steady_timer> timer;
    };

    std::shared_ptr<PeerSession> attachSession(std::shared_ptr<transport::Connection> conn, bool outbound);
    void beginAttempt(PeerSession& s, bool incoming);
    void sendReveal(Attempt& a);
    void onReveal(PeerSession& s, Attempt& a, const std::string& nonce);
    void failVerification(const std::string& id, const std::string& why);
    void evaluate(const std::string& id);
    void finishAttempt(const std::string& id, const std::string& outcome, bool isError);
    void markTrusted(const std::shared_ptr<PeerSession>& s);
    void reconnectTick();
    void emit(EngineEvent::Kind k, std::string title = {}, std::string text = {}, bool err = false);
    std::shared_ptr<PeerSession> findShared(const PeerSession& s) const;

    Deps deps_;
    IPeerSink* sink_{nullptr};
    std::map<std::string, discovery::DiscoveredDevice> discovered_;
    std::map<std::string, std::shared_ptr<PeerSession>> sessions_;   // by peer id
    std::set<std::shared_ptr<PeerSession>> unbound_;                  // before hello
    std::map<std::string, Attempt> attempts_;
    std::map<std::string, TimestampMs> lastDial_;
    std::set<std::string> dialing_;
    asio::steady_timer reconnectTimer_;
    bool running_{false};
};

}  // namespace lansync::device
