// Two DeviceManagers talking over real TLS on loopback (no UDP involved:
// discovery results are injected).
#include <gtest/gtest.h>

#include "core/device/device_manager.hpp"
#include "network/transport/tls_server.hpp"
#include "storage/database/migrator.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::device;

namespace {

struct Node : IPeerSink {
    asio::io_context& io;
    security::Identity id;
    storage::Database db = std::move(*storage::Database::open(":memory:"));
    std::unique_ptr<storage::DeviceRepository> repo;
    std::shared_ptr<transport::TlsServer> server;
    std::unique_ptr<DeviceManager> dm;
    std::vector<EngineEvent> events;
    int ready{0}, closed{0};
    std::vector<std::string> received;

    Node(asio::io_context& io_, const std::string& name) : io(io_), id(*security::generateIdentity(name)) {
        support::FakeClock c;
        (void)storage::Migrator::migrate(db, storage::builtinMigrations(), c);
        repo = std::make_unique<storage::DeviceRepository>(db);
        transport::ServerOptions so;
        so.bindAddress = "127.0.0.1";
        server = std::make_shared<transport::TlsServer>(io, *transport::makeTlsContext(id, true), so);
        DeviceManager::Deps deps{io, id, name, *repo, std::make_shared<SystemClock>(),
                                 *transport::makeTlsContext(id, false), {},
                                 [this](const EngineEvent& e) { events.push_back(e); }};
        dm = std::make_unique<DeviceManager>(std::move(deps));
        dm->setSink(this);
        EXPECT_TRUE(server->start([this](auto conn) { dm->onInboundConnection(conn); }));
        dm->start();
    }
    ~Node() override { dm->shutdown(); server->stop(); }

