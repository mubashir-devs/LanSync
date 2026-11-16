#include "network/discovery/discovery.hpp"

#include <nlohmann/json.hpp>

#include "core/common/crypto.hpp"
#include "core/common/logging.hpp"
#include "platform/platform.hpp"

namespace lansync::discovery {

using nlohmann::json;

// ------------------------------------------------------------------ messages

std::string serialize(const Announcement& a) {
    json j;
    j["app"] = "lansync";
    j["protocol_version"] = a.protocolVersion;
    j["message_type"] = a.kind == Announcement::Kind::Bye ? "bye" : "announce";
    j["device_id"] = a.deviceId;
    j["device_name"] = a.deviceName;
    j["app_version"] = a.appVersion;
    j["tcp_port"] = a.tcpPort;
    return j.dump();
}

Result<Announcement> parseAnnouncement(std::string_view d) {
    auto bad = [](const char* why) { return Error{ErrorCode::ProtocolError, why}; };
    if (d.empty() || d.size() > kMaxDatagramBytes) return bad("bad datagram size");
    json j = json::parse(d.begin(), d.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return bad("not json");
    if (j.value("app", std::string{}) != "lansync") return bad("not a lansync datagram");

    Announcement a;
    const auto type = j.value("message_type", std::string{});
    if (type == "announce") a.kind = Announcement::Kind::Announce;
    else if (type == "bye") a.kind = Announcement::Kind::Bye;
    else return bad("unknown message_type");

    if (!j.contains("protocol_version") || !j["protocol_version"].is_number_integer()) {
        return bad("bad protocol_version");
    }
    a.protocolVersion = j["protocol_version"].get<int>();

    for (auto [key, out, max] : {std::tuple<const char*, std::string*, std::size_t>{"device_id", &a.deviceId, 64},
                                 {"device_name", &a.deviceName, 128},
                                 {"app_version", &a.appVersion, 32}}) {
        if (!j.contains(key) || !j[key].is_string()) return bad("missing string field");
        *out = j[key].get<std::string>();
        if (out->size() > max) return bad("field too long");
    }
    if (a.deviceId.empty() || !crypto::isLowerHex(a.deviceId)) return bad("bad device_id");
    if (!j.contains("tcp_port") || !j["tcp_port"].is_number_integer()) return bad("bad tcp_port");
    const auto port = j["tcp_port"].get<long long>();
    if (port < 1 || port > 65535) return bad("tcp_port out of range");
    a.tcpPort = static_cast<std::uint16_t>(port);
    return a;
}

// ------------------------------------------------------------------ registry

DeviceRegistry::Change DeviceRegistry::update(const Announcement& a, const std::string& source,
                                              TimestampMs now) {
    auto it = devices_.find(a.deviceId);
    DiscoveredDevice d{a.deviceId, a.deviceName, a.appVersion, a.protocolVersion, source,
                       a.tcpPort, now};
    if (it == devices_.end()) {
        devices_.emplace(a.deviceId, std::move(d));
        return Change::Joined;
    }
    const bool changed = it->second.address != d.address || it->second.tcpPort != d.tcpPort ||
                         it->second.deviceName != d.deviceName;
    it->second = std::move(d);
    return changed ? Change::Updated : Change::None;
}

bool DeviceRegistry::remove(const std::string& id) { return devices_.erase(id) > 0; }

std::vector<DiscoveredDevice> DeviceRegistry::expire(TimestampMs now, TimestampMs ttl) {
    std::vector<DiscoveredDevice> gone;
    for (auto it = devices_.begin(); it != devices_.end();) {
        if (now - it->second.lastSeenMs > ttl) {
            gone.push_back(std::move(it->second));
            it = devices_.erase(it);
        } else {
            ++it;
        }
    }
    return gone;
}

std::vector<DiscoveredDevice> DeviceRegistry::list() const {
    std::vector<DiscoveredDevice> out;
    for (const auto& [id, d] : devices_) out.push_back(d);
    return out;
}

std::optional<DiscoveredDevice> DeviceRegistry::find(const std::string& id) const {
    const auto it = devices_.find(id);
    if (it == devices_.end()) return std::nullopt;
    return it->second;
}

// ------------------------------------------------------------------ service

DiscoveryService::DiscoveryService(asio::io_context& io, DiscoveryOptions opts, Announcement self,
                                   std::shared_ptr<const IClock> clock)
    : io_(io),
      opts_(std::move(opts)),
      self_(std::move(self)),
      clock_(std::move(clock)),
      socket_(io),
      announceTimer_(io),
      expiryTimer_(io) {}

Result<void> DiscoveryService::start(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    boost::system::error_code ec;
    const auto bindAddr = asio::ip::make_address_v4(opts_.bindAddress, ec);
    if (ec) return Error{ErrorCode::InvalidArgument, "bad discovery bind address"};

    socket_.open(asio::ip::udp::v4(), ec);
    if (!ec) socket_.set_option(asio::socket_base::reuse_address(true), ec);
    if (!ec) socket_.set_option(asio::socket_base::broadcast(true), ec);
    if (!ec) socket_.bind(asio::ip::udp::endpoint(bindAddr, opts_.port), ec);
    if (ec) {
        return Error{ErrorCode::IoError, "discovery bind failed on UDP port " +
                                             std::to_string(opts_.port) + ": " + ec.message()};
    }

    if (opts_.useMulticast) {
        const auto group = asio::ip::make_address_v4(opts_.multicastGroup, ec);
        if (ec) return Error{ErrorCode::InvalidArgument, "bad multicast group"};
        const auto ifaces = platform::networkInterfaces();
        bool joined = false;
        for (const auto& ni : ifaces) {
            if (ni.loopback || !ni.multicast) continue;
            boost::system::error_code jec;
            socket_.set_option(asio::ip::multicast::join_group(group, asio::ip::make_address_v4(ni.ipv4)), jec);
            if (jec) LS_DEBUG("multicast join on {} failed: {}", ni.name, jec.message());
            else joined = true;
        }
        if (!joined) {   // no enumeration (or all failed): let the OS pick
            boost::system::error_code jec;
            socket_.set_option(asio::ip::multicast::join_group(group), jec);
            if (jec) LS_DEBUG("default multicast join failed: {}", jec.message());
        }
    }

    running_ = true;
    receiveLoop();
    announceNow();
    scheduleAnnounce();
    scheduleExpiry();
    return {};
}

void DiscoveryService::stop() {
    if (!running_) return;
    Announcement bye = self_;
    bye.kind = Announcement::Kind::Bye;
    send(serialize(bye));
    running_ = false;
    announceTimer_.cancel();
    expiryTimer_.cancel();
    boost::system::error_code ignored;
    socket_.close(ignored);
}

void DiscoveryService::announceNow() {
    if (!running_) return;
    send(serialize(self_));
}

void DiscoveryService::send(const std::string& datagram) {
    boost::system::error_code ec;
    for (const auto& target : opts_.unicastTargets) socket_.send_to(asio::buffer(datagram), target, 0, ec);

    const auto ifaces = platform::networkInterfaces();
    if (opts_.useMulticast) {
        const asio::ip::udp::endpoint group(asio::ip::make_address_v4(opts_.multicastGroup), opts_.port);
        bool any = false;
        for (const auto& ni : ifaces) {
            if (ni.loopback || !ni.multicast) continue;
            socket_.set_option(asio::ip::multicast::outbound_interface(asio::ip::make_address_v4(ni.ipv4)), ec);
            socket_.send_to(asio::buffer(datagram), group, 0, ec);
            any = true;
        }
        if (!any) socket_.send_to(asio::buffer(datagram), group, 0, ec);
    }
    if (opts_.useBroadcast) {
        bool any = false;
        for (const auto& ni : ifaces) {
            if (ni.loopback || ni.broadcast.empty()) continue;
            socket_.send_to(asio::buffer(datagram),
                            {asio::ip::make_address_v4(ni.broadcast), opts_.port}, 0, ec);
            any = true;
        }
        if (!any) {
            socket_.send_to(asio::buffer(datagram),
                            {asio::ip::address_v4::broadcast(), opts_.port}, 0, ec);
        }
    }
}

void DiscoveryService::receiveLoop() {
    if (!running_) return;
    auto self = shared_from_this();
    socket_.async_receive_from(
        asio::buffer(buf_), sender_, [self](const boost::system::error_code& ec, std::size_t n) {
            if (!self->running_) return;
            if (!ec && n > 0 && n <= kMaxDatagramBytes) {
                auto a = parseAnnouncement(std::string_view(self->buf_.data(), n));
                if (!a) {
                    LS_DEBUG("ignoring invalid discovery datagram from {}: {}",
                             self->sender_.address().to_string(), a.error().message);
                } else if (a->deviceId != self->self_.deviceId) {   // our own multicast echo
                    const auto src = self->sender_.address().to_string();
                    if (a->kind == Announcement::Kind::Bye) {
                        if (auto d = self->registry_.find(a->deviceId);
                            d && self->registry_.remove(a->deviceId) && self->callbacks_.onLeft) {
                            self->callbacks_.onLeft(*d);
                        }
                    } else if (a->protocolVersion == self->self_.protocolVersion) {
                        const auto change = self->registry_.update(*a, src, self->clock_->nowMs());
                        const auto d = self->registry_.find(a->deviceId);
                        if (change == DeviceRegistry::Change::Joined && self->callbacks_.onJoined) {
                            self->callbacks_.onJoined(*d);
                            self->announceNow();   // let the newcomer see us quickly
                        } else if (change == DeviceRegistry::Change::Updated &&
                                   self->callbacks_.onUpdated) {
                            self->callbacks_.onUpdated(*d);
                        }
                    }
                }
            }
            self->receiveLoop();
        });
}

void DiscoveryService::scheduleAnnounce() {
    auto self = shared_from_this();
    announceTimer_.expires_after(opts_.announceInterval);
    announceTimer_.async_wait([self](const boost::system::error_code& ec) {
        if (ec || !self->running_) return;
        self->announceNow();
        self->scheduleAnnounce();
    });
}

void DiscoveryService::scheduleExpiry() {
    auto self = shared_from_this();
    expiryTimer_.expires_after(opts_.ttl / 3);
    expiryTimer_.async_wait([self](const boost::system::error_code& ec) {
        if (ec || !self->running_) return;
        for (const auto& d : self->registry_.expire(self->clock_->nowMs(), self->opts_.ttl.count())) {
            if (self->callbacks_.onLeft) self->callbacks_.onLeft(d);
        }
        self->scheduleExpiry();
    });
}

}  // namespace lansync::discovery
