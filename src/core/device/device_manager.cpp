#include "core/device/device_manager.hpp"

#include "core/common/crypto.hpp"
#include "core/common/logging.hpp"

namespace lansync::device {

namespace proto = lansync::protocol;
using storage::DeviceRow;

namespace {
constexpr auto kPairingTimeout = std::chrono::seconds(120);
constexpr auto kUntrustedGrace = std::chrono::seconds(30);
constexpr TimestampMs kRedialIntervalMs = 10'000;
}  // namespace

DeviceManager::DeviceManager(Deps deps) : deps_(std::move(deps)), reconnectTimer_(deps_.io) {}
DeviceManager::~DeviceManager() = default;

void DeviceManager::emit(EngineEvent::Kind k, std::string title, std::string text, bool err) {
    if (!deps_.emit) return;
    EngineEvent e;
    e.kind = k;
    e.title = std::move(title);
    e.text = std::move(text);
    e.isError = err;
    deps_.emit(e);
}

void DeviceManager::start() {
    running_ = true;
    reconnectTick();
}

void DeviceManager::shutdown() {
    running_ = false;
    reconnectTimer_.cancel();
    for (auto& [id, a] : attempts_) {
        if (a.timer) a.timer->cancel();
    }
    attempts_.clear();
    auto sessions = std::move(sessions_);
    auto unbound = std::move(unbound_);
    sessions_.clear();
    unbound_.clear();
    sink_ = nullptr;
    for (auto& [id, s] : sessions) s->close();
    for (auto& s : unbound) s->close();
}

// ---------------------------------------------------------------- discovery

void DeviceManager::onDiscovered(const discovery::DiscoveredDevice& d) {
    discovered_[d.deviceId] = d;
    auto row = deps_.devices.get(d.deviceId);
    if (row && *row && (*row)->pairingStatus == "paired") {
        (void)deps_.devices.setAddress(d.deviceId, d.address, d.tcpPort, d.lastSeenMs);
    }
    emit(EngineEvent::Kind::DevicesChanged);
}

void DeviceManager::onLost(const std::string& deviceId) {
    discovered_.erase(deviceId);
    emit(EngineEvent::Kind::DevicesChanged);
}

// ---------------------------------------------------------------- sessions

std::shared_ptr<PeerSession> DeviceManager::findShared(const PeerSession& s) const {
    for (const auto& u : unbound_) {
        if (u.get() == &s) return u;
    }
    const auto it = sessions_.find(s.peerId());
    if (it != sessions_.end() && it->second.get() == &s) return it->second;
    return nullptr;
}

std::shared_ptr<PeerSession> DeviceManager::attachSession(std::shared_ptr<transport::Connection> conn, bool outbound) {
    auto session = std::make_shared<PeerSession>(std::move(conn), deps_.identity.deviceId,
                                                 deps_.deviceName, outbound, *this, deps_.io);
    unbound_.insert(session);
    session->start();

    // A connection that never says hello or never pairs must not linger.
    auto timer = std::make_shared<asio::steady_timer>(deps_.io, kUntrustedGrace);
    std::weak_ptr<PeerSession> weak = session;
    timer->async_wait([timer, weak, this](const boost::system::error_code& ec) {
        if (ec || !running_) return;
        auto s = weak.lock();
        if (s && s->isOpen() && !s->trusted() && !attempts_.count(s->peerId())) {
            LS_DEBUG("closing idle untrusted session from {}", s->peerId().substr(0, 8));
            s->close(Error{ErrorCode::Timeout, "no pairing request"});
        }
    });
    return session;
}

void DeviceManager::onInboundConnection(std::shared_ptr<transport::Connection> conn) {
    if (!running_) {
        conn->close();
        return;
    }
    attachSession(std::move(conn), false);
}

void DeviceManager::markTrusted(const std::shared_ptr<PeerSession>& s) {
    s->setTrusted(true);
    unbound_.erase(s);
    if (auto old = sessions_.find(s->peerId()); old != sessions_.end() && old->second != s) {
        auto prev = old->second;
        sessions_.erase(old);
        prev->setTrusted(false);
        prev->close(Error{ErrorCode::Cancelled, "replaced by newer session"});
    }
    sessions_[s->peerId()] = s;
    (void)deps_.devices.setAddress(s->peerId(), s->address(),
                                   discovered_.count(s->peerId()) ? discovered_[s->peerId()].tcpPort
                                                                  : s->remotePort(),
                                   deps_.clock->nowMs());
    emit(EngineEvent::Kind::DevicesChanged);
    if (sink_) sink_->onPeerReady(*s);
}

void DeviceManager::onSessionHello(PeerSession& s) {
    auto shared = findShared(s);
    if (!shared) return;
    auto row = deps_.devices.get(s.peerId());

    if (row && *row && (*row)->pairingStatus == "paired") {
        if ((*row)->fingerprint.value_or("") != s.peerFingerprint()) {
            // Same id, different certificate: cannot happen without the private
            // key unless the pin was tampered with. Refuse loudly.
            LS_ERROR("paired device {} presented an unexpected certificate; refusing",
                     s.peerId().substr(0, 8));
            emit(EngineEvent::Kind::Notification, "Security warning",
                 "Device '" + (*row)->name + "' presented a different certificate and was refused.", true);
            s.send(proto::type::kError, {{"code", "certificate_mismatch"}});
            s.close(Error{ErrorCode::SecurityError, "certificate mismatch"});
            return;
        }
        if ((*row)->name != s.peerName()) {
            DeviceRow upd = **row;
            upd.name = s.peerName();
            (void)deps_.devices.upsert(upd);
        }
        // An in-flight pairing attempt with an already-paired device is moot.
        markTrusted(shared);
        return;
    }

    // Not paired. Outbound sessions exist only because the user asked to pair.
    if (s.outbound()) {
        auto it = attempts_.find(s.peerId());
        if (it != attempts_.end() && it->second.session == shared) {
            beginAttempt(s, /*incoming=*/false);
            s.send(proto::type::kPairRequest, {{"device_name", deps_.deviceName},
                                               {"commit", security::pairingCommitment(attempts_[s.peerId()].myNonce)}});
        }
    }
    // Inbound: wait for pair_request (see onSessionMessage).
}

void DeviceManager::beginAttempt(PeerSession& s, bool incoming) {
    auto& a = attempts_[s.peerId()];
    a.session = findShared(s);
    a.incoming = incoming;
    a.peerName = s.peerName();
    a.myNonce = security::newPairingNonce();
    a.code.clear();   // computed only after both nonces are revealed (commit-then-reveal)
    a.timer = std::make_unique<asio::steady_timer>(deps_.io, kPairingTimeout);
    const std::string id = s.peerId();
    a.timer->async_wait([this, id](const boost::system::error_code& ec) {
        if (ec || !running_) return;
        finishAttempt(id, "Pairing timed out", true);
    });
    emit(EngineEvent::Kind::DevicesChanged);
}

void DeviceManager::sendReveal(Attempt& a) {
    if (a.revealSent || !a.session) return;
    a.revealSent = true;
    a.session->send(proto::type::kPairReveal, {{"nonce", a.myNonce}});
}

void DeviceManager::failVerification(const std::string& id, const std::string& why) {
    LS_WARN("pairing verification failed with {}: {}", id.substr(0, 8), why);
    finishAttempt(id, "Pairing verification failed: " + why, true);
}

void DeviceManager::onReveal(PeerSession& s, Attempt& a, const std::string& nonce) {
    if (a.peerCommit.empty() || !a.peerNonce.empty()) return failVerification(s.peerId(), "unexpected reveal");
    if (nonce.size() != 32 || !crypto::isLowerHex(nonce) || security::pairingCommitment(nonce) != a.peerCommit) {
        return failVerification(s.peerId(), "revealed value does not match the commitment");
    }
    a.peerNonce = nonce;
    if (a.incoming) sendReveal(a);   // the initiator reveals first, we answer
    a.code = security::pairingCode(deps_.identity.fingerprint, a.myNonce, s.peerFingerprint(), a.peerNonce);

    EngineEvent e;
    e.kind = EngineEvent::Kind::PairingRequested;
    e.pairing = {s.peerId(), a.peerName, a.code, a.incoming};
    if (deps_.emit) deps_.emit(e);
    emit(EngineEvent::Kind::DevicesChanged);
}

void DeviceManager::onSessionMessage(PeerSession& s, const proto::Message& m) {
    if (m.type == proto::type::kPairRequest) {
        if (s.trusted() || s.outbound()) {
            s.close(Error{ErrorCode::ProtocolError, "unexpected pair_request"});
            return;
        }
        if (auto it = attempts_.find(s.peerId()); it != attempts_.end()) {
            // Both users pressed "Pair" at the same moment. Deterministic tie-break:
            // the attempt initiated by the smaller device id survives.
            if (it->second.incoming || it->second.state.localDecided()) {
                s.close(Error{ErrorCode::ProtocolError, "duplicate pair_request"});
                return;
            }
            if (deps_.identity.deviceId < s.peerId()) {
                s.close(Error{ErrorCode::Cancelled, "simultaneous pairing: keeping our own request"});
                return;
            }
            auto mine = it->second.session;
            const auto name = it->second.peerName;
            if (it->second.timer) it->second.timer->cancel();
            attempts_.erase(it);
            EngineEvent e;
            e.kind = EngineEvent::Kind::PairingFinished;
            e.title = name;
            e.text = "superseded by the other device's request";
            e.pairing.deviceId = s.peerId();
            if (deps_.emit) deps_.emit(e);
            if (mine) mine->close(Error{ErrorCode::Cancelled, "superseded"});
        }
        const auto commit = m.payload.value("commit", std::string{});
        if (commit.size() != 64 || !crypto::isLowerHex(commit)) {
            s.close(Error{ErrorCode::ProtocolError, "pair_request without a valid commitment"});
            return;
        }
        beginAttempt(s, /*incoming=*/true);
        auto& a = attempts_[s.peerId()];
        a.peerCommit = commit;
        s.send(proto::type::kPairCommit, {{"commit", security::pairingCommitment(a.myNonce)}});
        return;
    }
    if (m.type == proto::type::kPairCommit || m.type == proto::type::kPairReveal) {
        auto it = attempts_.find(s.peerId());
        if (it == attempts_.end() || it->second.session.get() != &s) return;
        Attempt& a = it->second;
        if (m.type == proto::type::kPairCommit) {
            const auto commit = m.payload.value("commit", std::string{});
            if (a.incoming || !a.peerCommit.empty() || commit.size() != 64 || !crypto::isLowerHex(commit)) {
                return failVerification(s.peerId(), "unexpected commitment");
            }
            a.peerCommit = commit;
            sendReveal(a);   // both commitments exist now: safe to reveal
        } else {
            onReveal(s, a, m.payload.value("nonce", std::string{}));
        }
        return;
    }
    if (m.type == proto::type::kPairDecision) {
        auto it = attempts_.find(s.peerId());
        if (it == attempts_.end() || it->second.session.get() != &s) return;
        const bool accepted = m.payload.value("accepted", false);
        it->second.state.onRemoteDecision(accepted);
        evaluate(s.peerId());
        return;
    }
    if (m.type == proto::type::kError) {
        const auto code = m.payload.value("code", std::string{"unknown"});
        LS_WARN("peer {} reported error '{}'", s.peerId().substr(0, 8), code);
        if (code == "not_paired" || code == "certificate_mismatch") {
            emit(EngineEvent::Kind::Notification, "Peer refused connection",
                 "'" + s.peerName() + "' no longer trusts this device (" + code + ").", true);
        }
        return;
    }
    if (s.trusted() && sink_) sink_->onPeerMessage(s, m);
}

void DeviceManager::onSessionChunk(PeerSession& s, std::uint32_t streamId, std::string_view data) {
    if (s.trusted() && sink_) sink_->onPeerChunk(s, streamId, data);
}

void DeviceManager::onSessionClosed(PeerSession& s, const Error& reason) {
    const std::string id = s.peerId();
    LS_DEBUG("session with {} closed: {}", id.substr(0, 8), reason.message);
    bool wasTrusted = false;
    for (auto it = unbound_.begin(); it != unbound_.end();) {
        it = (it->get() == &s) ? unbound_.erase(it) : std::next(it);
    }
    if (auto it = sessions_.find(id); it != sessions_.end() && it->second.get() == &s) {
        sessions_.erase(it);
        wasTrusted = true;
    }
    if (auto it = attempts_.find(id); it != attempts_.end() && it->second.session.get() == &s) {
        // Our outgoing attempt died while the peer's own request is arriving on another
        // session (both users pressed "Pair"): that is the tie-break, not an error.
        bool peerIsInitiatingToo = false;
        if (!it->second.incoming && !it->second.state.localDecided()) {
            for (const auto& u : unbound_) {
                if (u.get() != &s && u->peerId() == id && !u->outbound()) peerIsInitiatingToo = true;
            }
        }
        if (peerIsInitiatingToo) {
            if (it->second.timer) it->second.timer->cancel();
            attempts_.erase(it);
        } else {
            finishAttempt(id, "Connection lost during pairing", true);
        }
    }
    if (wasTrusted && sink_) sink_->onPeerClosed(id);
    if (running_) emit(EngineEvent::Kind::DevicesChanged);
}

// ---------------------------------------------------------------- pairing

Result<void> DeviceManager::startPairing(const std::string& deviceId) {
    if (deviceId == deps_.identity.deviceId) return Error{ErrorCode::InvalidArgument, "cannot pair with self"};
    if (sessions_.count(deviceId)) return Error{ErrorCode::AlreadyExists, "already paired and connected"};
    if (attempts_.count(deviceId) || dialing_.count(deviceId)) {
        return Error{ErrorCode::Busy, "pairing already in progress"};
    }
    const auto it = discovered_.find(deviceId);
    if (it == discovered_.end()) return Error{ErrorCode::NotFound, "device is not currently visible on the network"};

    boost::system::error_code ec;
    const auto addr = asio::ip::make_address(it->second.address, ec);
    if (ec) return Error{ErrorCode::InvalidArgument, "bad device address"};

    dialing_.insert(deviceId);
    const std::string name = it->second.deviceName;
    transport::connectTo(
        deps_.io, deps_.clientCtx, {addr, it->second.tcpPort}, deps_.limits,
        [this, deviceId, name](Result<std::shared_ptr<transport::Connection>> r) {
            dialing_.erase(deviceId);
            if (!running_) return;
            if (!r) {
                emit(EngineEvent::Kind::Notification, "Cannot reach device", r.error().message, true);
                return;
            }
            if ((*r)->peer().deviceId != deviceId) {
                (*r)->close();
                emit(EngineEvent::Kind::Notification, "Pairing refused",
                     "The device at that address has a different identity than advertised.", true);
                return;
            }
            // Register the attempt before the session starts so hello finds it.
            attempts_[deviceId].incoming = false;
            attempts_[deviceId].session = attachSession(*r, /*outbound=*/true);   // exactly OUR session
        });
    return {};
}

void DeviceManager::decidePairing(const std::string& deviceId, bool accepted) {
    auto it = attempts_.find(deviceId);
    if (it == attempts_.end() || !it->second.session) return;
    if (it->second.code.empty()) return;   // nothing to approve until the code exists
    it->second.state.onLocalDecision(accepted);
    it->second.session->send(proto::type::kPairDecision, {{"accepted", accepted}});
    evaluate(deviceId);
}

void DeviceManager::evaluate(const std::string& id) {
    auto it = attempts_.find(id);
    if (it == attempts_.end()) return;
    switch (it->second.state.phase()) {
        case PairingState::Phase::Rejected:
            finishAttempt(id, "Pairing was rejected", true);
            break;
        case PairingState::Phase::Paired: {
            auto session = it->second.session;
            DeviceRow row;
            row.id = id;
            row.name = session->peerName();
            row.publicKeyPem = session->peerPublicKeyPem();
            row.fingerprint = session->peerFingerprint();
            row.address = session->address();
            auto disc = discovered_.find(id);
            if (disc != discovered_.end()) row.port = disc->second.tcpPort;
            row.pairingStatus = "paired";
            row.lastSeenAt = deps_.clock->nowMs();
            row.createdAt = deps_.clock->nowMs();
            if (auto r = deps_.devices.upsert(row); !r) {
                finishAttempt(id, "Could not save pairing: " + r.error().message, true);
                return;
            }
            if (it->second.timer) it->second.timer->cancel();
            const auto name = row.name;
            attempts_.erase(it);
            markTrusted(session);
            emit(EngineEvent::Kind::PairingFinished, name, "paired");
            break;
        }
        default:
            break;
    }
}

void DeviceManager::finishAttempt(const std::string& id, const std::string& outcome, bool isError) {
    auto it = attempts_.find(id);
    if (it == attempts_.end()) return;
    auto session = it->second.session;
    const auto name = it->second.peerName;
    if (it->second.timer) it->second.timer->cancel();
    attempts_.erase(it);
    EngineEvent e;
    e.kind = EngineEvent::Kind::PairingFinished;
    e.title = name;
    e.text = outcome;
    e.isError = isError;
    e.pairing.deviceId = id;
    if (deps_.emit) deps_.emit(e);
    if (session && session->isOpen() && !session->trusted()) session->close();
    emit(EngineEvent::Kind::DevicesChanged);
}

Result<void> DeviceManager::unpair(const std::string& deviceId) {
    if (auto r = deps_.devices.revoke(deviceId); !r) return r;
    if (auto it = sessions_.find(deviceId); it != sessions_.end()) {
        auto s = it->second;
        s->setTrusted(false);
        sessions_.erase(it);
        s->close(Error{ErrorCode::Cancelled, "unpaired"});
        if (sink_) sink_->onPeerClosed(deviceId);
    }
    emit(EngineEvent::Kind::DevicesChanged);
    return {};
}

Result<void> DeviceManager::forget(const std::string& deviceId) {
    if (isPaired(deviceId)) return Error{ErrorCode::InvalidArgument, "unpair the device before removing it"};
    if (auto r = deps_.devices.remove(deviceId); !r) return r;
    emit(EngineEvent::Kind::DevicesChanged);
    return {};
}

// ---------------------------------------------------------------- reconnect

void DeviceManager::connectNow(const std::string& deviceId) {
    lastDial_.erase(deviceId);
    reconnectTick();
}

void DeviceManager::reconnectTick() {
    if (!running_) return;
    auto rows = deps_.devices.list();
    const auto now = deps_.clock->nowMs();
    if (rows) {
        for (const auto& d : *rows) {
            if (d.pairingStatus != "paired" || sessions_.count(d.id) || dialing_.count(d.id)) continue;
            // Only the lexicographically smaller id dials, so both sides never
            // connect simultaneously and replace each other's sessions.
            if (!(deps_.identity.deviceId < d.id)) continue;
            const auto disc = discovered_.find(d.id);
            if (disc == discovered_.end()) continue;
            if (const auto last = lastDial_.find(d.id);
                last != lastDial_.end() && now - last->second < kRedialIntervalMs) continue;
            lastDial_[d.id] = now;

            boost::system::error_code ec;
            const auto addr = asio::ip::make_address(disc->second.address, ec);
            if (ec) continue;
            dialing_.insert(d.id);
            const std::string id = d.id;
            transport::connectTo(
                deps_.io, deps_.clientCtx, {addr, disc->second.tcpPort}, deps_.limits,
                [this, id](Result<std::shared_ptr<transport::Connection>> r) {
                    dialing_.erase(id);
                    if (!running_) return;
                    if (!r) {
                        LS_DEBUG("dial {} failed: {}", id.substr(0, 8), r.error().message);
                        return;
                    }
                    if ((*r)->peer().deviceId != id) {
                        (*r)->close();
                        return;
                    }
                    attachSession(*r, true);
                });
        }
    }
    reconnectTimer_.expires_after(std::chrono::seconds(3));
    reconnectTimer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) reconnectTick();
    });
}

