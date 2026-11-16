#include <gtest/gtest.h>

#include "network/protocol/message.hpp"
#include "network/transport/connection.hpp"
#include "network/transport/tls_server.hpp"

using namespace lansync;
using namespace lansync::transport;
namespace proto = lansync::protocol;

namespace {

struct Fixture {
    asio::io_context io;
    security::Identity serverId = *security::generateIdentity("server");
    security::Identity clientId = *security::generateIdentity("client");
    std::shared_ptr<TlsServer> server;
    std::shared_ptr<Connection> serverConn, clientConn;
    std::vector<proto::Frame> serverGot, clientGot;
    std::vector<Error> serverClosed, clientClosed;

    void startServer(ServerOptions opts = {}) {
        opts.bindAddress = "127.0.0.1";
        server = std::make_shared<TlsServer>(io, *makeTlsContext(serverId, true), opts);
        ASSERT_TRUE(server->start([this](std::shared_ptr<Connection> c) {
            serverConn = c;
            c->start({[this](proto::Frame f) { serverGot.push_back(std::move(f)); },
                      [this](const Error& e) { serverClosed.push_back(e); }});
        }));
    }
    void connect(ConnectionLimits limits = {}) {
        connectTo(io, *makeTlsContext(clientId, false),
                  {asio::ip::make_address_v4("127.0.0.1"), server->port()}, limits,
                  [this](Result<std::shared_ptr<Connection>> r) {
                      ASSERT_TRUE(r) << r.error().message;
                      clientConn = *r;
                      clientConn->start({[this](proto::Frame f) { clientGot.push_back(std::move(f)); },
                                         [this](const Error& e) { clientClosed.push_back(e); }});
                  });
    }
    void run(int ms = 500) { io.restart(); io.run_for(std::chrono::milliseconds(ms)); }
};

}  // namespace

TEST(Transport, MutualTlsAuthenticatesBothPeers) {
    Fixture f;
    f.startServer();
    f.connect();
    f.run();
    ASSERT_TRUE(f.serverConn && f.clientConn);
    EXPECT_EQ(f.serverConn->peer().deviceId, f.clientId.deviceId);
    EXPECT_EQ(f.serverConn->peer().fingerprint, f.clientId.fingerprint);
    EXPECT_EQ(f.clientConn->peer().deviceId, f.serverId.deviceId);
}

TEST(Transport, FramesFlowBothWays) {
    Fixture f;
    f.startServer();
    f.connect();
    f.run(300);
    f.clientConn->send(proto::encodeJsonFrame("hello-from-client"));
    f.serverConn->send(proto::encodeJsonFrame("hello-from-server"));
    f.serverConn->send(proto::encodeChunkFrame(9, "chunk"));
    f.run(300);
    ASSERT_EQ(f.serverGot.size(), 1u);
    EXPECT_EQ(f.serverGot[0].payload, "hello-from-client");
    ASSERT_EQ(f.clientGot.size(), 2u);
    EXPECT_EQ(f.clientGot[1].kind, proto::FrameKind::Chunk);
}

TEST(Transport, LargeFrameSplitAcrossManyReadsArrivesIntact) {
    Fixture f;
    f.startServer();
    f.connect();
    f.run(300);
    std::string big(3 * 1024 * 1024, 'q');
    big[12345] = 'Z';
    f.clientConn->send(proto::encodeChunkFrame(1, big));
    f.run(2000);
    ASSERT_EQ(f.serverGot.size(), 1u);
    auto chunk = proto::parseChunk(f.serverGot[0]);
    ASSERT_TRUE(chunk);
    EXPECT_EQ(chunk->data.size(), big.size());
    EXPECT_EQ(chunk->data[12345], 'Z');
}

TEST(Transport, OversizedFrameClosesConnection) {
    Fixture f;
    ServerOptions so;
    so.limits.maxFrameBytes = 1024;
    f.startServer(so);
    f.connect();
    f.run(300);
    f.clientConn->send(proto::encodeChunkFrame(1, std::string(5000, 'x')));
    f.run(500);
    EXPECT_TRUE(f.serverGot.empty());
    ASSERT_EQ(f.serverClosed.size(), 1u);
    EXPECT_EQ(f.serverClosed[0].code, ErrorCode::ProtocolError);
}

TEST(Transport, IdleConnectionTimesOut) {
    Fixture f;
    ServerOptions so;
    so.limits.idleTimeout = std::chrono::seconds(1);
    f.startServer(so);
    f.connect();
    f.run(1800);
    ASSERT_EQ(f.serverClosed.size(), 1u);
    EXPECT_EQ(f.serverClosed[0].code, ErrorCode::Timeout);
}

TEST(Transport, ConnectionLimitIsEnforced) {
    Fixture f;
    ServerOptions so;
    so.maxConnections = 1;
    f.startServer(so);
    f.connect();
    f.run(300);
    ASSERT_TRUE(f.serverConn);

    bool secondFailedOrClosed = false;
    auto clientCtx = *makeTlsContext(f.clientId, false);
    connectTo(f.io, clientCtx, {asio::ip::make_address_v4("127.0.0.1"), f.server->port()}, {},
              [&](Result<std::shared_ptr<Connection>> r) { secondFailedOrClosed = !r.ok(); });
    f.run(500);
    EXPECT_TRUE(secondFailedOrClosed);
}

TEST(Transport, ClientCloseIsObservedByServer) {
    Fixture f;
    f.startServer();
    f.connect();
    f.run(300);
    f.clientConn->close();
    f.run(300);
    EXPECT_EQ(f.serverClosed.size(), 1u);
}

TEST(Transport, PrivateAddressClassification) {
    auto ip = [](const char* s) { return asio::ip::make_address(s); };
    EXPECT_TRUE(isPrivateAddress(ip("127.0.0.1")));
    EXPECT_TRUE(isPrivateAddress(ip("10.1.2.3")));
    EXPECT_TRUE(isPrivateAddress(ip("172.16.0.1")));
    EXPECT_TRUE(isPrivateAddress(ip("192.168.1.9")));
    EXPECT_TRUE(isPrivateAddress(ip("169.254.1.1")));
    EXPECT_TRUE(isPrivateAddress(ip("fe80::1")));
    EXPECT_TRUE(isPrivateAddress(ip("fd00::1")));
    EXPECT_FALSE(isPrivateAddress(ip("8.8.8.8")));
    EXPECT_FALSE(isPrivateAddress(ip("172.32.0.1")));
    EXPECT_FALSE(isPrivateAddress(ip("2001:4860:4860::8888")));
}
