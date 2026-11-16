#include "network/transport/connection.hpp"

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <utility>

#include "core/common/logging.hpp"

namespace lansync::transport {

Result<std::shared_ptr<asio::ssl::context>> makeTlsContext(const security::Identity& id,
                                                           bool server) {
    try {
        auto ctx = std::make_shared<asio::ssl::context>(server ? asio::ssl::context::tls_server
                                                               : asio::ssl::context::tls_client);
        ctx->set_options(asio::ssl::context::default_workarounds |
                         asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3);
        if (SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_3_VERSION) != 1) {
            return Error{ErrorCode::SecurityError, "cannot require TLS 1.3"};
        }
        ctx->use_certificate(asio::buffer(id.certificatePem), asio::ssl::context::pem);
        ctx->use_private_key(asio::buffer(id.privateKeyPem), asio::ssl::context::pem);
        ctx->set_verify_mode(asio::ssl::verify_peer |
                             (server ? asio::ssl::verify_fail_if_no_peer_cert : 0));
        // Self-signed identities: accept at the TLS layer, authorize by pinned
        // device id in the application layer (see header comment).
        ctx->set_verify_callback([](bool, asio::ssl::verify_context&) { return true; });
        return ctx;
    } catch (const std::exception& e) {
        return Error{ErrorCode::SecurityError, std::string("TLS context: ") + e.what()};
    }
}