// ---------------------------------------------------------------- queries

PeerSession* DeviceManager::trustedSession(const std::string& id) const {
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

bool DeviceManager::isPaired(const std::string& id) const {
    auto row = deps_.devices.get(id);
    return row && *row && (*row)->pairingStatus == "paired";
}

std::string DeviceManager::deviceName(const std::string& id) const {
    if (id == deps_.identity.deviceId) return deps_.deviceName;
    if (auto row = deps_.devices.get(id); row && *row) return (*row)->name;
    if (auto it = discovered_.find(id); it != discovered_.end()) return it->second.deviceName;
    return id.substr(0, 8);
}

std::vector<DeviceInfo> DeviceManager::deviceList() const {
    std::map<std::string, DeviceInfo> out;
    if (auto rows = deps_.devices.list()) {
        for (const auto& d : *rows) {
            DeviceInfo i;
            i.id = d.id;
            i.name = d.name;
            i.address = d.address.value_or("");
            i.port = static_cast<int>(d.port.value_or(0));
            i.pairingStatus = d.pairingStatus;
            i.lastSeenMs = d.lastSeenAt.value_or(0);
            i.fingerprint = d.fingerprint.value_or("");
            out[d.id] = std::move(i);
        }
    }
    for (const auto& [id, d] : discovered_) {
        auto& i = out[id];
        i.id = id;
        if (i.name.empty() || i.pairingStatus.empty() || i.pairingStatus == "unpaired") i.name = d.deviceName;
        if (i.pairingStatus.empty()) i.pairingStatus = "unpaired";
        i.address = d.address;
        i.port = d.tcpPort;
        i.lastSeenMs = d.lastSeenMs;
        i.discovered = true;
        i.appVersion = d.appVersion;
    }
    for (auto& [id, i] : out) {
        i.connected = sessions_.count(id) > 0;
        if (attempts_.count(id)) i.pairingStatus = "pending";
    }
    std::vector<DeviceInfo> list;
    for (auto& [id, i] : out) list.push_back(std::move(i));
    return list;
}

}  // namespace lansync::device
