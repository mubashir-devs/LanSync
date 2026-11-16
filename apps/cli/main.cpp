// lansync-cli: headless front-end over the core library.
// Proves the core is usable without Qt, and drives the multi-process system tests.

#include <csignal>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/application/application.hpp"
#include "core/application/engine.hpp"
#include "core/common/logging.hpp"
#include "core/common/version.hpp"

namespace {

std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }

void printUsage() {
    std::cout <<
        "Usage: lansync-cli [options] <command>\n"
        "\n"
        "Commands:\n"
        "  version      Print version information\n"
        "  info         Initialise the data directory/database and print status\n"
        "  daemon       Run LanSync headless (discovery, pairing, sync) until interrupted\n"
        "\n"
        "Common options:\n"
        "  --data-dir <path>       Override the data directory\n"
        "  --device-name <name>    Override the device name\n"
        "  --log-level <level>     trace|debug|info|warn|error|critical|off (default: warn)\n"
        "  -h, --help              Show this help\n"
        "\n"
        "Daemon options:\n"
        "  --listen-port <n>       TCP port (0 = ephemeral)\n"
        "  --discovery-port <n>    UDP discovery port\n"
        "  --discovery-target <host:port>  Extra unicast discovery target (repeatable)\n"
        "  --no-multicast          Disable multicast/broadcast discovery (use with --discovery-target)\n"
        "  --bind <address>        Bind TCP/UDP to this address (default 0.0.0.0)\n"
        "  --pair-with <name>      Pair with the first discovered device of this name\n"
        "  --share <name>=<path>   After pairing, share <path> with the paired device as <name> (repeatable)\n"
        "  --auto-approve-pairing  TESTING ONLY: accept pairing without comparing the code\n"
        "\n"
        "Daemon output (stdout, one event per line): READY, PAIRING, PAIRED, SHARED, FOLDER, ERROR\n";
}

struct Options {
    lansync::app::AppConfig config = lansync::app::AppConfig::defaults();
    std::string command;
    int listenPort = -1;
    int discoveryPort = -1;
    std::vector<std::string> discoveryTargets;
    bool noMulticast = false;
    std::string bind = "0.0.0.0";
    std::string pairWith;
    std::vector<std::pair<std::string, std::string>> shares;
    bool autoApprove = false;
};

bool parseHostPort(const std::string& s, boost::asio::ip::udp::endpoint& out) {
    const auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    boost::system::error_code ec;
    const auto addr = boost::asio::ip::make_address(s.substr(0, colon), ec);
    if (ec) return false;
    try {
        out = {addr, static_cast<std::uint16_t>(std::stoi(s.substr(colon + 1)))};
    } catch (...) {
        return false;
    }
    return true;
}

