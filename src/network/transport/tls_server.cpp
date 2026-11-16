#include "network/transport/tls_server.hpp"

#include "core/common/logging.hpp"

namespace lansync::transport {

TlsServer::TlsServer(asio::io_context& io, std::shared_ptr<asio::ssl::context> ctx,
                     ServerOptions opts)
    : io_(io), ctx_(std::move(ctx)), opts_(std::move(opts)), acceptor_(io) {}

Result<void> TlsServer::start(OnConnection onConnection) {
    onConnection_ = std::move(onConnection);
    boost::system::error_code ec;
    const auto addr = asio::ip::make_address(opts_.bindAddress, ec);
    if (ec) return Error{ErrorCode::InvalidArgument, "bad bind address: " + opts_.bindAddress};
    const asio::ip::tcp::endpoint ep(addr, opts_.port);
    acceptor_.open(ep.protocol(), ec);
    if (!ec) acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (!ec) acceptor_.bind(ep, ec);
    if (!ec) acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot listen on " + opts_.bindAddress + ":" +
                                                 std::to_string(opts_.port) + ": " + ec.message()};
    port_ = acceptor_.local_endpoint().port();
    running_ = true;
    acceptLoop();
    return {};
}

void TlsServer::stop() {
    running_ = false;
    boost::system::error_code ignored;
    acceptor_.close(ignored);
}

void TlsServer::connectionClosed() {
    if (active_ > 0) --active_;
}

void TlsServer::acceptLoop() {
    if (!running_) return;
    auto self = shared_from_this();
    acceptor_.async_accept([self](const boost::system::error_code& ec, asio::ip::tcp::socket sock) {
        if (!self->running_) return;
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                LS_WARN("accept failed: {}", ec.message());
            }
            if (self->running_ && ec != asio::error::operation_aborted) self->acceptLoop();
            return;
        }
        boost::system::error_code rec;
        const auto remote = sock.remote_endpoint(rec);
        if (rec || (!self->opts_.allowPublicPeers && !isPrivateAddress(remote.address()))) {
            LS_WARN("rejecting connection from non-private address {}",
                    rec ? "?" : remote.address().to_string());
            boost::system::error_code ignored;
            sock.close(ignored);
        } else if (self->active_ >= self->opts_.maxConnections) {
            LS_WARN("connection limit reached; rejecting {}", remote.address().to_string());
            boost::system::error_code ignored;
            sock.close(ignored);
        } else {
            ++self->active_;
            auto conn = Connection::create(self->io_, self->ctx_, std::move(sock), true,
                                           self->opts_.limits);
            conn->setCloseObserver([self] { self->connectionClosed(); });
            conn->handshake([self, conn](Result<security::PeerIdentity> peer) {
                if (!peer) {
                    LS_DEBUG("inbound handshake failed: {}", peer.error().message);
                    return;
                }
                if (self->onConnection_) self->onConnection_(conn);
            });
        }
        self->acceptLoop();
    });
}

}  // namespace lansync::transport
