#pragma once
// Accepts inbound TLS connections, enforcing resource limits and (by default)
// refusing peers that are not on a private/local network.

#include <functional>
#include <memory>

#include "network/transport/connection.hpp"

namespace lansync::transport {

struct ServerOptions {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{0};                 // 0 = ephemeral
    bool allowPublicPeers{false};          // accept non-private source addresses
    std::size_t maxConnections{64};        // established + handshaking
    ConnectionLimits limits{};
};

class TlsServer : public std::enable_shared_from_this<TlsServer> {
public:
    using OnConnection = std::function<void(std::shared_ptr<Connection>)>;

    TlsServer(asio::io_context& io, std::shared_ptr<asio::ssl::context> ctx, ServerOptions opts);

    Result<void> start(OnConnection onConnection);
    void stop();
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void connectionClosed();
    void acceptLoop();

    asio::io_context& io_;
    std::shared_ptr<asio::ssl::context> ctx_;
    ServerOptions opts_;
    asio::ip::tcp::acceptor acceptor_;
    OnConnection onConnection_;
    std::uint16_t port_{0};
    std::size_t active_{0};
    bool running_{false};
};

}  // namespace lansync::transport