bool isPrivateAddress(const asio::ip::address& addr) {
    if (addr.is_loopback()) return true;
    if (addr.is_v4()) {
        const auto b = addr.to_v4().to_bytes();
        return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
               (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
    }
    const auto v6 = addr.to_v6();
    if (v6.is_v4_mapped()) return isPrivateAddress(asio::ip::make_address_v4(asio::ip::v4_mapped, v6));
    const auto b = v6.to_bytes();
    return (b[0] & 0xFE) == 0xFC || (b[0] == 0xFE && (b[1] & 0xC0) == 0x80);
}

std::shared_ptr<Connection> Connection::create(asio::io_context& io,
                                               std::shared_ptr<asio::ssl::context> ctx,
                                               asio::ip::tcp::socket socket, bool isServer,
                                               ConnectionLimits limits) {
    return std::shared_ptr<Connection>(
        new Connection(io, std::move(ctx), std::move(socket), isServer, limits));
}

Connection::Connection(asio::io_context& io, std::shared_ptr<asio::ssl::context> ctx,
                       asio::ip::tcp::socket socket, bool isServer, ConnectionLimits limits)
    : io_(io),
      ctx_(std::move(ctx)),
      stream_(std::move(socket), *ctx_),
      timer_(io),
      isServer_(isServer),
      limits_(limits),
      decoder_(limits.maxFrameBytes) {
    boost::system::error_code ec;
    const auto ep = stream_.lowest_layer().remote_endpoint(ec);
    if (!ec) {
        remoteAddr_ = ep.address();
        remotePort_ = ep.port();
    }
}

Connection::~Connection() = default;

void Connection::handshake(std::function<void(Result<security::PeerIdentity>)> done) {
    auto self = shared_from_this();
    timer_.expires_after(limits_.handshakeTimeout);
    timer_.async_wait([self](const boost::system::error_code& ec) {
        if (!ec) self->close(Error{ErrorCode::Timeout, "TLS handshake timed out"});
    });
    stream_.async_handshake(isServer_ ? asio::ssl::stream_base::server
                                      : asio::ssl::stream_base::client,
                            [self, done = std::move(done)](const boost::system::error_code& ec) {
                                self->timer_.cancel();
                                if (ec) {
                                    const Error err{ErrorCode::SecurityError,
                                                    "TLS handshake failed: " + ec.message()};
                                    self->close(err);
                                    done(err);
                                    return;
                                }
                                X509* raw = SSL_get1_peer_certificate(self->stream_.native_handle());
                                auto peer = security::peerFromCertificate(raw);
                                if (raw) X509_free(raw);
                                if (!peer) {
                                    self->close(peer.error());
                                    done(peer.error());
                                    return;
                                }
                                self->peer_ = *peer;
                                done(std::move(*peer));
                            });
}

void Connection::start(Handlers handlers) {
    handlers_ = std::move(handlers);
    armIdleTimer();
    readLoop();
}

void Connection::armIdleTimer() {
    auto self = shared_from_this();
    timer_.expires_after(limits_.idleTimeout);
    timer_.async_wait([self](const boost::system::error_code& ec) {
        if (!ec) self->close(Error{ErrorCode::Timeout, "connection idle timeout"});
    });
}

void Connection::readLoop() {
    if (!open_) return;
    auto self = shared_from_this();
    stream_.async_read_some(
        asio::buffer(readBuf_), [self](const boost::system::error_code& ec, std::size_t n) {
            if (!self->open_) return;
            if (ec) {
                self->finish(Error{ec == asio::error::eof ? ErrorCode::Cancelled : ErrorCode::IoError,
                                   "read: " + ec.message()});
                return;
            }
            self->armIdleTimer();
            self->decoder_.feed(self->readBuf_.data(), n);
            for (;;) {
                auto frame = self->decoder_.next();
                if (!frame) {
                    self->close(frame.error());
                    return;
                }
                if (!frame->has_value()) break;
                if (!self->open_) return;
                // JSON frames have a much tighter bound than file chunks.
                if ((*frame)->kind == protocol::FrameKind::Json &&
                    (*frame)->payload.size() > 1024 * 1024) {
                    self->close(Error{ErrorCode::ProtocolError, "oversized JSON frame"});
                    return;
                }
                if (self->handlers_.onFrame) self->handlers_.onFrame(std::move(**frame));
                if (!self->open_) return;
            }
            self->readLoop();
        });
}

void Connection::send(std::string framed, SendDone done) {
    if (!open_) {
        if (done) done(false);
        return;
    }
    queuedBytes_ += framed.size();
    if (queuedBytes_ > limits_.maxQueuedWriteBytes) {
        if (done) done(false);
        close(Error{ErrorCode::Busy, "peer too slow: write queue limit exceeded"});
        return;
    }
    queue_.push_back({std::move(framed), std::move(done)});
    if (!writing_) writeLoop();
}

void Connection::writeLoop() {
    if (queue_.empty() || !open_) {
        writing_ = false;
        return;
    }
    writing_ = true;
    auto self = shared_from_this();
    asio::async_write(stream_, asio::buffer(queue_.front().data),
                      [self](const boost::system::error_code& ec, std::size_t) {
                          if (self->queue_.empty()) return;   // closed meanwhile
                          auto item = std::move(self->queue_.front());
                          self->queue_.pop_front();
                          self->queuedBytes_ -= item.data.size();
                          if (ec) {
                              if (item.done) item.done(false);
                              self->finish(Error{ErrorCode::IoError, "write: " + ec.message()});
                              return;
                          }
                          if (item.done) item.done(true);
                          self->writeLoop();
                      });
}

void Connection::close(const Error& reason) { finish(reason); }

void Connection::finish(const Error& reason) {
    if (!open_) return;
    open_ = false;
    timer_.cancel();
    boost::system::error_code ignored;
    stream_.lowest_layer().cancel(ignored);
    stream_.lowest_layer().close(ignored);
    auto pending = std::move(queue_);
    queue_.clear();
    queuedBytes_ = 0;
    for (auto& p : pending) {
        if (p.done) p.done(false);
    }
    if (closeObserver_) std::exchange(closeObserver_, nullptr)();
    auto onClosed = std::move(handlers_.onClosed);
    handlers_ = {};   // break reference cycles with the owner
    if (onClosed) onClosed(reason);
}

void connectTo(asio::io_context& io, std::shared_ptr<asio::ssl::context> ctx,
               const asio::ip::tcp::endpoint& endpoint, ConnectionLimits limits,
               std::function<void(Result<std::shared_ptr<Connection>>)> done) {
    struct State {
        asio::ip::tcp::socket socket;
        asio::steady_timer timer;
        bool finished{false};
        std::function<void(Result<std::shared_ptr<Connection>>)> done;
        State(asio::io_context& io_, std::function<void(Result<std::shared_ptr<Connection>>)> d)
            : socket(io_), timer(io_), done(std::move(d)) {}
    };
    auto st = std::make_shared<State>(io, std::move(done));
    st->timer.expires_after(limits.handshakeTimeout);
    st->timer.async_wait([st](const boost::system::error_code& ec) {
        if (ec || st->finished) return;
        st->finished = true;
        boost::system::error_code ignored;
        st->socket.close(ignored);
        st->done(Error{ErrorCode::Timeout, "connect timed out"});
    });
    st->socket.async_connect(endpoint, [&io, ctx, limits, st](const boost::system::error_code& ec) {
        if (st->finished) return;
        if (ec) {
            st->finished = true;
            st->timer.cancel();
            st->done(Error{ErrorCode::IoError, "connect: " + ec.message()});
            return;
        }
        st->finished = true;
        st->timer.cancel();
        auto conn = Connection::create(io, ctx, std::move(st->socket), false, limits);
        conn->handshake([conn, st](Result<security::PeerIdentity> peer) {
            if (!peer) {
                st->done(peer.error());
                return;
            }
            st->done(conn);
        });
    });
}

}  // namespace lansync::transport
