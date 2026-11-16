#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "core/common/version.hpp"
#include "network/discovery/discovery.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::discovery;

namespace {
Announcement ann(std::string id, std::uint16_t port = 5000) {
    Announcement a;
    a.deviceId = std::move(id);
    a.deviceName = "dev";
    a.appVersion = "1.0";
    a.protocolVersion = kProtocolVersion;
    a.tcpPort = port;
    return a;
}
}  // namespace

TEST(DiscoveryMessage, RoundTrip) {
    auto parsed = parseAnnouncement(serialize(ann("abcd", 1234)));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->deviceId, "abcd");
    EXPECT_EQ(parsed->tcpPort, 1234);
    EXPECT_EQ(parsed->kind, Announcement::Kind::Announce);
}

TEST(DiscoveryMessage, RejectsInvalid) {
    for (const char* bad : {"", "x", "{}", R"({"app":"other"})",
         R"({"app":"lansync","protocol_version":1,"message_type":"announce","device_id":"zz","device_name":"n","app_version":"1","tcp_port":5})",
         R"({"app":"lansync","protocol_version":1,"message_type":"announce","device_id":"ab","device_name":"n","app_version":"1","tcp_port":0})",
         R"({"app":"lansync","protocol_version":1,"message_type":"announce","device_id":"ab","device_name":"n","app_version":"1","tcp_port":70000})",
         R"({"app":"lansync","protocol_version":1,"message_type":"hack","device_id":"ab","device_name":"n","app_version":"1","tcp_port":5})"}) {
        EXPECT_FALSE(parseAnnouncement(bad)) << bad;
    }
    EXPECT_FALSE(parseAnnouncement(std::string(kMaxDatagramBytes + 1, 'a')));
    auto longName = ann("ab");
    longName.deviceName = std::string(200, 'n');
    EXPECT_FALSE(parseAnnouncement(serialize(longName)));
}

TEST(DeviceRegistry, JoinUpdateExpire) {
    DeviceRegistry r;
    EXPECT_EQ(r.update(ann("aa"), "10.0.0.1", 1000), DeviceRegistry::Change::Joined);
    EXPECT_EQ(r.update(ann("aa"), "10.0.0.1", 2000), DeviceRegistry::Change::None);
    EXPECT_EQ(r.update(ann("aa", 6000), "10.0.0.1", 3000), DeviceRegistry::Change::Updated);
    EXPECT_EQ(r.update(ann("bb"), "10.0.0.2", 3000), DeviceRegistry::Change::Joined);
    EXPECT_EQ(r.list().size(), 2u);

    auto gone = r.expire(/*now*/ 20000, /*ttl*/ 15000);   // aa last seen 3000 -> expired
    EXPECT_EQ(gone.size(), 2u);
    EXPECT_TRUE(r.list().empty());
}

TEST(DeviceRegistry, RemoveAndFind) {
    DeviceRegistry r;
    r.update(ann("aa"), "10.0.0.1", 1);
    ASSERT_TRUE(r.find("aa"));
    EXPECT_EQ(r.find("aa")->address, "10.0.0.1");
    EXPECT_TRUE(r.remove("aa"));
    EXPECT_FALSE(r.remove("aa"));
    EXPECT_FALSE(r.find("aa"));
}

namespace {
std::uint16_t freeUdpPort() {
    asio::io_context io;
    asio::ip::udp::socket s(io, asio::ip::udp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
    return s.local_endpoint().port();
}
}  // namespace

// Two services on loopback with unicast targets: each must see the other, ignore
// itself, and notice the other leaving.
TEST(DiscoveryService, TwoInstancesFindEachOtherOverUdp) {
    asio::io_context io;
    auto clock = std::make_shared<SystemClock>();
    const auto portA = freeUdpPort();
    const auto portB = freeUdpPort();

    auto mk = [&](std::uint16_t own, std::uint16_t other, const char* id) {
        DiscoveryOptions o;
        o.port = own;
        o.bindAddress = "127.0.0.1";
        o.useMulticast = false;
        o.useBroadcast = false;
        o.unicastTargets = {{asio::ip::make_address_v4("127.0.0.1"), other}};
        o.announceInterval = std::chrono::milliseconds(100);
        o.ttl = std::chrono::milliseconds(2000);
        return std::make_shared<DiscoveryService>(io, o, ann(id, 4242), clock);
    };
    auto a = mk(portA, portB, "aaaa");
    auto b = mk(portB, portA, "bbbb");

    std::atomic<int> aSawB{0}, bSawA{0}, aSawSelf{0}, left{0};
    DiscoveryService::Callbacks ca;
    ca.onJoined = [&](const DiscoveredDevice& d) { d.deviceId == "bbbb" ? ++aSawB : ++aSawSelf; };
    ca.onLeft = [&](const DiscoveredDevice& d) { if (d.deviceId == "bbbb") ++left; };
    DiscoveryService::Callbacks cb;
    cb.onJoined = [&](const DiscoveredDevice& d) { if (d.deviceId == "aaaa") ++bSawA; };
    ASSERT_TRUE(a->start(ca));
    ASSERT_TRUE(b->start(cb));

    io.run_for(std::chrono::milliseconds(600));
    EXPECT_EQ(aSawB, 1);
    EXPECT_EQ(bSawA, 1);
    EXPECT_EQ(aSawSelf, 0);
    ASSERT_EQ(a->devices().size(), 1u);
    EXPECT_EQ(a->devices()[0].tcpPort, 4242);
    EXPECT_EQ(a->devices()[0].address, "127.0.0.1");

    b->stop();   // sends "bye"
    io.restart();
    io.run_for(std::chrono::milliseconds(300));
    EXPECT_EQ(left, 1);
    EXPECT_TRUE(a->devices().empty());
    a->stop();
}

TEST(DiscoveryService, IgnoresGarbageDatagrams) {
    asio::io_context io;
    const auto port = freeUdpPort();
    DiscoveryOptions o;
    o.port = port;
    o.bindAddress = "127.0.0.1";
    o.useMulticast = false;
    o.useBroadcast = false;
    o.announceInterval = std::chrono::milliseconds(5000);
    auto svc = std::make_shared<DiscoveryService>(io, o, ann("aaaa"), std::make_shared<SystemClock>());
    std::atomic<int> joined{0};
    DiscoveryService::Callbacks cb;
    cb.onJoined = [&](const DiscoveredDevice&) { ++joined; };
    ASSERT_TRUE(svc->start(cb));

    asio::ip::udp::socket s(io, asio::ip::udp::v4());
    const asio::ip::udp::endpoint target(asio::ip::make_address_v4("127.0.0.1"), port);
    s.send_to(asio::buffer(std::string("not json at all")), target);
    s.send_to(asio::buffer(std::string(2000, 'x')), target);
    s.send_to(asio::buffer(serialize(ann("cccc"))), target);   // valid
    io.run_for(std::chrono::milliseconds(300));
    EXPECT_EQ(joined, 1);
    svc->stop();
}
