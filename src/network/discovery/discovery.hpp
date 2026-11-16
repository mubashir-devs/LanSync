#pragma once
// UDP device discovery (multicast + subnet broadcast + optional unicast targets).
//
// Discovery messages are UNAUTHENTICATED hints. Being listed here grants nothing:
// trust is established only by pairing and certificate pinning. Datagrams are
// size-limited and strictly validated; the peer address is taken from the packet
// source, never from its contents.

#include <boost/asio.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/clock.hpp"
#include "core/common/result.hpp"

namespace lansync::discovery {

namespace asio = boost::asio;

inline constexpr std::size_t kMaxDatagramBytes = 1024;

struct Announcement {
    enum class Kind { Announce, Bye };
    Kind kind{Kind::Announce};
    std::string deviceId;
    std::string deviceName;
    std::string appVersion;
    int protocolVersion{0};
    std::uint16_t tcpPort{0};
};

std::string serialize(const Announcement& a);
Result<Announcement> parseAnnouncement(std::string_view datagram);

struct DiscoveredDevice {
    std::string deviceId;
    std::string deviceName;
    std::string appVersion;
    int protocolVersion{0};
    std::string address;
    std::uint16_t tcpPort{0};
    TimestampMs lastSeenMs{0};
};

// Pure bookkeeping of who is currently visible (unit-testable without sockets).
class DeviceRegistry {
public:
    enum class Change { None, Joined, Updated };

    Change update(const Announcement& a, const std::string& sourceAddress, TimestampMs now);
    bool remove(const std::string& deviceId);
    // Removes and returns entries not seen for longer than ttl.
    std::vector<DiscoveredDevice> expire(TimestampMs now, TimestampMs ttlMs);
    [[nodiscard]] std::vector<DiscoveredDevice> list() const;
    [[nodiscard]] std::optional<DiscoveredDevice> find(const std::string& deviceId) const;

private:
    std::map<std::string, DiscoveredDevice> devices_;
};

struct DiscoveryOptions {
    std::uint16_t port{45454};
    std::string bindAddress{"0.0.0.0"};
    std::string multicastGroup{"239.255.77.77"};
    bool useMulticast{true};
    bool useBroadcast{true};
    std::vector<asio::ip::udp::endpoint> unicastTargets;   // tests / routed networks
    std::chrono::milliseconds announceInterval{5000};
    std::chrono::milliseconds ttl{15000};
};

class DiscoveryService : public std::enable_shared_from_this<DiscoveryService> {
public:
    struct Callbacks {
        std::function<void(const DiscoveredDevice&)> onJoined;
        std::function<void(const DiscoveredDevice&)> onUpdated;
        std::function<void(const DiscoveredDevice&)> onLeft;
    };

    DiscoveryService(asio::io_context& io, DiscoveryOptions opts, Announcement self,
                     std::shared_ptr<const IClock> clock);

    Result<void> start(Callbacks callbacks);
    void stop();   // sends a "bye" and closes the socket
    void setTcpPort(std::uint16_t port) { self_.tcpPort = port; }
    void setDeviceName(std::string name) { self_.deviceName = std::move(name); }
    void announceNow();
    [[nodiscard]] std::vector<DiscoveredDevice> devices() const { return registry_.list(); }

private:
    void receiveLoop();
    void send(const std::string& datagram);
    void scheduleAnnounce();
    void scheduleExpiry();

    asio::io_context& io_;
    DiscoveryOptions opts_;
    Announcement self_;
    std::shared_ptr<const IClock> clock_;
    asio::ip::udp::socket socket_;
    asio::steady_timer announceTimer_;
    asio::steady_timer expiryTimer_;
    asio::ip::udp::endpoint sender_;
    std::array<char, kMaxDatagramBytes + 1> buf_{};
    DeviceRegistry registry_;
    Callbacks callbacks_;
    bool running_{false};
};

}  // namespace lansync::discovery
