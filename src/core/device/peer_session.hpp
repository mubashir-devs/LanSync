#pragma once
// One authenticated-or-pairing connection to a peer.
//
// Lifecycle:  TLS handshake (done by Connection)  ->  hello exchange  ->  ready.
// The hello's device_id MUST equal the id derived from the TLS certificate;
// otherwise the peer is claiming an identity it does not hold a key for and the
// session is dropped. Until the DeviceManager marks the session `trusted`
// (peer is paired AND its pinned fingerprint matches), only hello/ping/pong/
// error/pair_* messages are accepted.

#include <chrono>
#include <memory>
#include <string>

#include "network/protocol/message.hpp"
#include "network/transport/connection.hpp"

namespace lansync::device {

namespace asio = boost::asio;

class PeerSession;

class ISessionListener {
public:
    virtual ~ISessionListener() = default;
    // Peer sent a valid hello whose device_id matches its certificate.
    virtual void onSessionHello(PeerSession& s) = 0;
    virtual void onSessionMessage(PeerSession& s, const protocol::Message& m) = 0;
    virtual void onSessionChunk(PeerSession& s, std::uint32_t streamId, std::string_view data) = 0;
    virtual void onSessionClosed(PeerSession& s, const Error& reason) = 0;
};

class PeerSession : public std::enable_shared_from_this<PeerSession> {
public:
    PeerSession(std::shared_ptr<transport::Connection> conn, std::string localDeviceId,
                std::string localDeviceName, bool outbound, ISessionListener& listener,
                asio::io_context& io);
    ~PeerSession();

    // Starts reading and sends our hello.
    void start();
    void close(const Error& reason = Error{ErrorCode::Cancelled, "session closed"});

    void send(std::string_view type, nlohmann::json payload = nlohmann::json::object());
    void sendFrame(std::string framed, transport::Connection::SendDone done = {});
    void sendChunk(std::uint32_t streamId, std::string_view data,
                   transport::Connection::SendDone done = {});

    [[nodiscard]] const std::string& peerId() const { return conn_->peer().deviceId; }
    [[nodiscard]] const std::string& peerFingerprint() const { return conn_->peer().fingerprint; }
    [[nodiscard]] const std::string& peerPublicKeyPem() const { return conn_->peer().publicKeyPem; }
    [[nodiscard]] const std::string& peerName() const { return peerName_; }
    [[nodiscard]] const std::string& peerAppVersion() const { return peerAppVersion_; }
    [[nodiscard]] std::string address() const { return conn_->remoteAddress().to_string(); }
    [[nodiscard]] std::uint16_t remotePort() const { return conn_->remotePort(); }
    [[nodiscard]] bool outbound() const { return outbound_; }
    [[nodiscard]] bool helloReceived() const { return helloReceived_; }
    [[nodiscard]] bool isOpen() const { return conn_->isOpen(); }
    [[nodiscard]] std::size_t queuedBytes() const { return conn_->queuedBytes(); }

    [[nodiscard]] bool trusted() const { return trusted_; }
    void setTrusted(bool t) { trusted_ = t; }

private:
    void onFrame(protocol::Frame frame);
    void onClosed(const Error& reason);
    void scheduleKeepalive();

    std::shared_ptr<transport::Connection> conn_;
    std::string localId_, localName_;
    bool outbound_;
    ISessionListener* listener_;   // cleared on close
    asio::steady_timer keepalive_;
    std::string peerName_, peerAppVersion_;
    bool helloReceived_{false};
    bool trusted_{false};
    bool closed_{false};
};

}  // namespace lansync::device
