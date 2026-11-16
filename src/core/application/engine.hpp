#pragma once
// Engine: runs the whole networked core on ONE dedicated thread.
//
// It owns discovery, the TLS server, the DeviceManager and the SyncManager and
// is the only thing front-ends (CLI, Qt) talk to. It is Qt-free.
//
// Threading model
//   * All components live on the engine thread (a single-threaded io_context),
//     so they need no locking.
//   * Front-ends never touch components directly: they use async()/run() (never
//     blocks) or call() (blocks until done - for CLI/tests, NOT for a UI thread).
//   * Events are delivered on the engine thread; handlers must be quick and
//     thread-safe (the Qt adapter just queues a signal).

#include <boost/asio.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <thread>

#include "core/application/application.hpp"
#include "core/application/engine_types.hpp"
#include "core/common/worker_pool.hpp"
#include "core/device/device_manager.hpp"
#include "core/sync/sync_manager.hpp"
#include "core/versioning/version_store.hpp"
#include "network/discovery/discovery.hpp"
#include "network/transport/tls_server.hpp"

namespace lansync::app {

namespace asio = boost::asio;

struct EngineOptions {
    std::string bindAddress{"0.0.0.0"};         // TCP listen address
    std::string discoveryBindAddress{"0.0.0.0"};
    std::vector<asio::ip::udp::endpoint> discoveryTargets;   // extra unicast targets (tests, routed nets)
    bool discoveryMulticast{true};
    bool discoveryBroadcast{true};
    bool enableWatchers{true};
    std::chrono::milliseconds discoveryTtl{15000};
    // Applied on top of stored settings at startup (not persisted). Used by tests/CLI.
    std::function<void(Settings&)> adjustSettings;
};

class Engine;

struct Services {
    device::DeviceManager& devices;
    sync::SyncManager& sync;
    Settings& settings;
    Engine& engine;
};

class Engine {
public:
    static Result<std::unique_ptr<Engine>> create(Application& application, EngineOptions options = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts listening, discovery and syncing. Call once.
    Result<void> start();
    // Closes sessions, stops everything and joins the thread. Idempotent.
    void stop();

    // Set before start(). Called on the engine thread.
    void setEventHandler(EventHandler handler) { handler_ = std::move(handler); }

    // ---- non-blocking
    void async(std::function<void(Services&)> fn);
    // Runs `fn`; on failure reports it as an error Notification event (or to `done`).
    void run(std::function<Result<void>(Services&)> fn, std::function<void(const Result<void>&)> done = {});
    void snapshotAsync(std::function<void(Snapshot)> cb);

    // ---- blocking (CLI, tests). Never call from a UI thread. Runs inline if already on the engine thread.
    template <typename F>
    auto call(F&& f) -> std::invoke_result_t<F, Services&> {
        using R = std::invoke_result_t<F, Services&>;
        if (onEngineThread()) return f(services());
        std::promise<R> p;
        auto fut = p.get_future();
        postOrThrow([&] {
            try {
                if constexpr (std::is_void_v<R>) { f(services()); p.set_value(); }
                else p.set_value(f(services()));
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });
        return fut.get();
    }
    Snapshot snapshot();
    // Re-announce ourselves and re-probe the network now (the "Discover devices" button).
    void discoverNow();

    [[nodiscard]] std::uint16_t listenPort() const { return listenPort_; }
    [[nodiscard]] const std::string& deviceId() const { return identity_.deviceId; }
    [[nodiscard]] bool onEngineThread() const { return std::this_thread::get_id() == threadId_; }

    // Engine-thread operations (also reachable through Services).
    Snapshot buildSnapshot();
    Result<void> updateSettings(Settings s);

private:
    Engine(Application& application, EngineOptions options, security::Identity identity);
    Services& services() { return *services_; }
    void postOrThrow(std::function<void()> fn);
    void emit(const EngineEvent& e) { if (handler_) handler_(e); }
    void maintenanceTick();

    Application& application_;
    EngineOptions options_;
    security::Identity identity_;

    // Declaration order matters: members are destroyed in reverse. The worker
    // pool must be joined before the io_context goes away, and components that
    // use either must be destroyed first.
    asio::io_context io_;
    Settings settings_;
    std::string deviceName_;
    std::unique_ptr<storage::DeviceRepository> deviceRepo_;
    std::unique_ptr<storage::SettingsRepository> settingsRepo_;
    std::unique_ptr<WorkerPool> workers_;
    std::unique_ptr<versioning::VersionStore> versions_;
    std::unique_ptr<device::DeviceManager> devices_;
    std::unique_ptr<sync::SyncManager> sync_;
    std::shared_ptr<discovery::DiscoveryService> discovery_;
    std::shared_ptr<transport::TlsServer> server_;
    std::unique_ptr<asio::steady_timer> maintenanceTimer_;
    std::optional<Services> services_;

    EventHandler handler_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::jthread thread_;
    std::thread::id threadId_;
    std::uint16_t listenPort_{0};
    bool running_{false};
};

}  // namespace lansync::app
