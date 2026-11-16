#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QDir>
#include <QString>
#include <QUrl>

#include <memory>

#include "app_controller.hpp"
#include "core/application/engine.hpp"
#include "core/common/logging.hpp"

namespace {
// Optional environment overrides: let two instances run side by side on one machine
// (e.g. for demos and manual testing) without relying on multicast.
//   LANSYNC_DATA_DIR, LANSYNC_LISTEN_PORT, LANSYNC_DISCOVERY_PORT,
//   LANSYNC_DISCOVERY_TARGET=host:port (unicast discovery to the other instance)
lansync::app::EngineOptions optionsFromEnvironment() {
    using namespace lansync::app;
    EngineOptions o;
    const auto listen = qEnvironmentVariable("LANSYNC_LISTEN_PORT");
    const auto disc = qEnvironmentVariable("LANSYNC_DISCOVERY_PORT");
    const auto target = qEnvironmentVariable("LANSYNC_DISCOVERY_TARGET");
    if (!target.isEmpty()) {
        const auto colon = target.lastIndexOf(':');
        boost::system::error_code ec;
        const auto addr = boost::asio::ip::make_address(target.left(colon).toStdString(), ec);
        if (colon > 0 && !ec) {
            o.discoveryTargets.push_back({addr, static_cast<std::uint16_t>(target.mid(colon + 1).toUInt())});
            o.discoveryMulticast = false;
            o.discoveryBroadcast = false;
        }
    }
    o.adjustSettings = [listen, disc, hasTarget = !target.isEmpty()](Settings& s) {
        if (!listen.isEmpty()) s.listenPort = static_cast<std::uint16_t>(listen.toUInt());
        if (!disc.isEmpty()) s.discoveryPort = static_cast<std::uint16_t>(disc.toUInt());
        if (hasTarget) s.announceIntervalSec = 1;
    };
    return o;
}
}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication qtApp(argc, argv);
    QGuiApplication::setApplicationName("LanSync");
    QGuiApplication::setOrganizationName("LanSync");
    QQuickStyle::setStyle("Basic");   // consistent look; we style controls ourselves

    auto config = lansync::app::AppConfig::defaults();
    if (const auto dir = qEnvironmentVariable("LANSYNC_DATA_DIR"); !dir.isEmpty()) {
        config.dataDir = dir.toStdString();
    }

    QString errorText;
    std::unique_ptr<lansync::app::Application> application;
    std::unique_ptr<lansync::app::Engine> engine;

    try {
        lansync::log::LogConfig logCfg;
        logCfg.directory = config.logDir();
        logCfg.level = config.logLevel;
        logCfg.console = false;   // a GUI app logs to its file
        lansync::log::init(logCfg);
    } catch (const std::exception& e) {
        errorText = QString("Logging could not be initialised: %1").arg(e.what());
    }

    if (errorText.isEmpty()) {
        auto created = lansync::app::Application::create(config);
        if (!created) {
            errorText = QString::fromStdString(created.error().message);
            LS_ERROR("startup failed: {}", created.error().message);
        } else {
            application = std::move(*created);
            auto eng = lansync::app::Engine::create(*application, optionsFromEnvironment());
            if (eng) {
                engine = std::move(*eng);
            } else {
                errorText = QString::fromStdString(eng.error().message);
                LS_ERROR("engine creation failed: {}", eng.error().message);
            }
        }
    }

    const QString dataDir = QString::fromStdString(config.dataDir.string());
    const int schema = application ? application->schemaVersion() : 0;
    {
        // The controller owns (and stops) the engine; it must die before `application`.
        AppController controller(std::move(engine), errorText, dataDir, schema);

        QQmlApplicationEngine qml;
        qml.rootContext()->setContextProperty("appController", &controller);
        QObject::connect(
            &qml, &QQmlApplicationEngine::objectCreationFailed, &qtApp,
            [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
        qml.loadFromModule("LanSync.UI", "Main");

        // Developer aid: LANSYNC_SCREENSHOT_DIR=<dir> renders every page to a PNG and exits
        // (works headless with QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software).
        if (const auto shots = qEnvironmentVariable("LANSYNC_SCREENSHOT_DIR");
            !shots.isEmpty() && !qml.rootObjects().isEmpty()) {
            auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().first());
            if (const auto size = qEnvironmentVariable("LANSYNC_WINDOW_SIZE").split('x'); size.size() == 2) {
                window->resize(size[0].toInt(), size[1].toInt());   // e.g. 600x700, to check responsiveness
            }
            auto step = std::make_shared<int>(0);
            auto* timer = new QTimer(&qtApp);
            timer->setInterval(1200);
            QObject::connect(timer, &QTimer::timeout, &qtApp, [=, &qtApp] {
                if (*step > 0) {
                    window->grabWindow().save(QDir(shots).filePath(QString("page%1.png").arg(*step - 1)));
                }
                if (*step >= 7) {
                    qtApp.quit();
                    return;
                }
                window->setProperty("currentIndex", *step);
                ++*step;
            });
            timer->start();
        }
        const int rc = QGuiApplication::exec();
        lansync::log::shutdown();
        return rc;
    }
}
