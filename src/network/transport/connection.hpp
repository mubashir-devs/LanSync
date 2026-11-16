#pragma once
// TLS 1.3 framed connection built on Boost.Asio.
//
// * Mutual TLS: both sides present their self-signed identity certificate.
//   The TLS layer accepts any well-formed certificate; WHO the peer is gets
//   decided by the application from `peer()` (pinning), never by the TLS layer.
// * All I/O is asynchronous; every handler holds a shared_ptr to the Connection
//   so it cannot be destroyed under an in-flight operation.
// * Threading: all methods must be called on the io_context's thread(s) with a
//   single-threaded io_context (the Engine's model). No internal locking.

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "core/common/result.hpp"
#include "network/protocol/framing.hpp"
#include "network/security/identity.hpp"

namespace lansync::transport {

namespace asio = boost::asio;

struct ConnectionLimits {
    std::size_t maxFrameBytes{8 * 1024 * 1024};
    std::chrono::seconds idleTimeout{60};
    std::chrono::seconds handshakeTimeout{10};
    std::size_t maxQueuedWriteBytes{32 * 1024 * 1024};
};

Result<std::shared_ptr<asio::ssl::context>> makeTlsContext(const security::Identity& id,
                                                           bool server);

class Connection : public std::enable_shared_from_this<Connection> {
public:
    struct Handlers {
        std::function<void(protocol::Frame)> onFrame;
        std::function<void(const Error&)> onClosed;   // called at most once
    };
    using SendDone = std::function<void(bool ok)>;

    static std::shared_ptr<Connection> create(asio::io_context& io,
                                              std::shared_ptr<asio::ssl::context> ctx,
                                              asio::ip::tcp::socket socket, bool isServer,
                                              ConnectionLimits limits);
    ~Connection();

    // Completes the TLS handshake and extracts the peer's identity.
    void handshake(std::function<void(Result<security::PeerIdentity>)> done);

    // Begins reading frames. Call right after a successful handshake.
    void start(Handlers handlers);

    // Queues an already-framed buffer. `done(ok)` runs after it was written (or
    // failed); use it to apply backpressure when streaming.
    void send(std::string framed, SendDone done = {});

    void close(const Error& reason = Error{ErrorCode::Cancelled, "closed locally"});

    // Invoked once when the connection closes, independent of Handlers (used by
    // the server for connection accounting).
    void setCloseObserver(std::function<void()> observer) { closeObserver_ = std::move(observer); }

    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] const security::PeerIdentity& peer() const { return peer_; }
    [[nodiscard]] asio::ip::address remoteAddress() const { return remoteAddr_; }
    [[nodiscard]] std::uint16_t remotePort() const { return remotePort_; }
    [[nodiscard]] std::size_t queuedBytes() const { return queuedBytes_; }

private:
    Connection(asio::io_context& io, std::shared_ptr<asio::ssl::context> ctx,
               asio::ip::tcp::socket socket, bool isServer, ConnectionLimits limits);

    void readLoop();
    void writeLoop();
    void armIdleTimer();
    void finish(const Error& reason);

    using SslStream = asio::ssl::stream<asio::ip::tcp::socket>;

    asio::io_context& io_;
    std::shared_ptr<asio::ssl::context> ctx_;   // must outlive the stream
    SslStream stream_;
    asio::steady_timer timer_;
    bool isServer_;
    ConnectionLimits limits_;
    protocol::FrameDecoder decoder_;
    Handlers handlers_;
    std::function<void()> closeObserver_;
    security::PeerIdentity peer_;
    asio::ip::address remoteAddr_;
    std::uint16_t remotePort_{0};

    struct Pending { std::string data; SendDone done; };
    std::deque<Pending> queue_;
    std::size_t queuedBytes_{0};
    bool writing_{false};
    bool open_{true};
    std::array<char, 65536> readBuf_{};
};

// Outgoing connection with connect + handshake under `limits` timeouts.
void connectTo(asio::io_context& io, std::shared_ptr<asio::ssl::context> ctx,
               const asio::ip::tcp::endpoint& endpoint, ConnectionLimits limits,
               std::function<void(Result<std::shared_ptr<Connection>>)> done);

// True for loopback, RFC1918, link-local and unique-local addresses.
bool isPrivateAddress(const asio::ip::address& addr);

}  // namespace lansync::transport