    void see(Node& other) {
        discovery::DiscoveredDevice d;
        d.deviceId = other.id.deviceId;
        d.deviceName = "other";
        d.address = "127.0.0.1";
        d.tcpPort = other.server->port();
        d.lastSeenMs = SystemClock().nowMs();
        dm->onDiscovered(d);
    }
    const EngineEvent* find(EngineEvent::Kind k) const {
        for (const auto& e : events) if (e.kind == k) return &e;
        return nullptr;
    }
    void onPeerReady(PeerSession&) override { ++ready; }
    void onPeerMessage(PeerSession&, const protocol::Message& m) override { received.push_back(m.type); }
    void onPeerChunk(PeerSession&, std::uint32_t, std::string_view) override {}
    void onPeerClosed(const std::string&) override { ++closed; }
};

void run(asio::io_context& io, int ms) { io.restart(); io.run_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST(Pairing, BothUsersApproveWithMatchingCodes) {
    asio::io_context io;
    Node a(io, "alice"), b(io, "bob");
    a.see(b);
    ASSERT_TRUE(a.dm->startPairing(b.id.deviceId));
    run(io, 700);

    const auto* pa = a.find(EngineEvent::Kind::PairingRequested);
    const auto* pb = b.find(EngineEvent::Kind::PairingRequested);
    ASSERT_TRUE(pa && pb);
    EXPECT_FALSE(pa->pairing.incoming);
    EXPECT_TRUE(pb->pairing.incoming);
    EXPECT_EQ(pa->pairing.code, pb->pairing.code);   // both users compare the same 6 digits
    EXPECT_EQ(pa->pairing.code.size(), 6u);
    EXPECT_FALSE(a.dm->isPaired(b.id.deviceId));      // nothing trusted before approval

    a.dm->decidePairing(b.id.deviceId, true);
    run(io, 300);
    EXPECT_FALSE(a.dm->isPaired(b.id.deviceId));      // one-sided approval is not enough
    EXPECT_FALSE(b.dm->isPaired(a.id.deviceId));

    b.dm->decidePairing(a.id.deviceId, true);
    run(io, 500);
    EXPECT_TRUE(a.dm->isPaired(b.id.deviceId));
    EXPECT_TRUE(b.dm->isPaired(a.id.deviceId));
    EXPECT_EQ(a.ready, 1);
    EXPECT_EQ(b.ready, 1);
    ASSERT_TRUE(a.dm->trustedSession(b.id.deviceId));

    // Pinned identity was persisted.
    auto row = a.repo->get(b.id.deviceId);
    ASSERT_TRUE(row && *row);
    EXPECT_EQ((*row)->fingerprint, b.id.fingerprint);
}

TEST(Pairing, RejectionLeavesNothingPersisted) {
    asio::io_context io;
    Node a(io, "alice"), b(io, "bob");
    a.see(b);
    ASSERT_TRUE(a.dm->startPairing(b.id.deviceId));
    run(io, 700);
    a.dm->decidePairing(b.id.deviceId, true);
    b.dm->decidePairing(a.id.deviceId, false);
    run(io, 500);
    EXPECT_FALSE(a.dm->isPaired(b.id.deviceId));
    EXPECT_FALSE(b.dm->isPaired(a.id.deviceId));
    EXPECT_EQ(a.ready + b.ready, 0);
    EXPECT_TRUE(a.repo->list()->empty());
    ASSERT_TRUE(a.find(EngineEvent::Kind::PairingFinished));
    EXPECT_TRUE(a.find(EngineEvent::Kind::PairingFinished)->isError);
}

TEST(Pairing, PairedDevicesReconnectAutomaticallyAndExchangeMessages) {
    asio::io_context io;
    Node a(io, "alice"), b(io, "bob");
    a.see(b);
    b.see(a);
    ASSERT_TRUE(a.dm->startPairing(b.id.deviceId));
    run(io, 700);
    a.dm->decidePairing(b.id.deviceId, true);
    b.dm->decidePairing(a.id.deviceId, true);
    run(io, 500);
    ASSERT_EQ(a.ready, 1);

    // Drop the session; the side with the smaller id must redial on its own.
    a.dm->trustedSession(b.id.deviceId)->close();
    run(io, 300);
    EXPECT_EQ(a.closed + b.closed, 2);
    run(io, 4500);
    EXPECT_EQ(a.ready + b.ready, 4);   // 2 initial + 2 after reconnect
    auto* s = a.dm->trustedSession(b.id.deviceId);
    ASSERT_TRUE(s);
    s->send(protocol::type::kIndex, {{"folder", "x"}, {"entries", nlohmann::json::array()}});
    run(io, 300);
    ASSERT_EQ(b.received.size(), 1u);
    EXPECT_EQ(b.received[0], "index");
}

TEST(Pairing, UnpairedPeerCannotSendSyncTraffic) {
    asio::io_context io;
    Node a(io, "alice"), b(io, "bob");
    a.see(b);
    // Raw connection (no pairing) that sends an index message.
    std::shared_ptr<transport::Connection> raw;
    std::vector<protocol::Frame> got;
    bool closed = false;
    transport::connectTo(io, *transport::makeTlsContext(a.id, false),
                         {asio::ip::make_address_v4("127.0.0.1"), b.server->port()}, {},
                         [&](Result<std::shared_ptr<transport::Connection>> r) {
                             ASSERT_TRUE(r);
                             raw = *r;
                             raw->start({[&](protocol::Frame f) { got.push_back(std::move(f)); },
                                         [&](const Error&) { closed = true; }});
                             raw->send(protocol::encodeJsonFrame(protocol::serialize(
                                 protocol::make(protocol::type::kHello, a.id.deviceId,
                                                {{"device_name", "a"}, {"app_version", "1"}}))));
                             raw->send(protocol::encodeJsonFrame(protocol::serialize(
                                 protocol::make(protocol::type::kIndex, a.id.deviceId,
                                                {{"folder", "x"}, {"entries", nlohmann::json::array()}}))));
                         });
    run(io, 700);
    EXPECT_TRUE(closed);
    EXPECT_TRUE(b.received.empty());
    EXPECT_EQ(b.ready, 0);
}

TEST(Pairing, SpoofedDeviceIdInHelloIsRejected) {
    asio::io_context io;
    Node b(io, "bob");
    auto mallory = *security::generateIdentity("mallory");
    auto victim = *security::generateIdentity("victim");
    bool closed = false;
    std::shared_ptr<transport::Connection> raw;
    transport::connectTo(io, *transport::makeTlsContext(mallory, false),
                         {asio::ip::make_address_v4("127.0.0.1"), b.server->port()}, {},
                         [&](Result<std::shared_ptr<transport::Connection>> r) {
                             ASSERT_TRUE(r);
                             raw = *r;
                             raw->start({[&](protocol::Frame) {}, [&](const Error&) { closed = true; }});
                             // Claims the victim's id while holding mallory's certificate.
                             raw->send(protocol::encodeJsonFrame(protocol::serialize(
                                 protocol::make(protocol::type::kHello, victim.deviceId,
                                                {{"device_name", "victim"}, {"app_version", "1"}}))));
                         });
    run(io, 700);
    EXPECT_TRUE(closed);
}

TEST(Pairing, UnpairRevokesTrustAndClosesSession) {
    asio::io_context io;
    Node a(io, "alice"), b(io, "bob");
    a.see(b);
    ASSERT_TRUE(a.dm->startPairing(b.id.deviceId));
    run(io, 700);
    a.dm->decidePairing(b.id.deviceId, true);
    b.dm->decidePairing(a.id.deviceId, true);
    run(io, 500);
    ASSERT_TRUE(a.dm->trustedSession(b.id.deviceId));
    ASSERT_TRUE(a.dm->unpair(b.id.deviceId));
    run(io, 300);
    EXPECT_FALSE(a.dm->isPaired(b.id.deviceId));
    EXPECT_EQ(a.dm->trustedSession(b.id.deviceId), nullptr);
    EXPECT_EQ(b.closed, 1);
}

TEST(Pairing, CannotPairWithUndiscoveredDevice) {
    asio::io_context io;
    Node a(io, "alice");
    auto r = a.dm->startPairing("deadbeef");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(Pairing, SimultaneousPairingFromBothSidesConvergesToOneAttempt) {
    asio::io_context io;
    Node a(io, "alice"), b(io, "bob");
    a.see(b);
    b.see(a);
    ASSERT_TRUE(a.dm->startPairing(b.id.deviceId));
    ASSERT_TRUE(b.dm->startPairing(a.id.deviceId));   // both users press "Pair" at once
    run(io, 1200);
    // Exactly one attempt survives on each side; approving it pairs the devices.
    a.dm->decidePairing(b.id.deviceId, true);
    b.dm->decidePairing(a.id.deviceId, true);
    run(io, 800);
    EXPECT_TRUE(a.dm->isPaired(b.id.deviceId));
    EXPECT_TRUE(b.dm->isPaired(a.id.deviceId));
    EXPECT_NE(a.dm->trustedSession(b.id.deviceId), nullptr);
    EXPECT_NE(b.dm->trustedSession(a.id.deviceId), nullptr);
}

// A peer that reveals a nonce different from the one it committed to (i.e. tries to steer the
// verification code) must make pairing fail without ever showing the user a code.
TEST(Pairing, RevealThatDoesNotMatchCommitmentAbortsPairing) {
    asio::io_context io;
    Node b(io, "bob");
    auto mallory = *security::generateIdentity("mallory");
    std::shared_ptr<transport::Connection> raw;
    bool commitReceived = false;
    auto send = [&](std::string_view type, nlohmann::json payload) {
        raw->send(protocol::encodeJsonFrame(protocol::serialize(protocol::make(type, mallory.deviceId, std::move(payload)))));
    };
    transport::connectTo(io, *transport::makeTlsContext(mallory, false),
                         {asio::ip::make_address_v4("127.0.0.1"), b.server->port()}, {},
                         [&](Result<std::shared_ptr<transport::Connection>> r) {
                             ASSERT_TRUE(r);
                             raw = *r;
                             raw->start({[&](protocol::Frame f) {
                                             auto m = protocol::parse(f.payload);
                                             if (m && m->type == protocol::type::kPairCommit && !commitReceived) {
                                                 commitReceived = true;
                                                 // Commit to one nonce, reveal another.
                                                 send(protocol::type::kPairReveal, {{"nonce", security::newPairingNonce()}});
                                             }
                                         },
                                         [](const Error&) {}});
                             send(protocol::type::kHello, {{"device_name", "mallory"}, {"app_version", "1"}});
                             send(protocol::type::kPairRequest,
                                  {{"device_name", "mallory"},
                                   {"commit", security::pairingCommitment(security::newPairingNonce())}});
                         });
    run(io, 900);
    EXPECT_TRUE(commitReceived);
    EXPECT_EQ(b.find(EngineEvent::Kind::PairingRequested), nullptr);   // the user was never asked to approve
    ASSERT_NE(b.find(EngineEvent::Kind::PairingFinished), nullptr);
    EXPECT_TRUE(b.find(EngineEvent::Kind::PairingFinished)->isError);
    EXPECT_FALSE(b.dm->isPaired(mallory.deviceId));
}

TEST(Pairing, PairRequestWithoutCommitmentIsRejected) {
    asio::io_context io;
    Node b(io, "bob");
    auto mallory = *security::generateIdentity("mallory");
    std::shared_ptr<transport::Connection> raw;
    bool closed = false;
    transport::connectTo(io, *transport::makeTlsContext(mallory, false),
                         {asio::ip::make_address_v4("127.0.0.1"), b.server->port()}, {},
                         [&](Result<std::shared_ptr<transport::Connection>> r) {
                             ASSERT_TRUE(r);
                             raw = *r;
                             raw->start({[](protocol::Frame) {}, [&](const Error&) { closed = true; }});
                             for (auto [type, payload] : {std::pair<std::string_view, nlohmann::json>{protocol::type::kHello, {{"device_name", "m"}, {"app_version", "1"}}},
                                                          {protocol::type::kPairRequest, {{"device_name", "m"}}}}) {
                                 raw->send(protocol::encodeJsonFrame(protocol::serialize(protocol::make(type, mallory.deviceId, payload))));
                             }
                         });
    run(io, 700);
    EXPECT_TRUE(closed);
    EXPECT_EQ(b.find(EngineEvent::Kind::PairingRequested), nullptr);
}
