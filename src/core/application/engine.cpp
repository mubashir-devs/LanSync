#include "core/application/engine.hpp"

#include "core/common/logging.hpp"
#include "core/common/version.hpp"
#include "network/security/identity.hpp"

namespace lansync::app {

namespace fs = std::filesystem;

Result<std::unique_ptr<Engine>> Engine::create(Application& application, EngineOptions options) {
    // Load settings first: the device name is part of our identity certificate subject.
    storage::SettingsRepository settingsRepo(application.database());
    auto stored = settingsRepo.all();
    if (!stored) return stored.error();
    Settings settings = Settings::fromMap(*stored);
    if (options.adjustSettings) {
        options.adjustSettings(settings);
        settings.sanitize();
    }
    const std::string name = settings.deviceName.empty() ? application.config().deviceName : settings.deviceName;

    auto identity = security::loadOrCreateIdentity(application.config().dataDir / "identity", name);
    if (!identity) return identity.error();

    std::unique_ptr<Engine> e(new Engine(application, std::move(options), std::move(*identity)));
    e->settings_ = std::move(settings);
    e->deviceName_ = name;
    return e;
}

Engine::Engine(Application& application, EngineOptions options, security::Identity identity)
    : application_(application), options_(std::move(options)), identity_(std::move(identity)) {}

Engine::~Engine() { stop(); }

void Engine::postOrThrow(std::function<void()> fn) {
    if (!running_) throw std::logic_error("Engine is not running");
    asio::post(io_, std::move(fn));
}

Result<void> Engine::start() {
    if (running_) return Error{ErrorCode::InvalidArgument, "engine already started"};
    auto& db = application_.database();
    auto clock = application_.sharedClock();

    deviceRepo_ = std::make_unique<storage::DeviceRepository>(db);
    settingsRepo_ = std::make_unique<storage::SettingsRepository>(db);
    workers_ = std::make_unique<WorkerPool>(2);
    versions_ = std::make_unique<versioning::VersionStore>(application_.config().dataDir / "versions", db, clock);

    auto clientCtx = transport::makeTlsContext(identity_, false);
    if (!clientCtx) return clientCtx.error();
    auto serverCtx = transport::makeTlsContext(identity_, true);
    if (!serverCtx) return serverCtx.error();

    EventHandler forward = [this](const EngineEvent& e) { emit(e); };

    device::DeviceManager::Deps dd{io_, identity_, deviceName_, *deviceRepo_, clock, *clientCtx, {}, forward};
    devices_ = std::make_unique<device::DeviceManager>(std::move(dd));

    sync::SyncDeps sd{io_, db, identity_.deviceId, application_.config().dataDir, clock, *devices_,
                      *versions_, *workers_, settings_, forward, options_.enableWatchers};
    sync_ = std::make_unique<sync::SyncManager>(std::move(sd));
    devices_->setSink(sync_.get());
    services_.emplace(Services{*devices_, *sync_, settings_, *this});

    // ---- TLS server (try the configured port, fall back to an ephemeral one)
    transport::ServerOptions so;
    so.bindAddress = options_.bindAddress;
    so.port = settings_.listenPort;
    so.allowPublicPeers = settings_.allowPublicNetworks;
    server_ = std::make_shared<transport::TlsServer>(io_, *serverCtx, so);
    auto onConn = [this](std::shared_ptr<transport::Connection> c) { devices_->onInboundConnection(std::move(c)); };
    if (auto r = server_->start(onConn); !r) {
        LS_WARN("{}; falling back to an ephemeral port", r.error().message);
        so.port = 0;
        server_ = std::make_shared<transport::TlsServer>(io_, *serverCtx, so);
        if (auto r2 = server_->start(onConn); !r2) return r2;
    }
    listenPort_ = server_->port();
    LS_INFO("listening on TCP {} as device {} ('{}')", listenPort_, identity_.deviceId.substr(0, 8), deviceName_);

    // ---- discovery
    if (settings_.discoveryEnabled) {
        discovery::DiscoveryOptions dopts;
        dopts.port = settings_.discoveryPort;
        dopts.bindAddress = options_.discoveryBindAddress;
        dopts.useMulticast = options_.discoveryMulticast;
        dopts.useBroadcast = options_.discoveryBroadcast;
        dopts.unicastTargets = options_.discoveryTargets;
        dopts.announceInterval = std::chrono::seconds(settings_.announceIntervalSec);
        dopts.ttl = options_.discoveryTtl;

        discovery::Announcement self;
        self.deviceId = identity_.deviceId;
        self.deviceName = deviceName_;
        self.appVersion = kAppVersion;
        self.protocolVersion = kProtocolVersion;
        self.tcpPort = listenPort_;
        discovery_ = std::make_shared<discovery::DiscoveryService>(io_, dopts, self, clock);

        discovery::DiscoveryService::Callbacks cb;
        cb.onJoined = [this](const discovery::DiscoveredDevice& d) { devices_->onDiscovered(d); };
        cb.onUpdated = cb.onJoined;
        cb.onLeft = [this](const discovery::DiscoveredDevice& d) { devices_->onLost(d.deviceId); };
        if (auto r = discovery_->start(std::move(cb)); !r) {
            // Not fatal: pairing by direct connection and existing pairs still work.
            LS_WARN("device discovery disabled: {}", r.error().message);
            emit({EngineEvent::Kind::Notification, "Discovery unavailable", r.error().message, true, {}});
            discovery_.reset();
        }
    }

    running_ = true;
    guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io_.get_executor());
    maintenanceTimer_ = std::make_unique<asio::steady_timer>(io_);