int runDaemon(const Options& opt, lansync::app::Application& application) {
    using namespace lansync;
    using namespace lansync::app;

    EngineOptions eo;
    eo.bindAddress = opt.bind;
    eo.discoveryBindAddress = opt.bind;
    eo.discoveryMulticast = !opt.noMulticast;
    eo.discoveryBroadcast = !opt.noMulticast;
    for (const auto& t : opt.discoveryTargets) {
        boost::asio::ip::udp::endpoint ep;
        if (!parseHostPort(t, ep)) {
            std::cerr << "bad --discovery-target: " << t << "\n";
            return 2;
        }
        eo.discoveryTargets.push_back(ep);
    }
    eo.adjustSettings = [&](Settings& s) {
        if (opt.listenPort >= 0) s.listenPort = static_cast<std::uint16_t>(opt.listenPort);
        if (opt.discoveryPort >= 0) s.discoveryPort = static_cast<std::uint16_t>(opt.discoveryPort);
        if (!opt.discoveryTargets.empty()) s.announceIntervalSec = 1;
    };

    auto engine = Engine::create(application, eo);
    if (!engine) {
        std::cerr << "error: " << engine.error().message << "\n";
        return 1;
    }
    auto& e = **engine;

    std::mutex outMutex;
    auto say = [&](const std::string& line) {
        std::lock_guard l(outMutex);
        std::cout << line << std::endl;   // flush: scripts tail this
    };
    e.setEventHandler([&](const EngineEvent& ev) {
        switch (ev.kind) {
            case EngineEvent::Kind::PairingRequested:
                say("PAIRING " + ev.pairing.deviceId + " \"" + ev.pairing.deviceName + "\" code=" +
                    ev.pairing.code + (ev.pairing.incoming ? " incoming" : " outgoing"));
                if (opt.autoApprove) {
                    const auto id = ev.pairing.deviceId;
                    e.async([id](Services& s) { s.devices.decidePairing(id, true); });
                }
                break;
            case EngineEvent::Kind::PairingFinished:
                say(std::string(ev.isError ? "ERROR pairing: " : "PAIRED ") + ev.title + " " + ev.text);
                break;
            case EngineEvent::Kind::Notification:
                say(std::string(ev.isError ? "ERROR " : "NOTICE ") + ev.title + ": " + ev.text);
                break;
            default: break;
        }
    });

    if (auto r = e.start(); !r) {
        std::cerr << "error: " << r.error().message << "\n";
        return 1;
    }
    say("READY id=" + e.deviceId() + " port=" + std::to_string(e.listenPort()));
    if (opt.autoApprove) say("NOTICE pairing codes are NOT being verified (--auto-approve-pairing)");

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // Mutated only on the engine thread; the main thread just reads the atomic count.
    auto pendingShares = std::make_shared<std::vector<std::pair<std::string, std::string>>>(opt.shares);
    auto pendingCount = std::make_shared<std::atomic<std::size_t>>(pendingShares->size());
    while (!gStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Pair with the first visible device of the requested name.
        if (!opt.pairWith.empty()) {
            e.run([&](Services& s) -> Result<void> {
                for (const auto& d : s.devices.deviceList()) {
                    if (d.name == opt.pairWith && d.discovered && d.pairingStatus != "paired" &&
                        d.pairingStatus != "pending") {
                        (void)s.devices.startPairing(d.id);   // errors (busy etc.) are retried next tick
                    }
                }
                return {};
            });
        }
        // Share folders with the (single) paired device once connected.
        if (pendingCount->load() > 0) {
            e.run([&, pendingShares, pendingCount](Services& s) -> Result<void> {
                for (const auto& d : s.devices.deviceList()) {
                    if (d.pairingStatus != "paired" || !d.connected) continue;
                    for (auto it = pendingShares->begin(); it != pendingShares->end();) {
                        auto id = s.sync.addFolder(it->second, it->first, d.id, "bidirectional");
                        if (id) say("SHARED " + it->first + " with " + d.name);
                        else say("ERROR share " + it->first + ": " + id.error().message);
                        it = pendingShares->erase(it);
                    }
                    pendingCount->store(pendingShares->size());
                }
                return {};
            });
        }
    }
    say("STOPPING");
    e.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lansync;

    Options opt;
    opt.config.logLevel = "warn";

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string v;
        if (arg == "-h" || arg == "--help") { printUsage(); return 0; }
        else if (arg == "--data-dir") { if (!value(v)) return 2; opt.config.dataDir = v; }
        else if (arg == "--device-name") { if (!value(opt.config.deviceName)) return 2; }
        else if (arg == "--log-level") { if (!value(opt.config.logLevel)) return 2; }
        else if (arg == "--listen-port") { if (!value(v)) return 2; opt.listenPort = std::stoi(v); }
        else if (arg == "--discovery-port") { if (!value(v)) return 2; opt.discoveryPort = std::stoi(v); }
        else if (arg == "--discovery-target") { if (!value(v)) return 2; opt.discoveryTargets.push_back(v); }
        else if (arg == "--no-multicast") { opt.noMulticast = true; }
        else if (arg == "--bind") { if (!value(opt.bind)) return 2; }
        else if (arg == "--pair-with") { if (!value(opt.pairWith)) return 2; }
        else if (arg == "--auto-approve-pairing") { opt.autoApprove = true; }
        else if (arg == "--share") {
            if (!value(v)) return 2;
            const auto eq = v.find('=');
            if (eq == std::string::npos) { std::cerr << "--share expects name=path\n"; return 2; }
            opt.shares.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        }
        else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            printUsage();
            return 2;
        } else if (opt.command.empty()) {
            opt.command = arg;
        } else {
            std::cerr << "unexpected argument: " << arg << "\n";
            return 2;
        }
    }

    if (opt.command.empty()) { printUsage(); return 2; }
    if (opt.command == "version") {
        std::cout << kAppName << " " << kAppVersion << " (protocol " << kProtocolVersion << ")\n";
        return 0;
    }
    if (opt.command != "info" && opt.command != "daemon") {
        std::cerr << "unknown command: " << opt.command << "\n";
        printUsage();
        return 2;
    }

    log::LogConfig logCfg;
    logCfg.directory = opt.config.logDir();
    logCfg.level = opt.config.logLevel;
    logCfg.console = true;
    try {
        log::init(logCfg);
    } catch (const std::exception& e) {
        std::cerr << "cannot initialise logging: " << e.what() << "\n";
        return 1;
    }

    auto application = app::Application::create(opt.config);
    if (!application) {
        LS_ERROR("startup failed: {}", application.error().message);
        std::cerr << "error: " << application.error().message << "\n";
        log::shutdown();
        return 1;
    }

    int rc = 0;
    if (opt.command == "info") {
        std::cout << "device name    : " << (*application)->config().deviceName << "\n"
                  << "data directory : " << (*application)->config().dataDir.string() << "\n"
                  << "database       : " << (*application)->config().databasePath().string() << "\n"
                  << "schema version : " << (*application)->schemaVersion() << "\n";
    } else {
        rc = runDaemon(opt, **application);
    }
    log::shutdown();
    return rc;
}
