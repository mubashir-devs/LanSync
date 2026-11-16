#include "core/device/peer_session.hpp"

#include "core/common/logging.hpp"
#include "core/common/version.hpp"

namespace lansync::device {

namespace proto = lansync::protocol;

PeerSession::PeerSession(std::shared_ptr<transport::Connection> conn, std::string localDeviceId,
                         std::string localDeviceName, bool outbound, ISessionListener& listener,
                         asio::io_context& io)
    : conn_(std::move(conn)),
      localId_(std::move(localDeviceId)),
      localName_(std::move(localDeviceName)),
      outbound_(outbound),
      listener_(&listener),
      keepalive_(io) {}

PeerSession::~PeerSession() = default;

void PeerSession::start() {
    auto self = shared_from_this();
    conn_->start({[self](proto::Frame f) { self->onFrame(std::move(f)); },
                  [self](const Error& e) { self->onClosed(e); }});
    send(proto::type::kHello,
         {{"device_name", localName_}, {"app_version", kAppVersion}});
    scheduleKeepalive();
}

void PeerSession::scheduleKeepalive() {
    auto self = shared_from_this();
    keepalive_.expires_after(std::chrono::seconds(15));
    keepalive_.async_wait([self](const boost::system::error_code& ec) {
        if (ec || self->closed_) return;
        self->send(proto::type::kPing);
        self->scheduleKeepalive();
    });
}

void PeerSession::send(std::string_view type, nlohmann::json payload) {
    if (closed_) return;
    conn_->send(proto::encodeJsonFrame(proto::serialize(proto::make(type, localId_, std::move(payload)))));
}

void PeerSession::sendFrame(std::string framed, transport::Connection::SendDone done) {
    if (closed_) {
        if (done) done(false);
        return;
    }
    conn_->send(std::move(framed), std::move(done));
}

void PeerSession::sendChunk(std::uint32_t streamId, std::string_view data,
                            transport::Connection::SendDone done) {
    sendFrame(proto::encodeChunkFrame(streamId, data), std::move(done));
}

void PeerSession::close(const Error& reason) {
    if (closed_) return;
    conn_->close(reason);   // triggers onClosed
}

void PeerSession::onClosed(const Error& reason) {
    if (closed_) return;
    closed_ = true;
    keepalive_.cancel();
    auto* l = std::exchange(listener_, nullptr);
    if (l) l->onSessionClosed(*this, reason);
}

void PeerSession::onFrame(proto::Frame frame) {
    if (closed_ || !listener_) return;

    if (frame.kind == proto::FrameKind::Chunk) {
        auto chunk = proto::parseChunk(frame);
        if (!chunk || !trusted_) {
            close(Error{ErrorCode::ProtocolError, "unexpected chunk frame"});
            return;
        }
        listener_->onSessionChunk(*this, chunk->streamId, chunk->data);
        return;
    }

    auto msg = proto::parse(frame.payload);
    if (!msg) {
        LS_WARN("closing session with {}: {}", peerId().substr(0, 8), msg.error().message);
        if (msg.error().code == ErrorCode::UnsupportedVersion) {
            send(proto::type::kError, {{"code", "unsupported_version"},
                                       {"supported", kProtocolVersion}});
        }
        close(msg.error());
        return;
    }

    // The envelope's device_id must be the certificate-bound id, always.
    if (msg->deviceId != peerId()) {
        close(Error{ErrorCode::SecurityError, "device_id does not match TLS certificate"});
        return;
    }

    if (!helloReceived_) {
        if (msg->type != proto::type::kHello) {
            close(Error{ErrorCode::ProtocolError, "expected hello"});
            return;
        }
        peerName_ = msg->payload.value("device_name", std::string{});
        peerAppVersion_ = msg->payload.value("app_version", std::string{});
        if (peerName_.empty() || peerName_.size() > 128 || peerAppVersion_.size() > 32) {
            close(Error{ErrorCode::ProtocolError, "invalid hello"});
            return;
        }
        helloReceived_ = true;
        listener_->onSessionHello(*this);
        return;
    }

    if (msg->type == proto::type::kPing) {
        send(proto::type::kPong);
        return;
    }
    if (msg->type == proto::type::kPong) return;

    if (!trusted_ && msg->type != proto::type::kPairRequest &&
        msg->type != proto::type::kPairDecision && msg->type != proto::type::kPairCommit &&
        msg->type != proto::type::kPairReveal && msg->type != proto::type::kError) {
        LS_WARN("untrusted peer {} sent '{}'; closing", peerId().substr(0, 8), msg->type);
        send(proto::type::kError, {{"code", "not_paired"}});
        close(Error{ErrorCode::SecurityError, "message from untrusted peer"});
        return;
    }
    listener_->onSessionMessage(*this, *msg);
}

}  // namespace lansync::device