    // Bring the components up on the engine thread so every later call is on it.
    std::promise<Result<void>> started;
    auto fut = started.get_future();
    thread_ = std::jthread([this] { threadId_ = std::this_thread::get_id(); io_.run(); });
    asio::post(io_, [&] {
        devices_->start();
        started.set_value(sync_->start());
        maintenanceTick();
    });
    return fut.get();
}

void Engine::maintenanceTick() {
    if (!running_) return;
    const versioning::RetentionPolicy policy{settings_.versionMaxCount, settings_.versionMaxAgeDays};
    if (auto pruned = versions_->prune(policy); pruned && *pruned > 0) {
        LS_INFO("pruned {} old file versions", *pruned);
    }
    maintenanceTimer_->expires_after(std::chrono::minutes(10));
    maintenanceTimer_->async_wait([this](const boost::system::error_code& ec) {
        if (!ec) maintenanceTick();
    });
}

void Engine::stop() {
    if (!running_) return;
    if (!onEngineThread()) {
        std::promise<void> done;
        auto fut = done.get_future();
        asio::post(io_, [&] {
            maintenanceTimer_->cancel();
            sync_->shutdown();
            devices_->shutdown();
            if (discovery_) discovery_->stop();
            server_->stop();
            done.set_value();
        });
        fut.get();
    }
    running_ = false;
    guard_.reset();
    // Let queued close handlers run, then stop.
    io_.stop();
    if (thread_.joinable()) thread_.join();
}

void Engine::async(std::function<void(Services&)> fn) {
    if (!running_) return;
    asio::post(io_, [this, fn = std::move(fn)] {
        fn(services());
    });
}

void Engine::run(std::function<Result<void>(Services&)> fn, std::function<void(const Result<void>&)> done) {
    async([this, fn = std::move(fn), done = std::move(done)](Services& s) {
        auto r = fn(s);
        if (done) {
            done(r);
        } else if (!r) {
            emit({EngineEvent::Kind::Notification, "Action failed", r.error().message, true, {}});
        }
    });
}

void Engine::snapshotAsync(std::function<void(Snapshot)> cb) {
    async([cb = std::move(cb)](Services& s) { cb(s.engine.buildSnapshot()); });
}

void Engine::discoverNow() {
    async([this](Services&) {
        if (discovery_) discovery_->announceNow();
    });
}

Snapshot Engine::snapshot() {
    return call([](Services& s) { return s.engine.buildSnapshot(); });
}

Snapshot Engine::buildSnapshot() {
    Snapshot snap;
    snap.devices = devices_->deviceList();
    snap.folders = sync_->folderInfos();
    snap.transfers = sync_->transferInfos();
    snap.conflicts = sync_->conflictInfos(false);
    snap.history = sync_->historyInfos(200);
    snap.stats = sync_->stats();
    for (const auto& d : snap.devices) {
        if (d.pairingStatus == "paired") ++snap.stats.pairedDevices;
    }
    snap.settings = settings_;
    snap.deviceId = identity_.deviceId;
    snap.deviceName = deviceName_;
    snap.listenPort = listenPort_;
    return snap;
}

Result<void> Engine::updateSettings(Settings s) {
    s.sanitize();
    auto tx = storage::Transaction::begin(application_.database());
    if (!tx) return tx.error();
    for (const auto& [k, v] : s.toMap()) {
        if (auto r = settingsRepo_->set(k, v); !r) return r;
    }
    if (auto r = tx->commit(); !r) return r;

    const bool restartNeeded = s.listenPort != settings_.listenPort || s.discoveryPort != settings_.discoveryPort ||
                               s.discoveryEnabled != settings_.discoveryEnabled ||
                               s.allowPublicNetworks != settings_.allowPublicNetworks ||
                               s.announceIntervalSec != settings_.announceIntervalSec;
    const std::string newName = s.deviceName.empty() ? application_.config().deviceName : s.deviceName;
    settings_ = std::move(s);   // components hold a reference: chunk size, limits, retention apply live

    if (newName != deviceName_) {
        deviceName_ = newName;
        devices_->setDeviceName(newName);
        if (discovery_) {
            discovery_->setDeviceName(newName);
            discovery_->announceNow();
        }
    }
    log::setLevel(settings_.logLevel);
    maintenanceTick();   // apply new retention policy right away
    if (restartNeeded) {
        emit({EngineEvent::Kind::Notification, "Restart required",
              "Network settings take effect after LanSync is restarted.", false, {}});
    }
    return {};
}

}  // namespace lansync::app
