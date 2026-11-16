#include "app_controller.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>

#include "core/common/version.hpp"

using namespace lansync;
using namespace lansync::app;

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QVariantMap toMap(const DeviceInfo& d) {
    QString conn = d.connected ? "Connected" : (d.discovered ? "Online" : "Offline");
    return {{"id", q(d.id)},
            {"name", q(d.name)},
            {"address", q(d.address)},
            {"port", d.port},
            {"pairingStatus", q(d.pairingStatus)},
            {"discovered", d.discovered},
            {"connected", d.connected},
            {"connection", conn},
            {"lastSeenMs", static_cast<double>(d.lastSeenMs)},
            {"fingerprint", q(d.fingerprint)},
            {"appVersion", q(d.appVersion)}};
}

QVariantMap toMap(const FolderInfo& f) {
    return {{"id", q(f.id)},
            {"name", q(f.name)},
            {"path", q(f.path)},
            {"remoteDeviceId", q(f.remoteDeviceId)},
            {"remoteDeviceName", q(f.remoteDeviceName)},
            {"direction", q(f.direction)},
            {"enabled", f.enabled},
            {"paused", f.paused},
            {"state", q(f.state)},
            {"fileCount", static_cast<qlonglong>(f.fileCount)},
            {"pendingCount", static_cast<qlonglong>(f.pendingCount)},
            {"conflictCount", static_cast<qlonglong>(f.conflictCount)},
            {"lastSyncMs", static_cast<double>(f.lastSyncMs)},
            {"error", q(f.error)}};
}

QVariantMap toMap(const ConflictInfo& c) {
    return {{"id", static_cast<qlonglong>(c.id)},
            {"folderId", q(c.folderId)},
            {"folderName", q(c.folderName)},
            {"path", q(c.path)},
            {"copyPath", q(c.copyPath)},
            {"localHash", q(c.localHash)},
            {"remoteHash", q(c.remoteHash)},
            {"localModifiedMs", static_cast<double>(c.localModifiedMs)},
            {"remoteModifiedMs", static_cast<double>(c.remoteModifiedMs)},
            {"detectedMs", static_cast<double>(c.detectedMs)}};
}

QVariantMap toMap(const HistoryInfo& h) {
    return {{"id", static_cast<qlonglong>(h.id)},
            {"timestampMs", static_cast<double>(h.timestampMs)},
            {"deviceName", q(h.deviceName)},
            {"path", q(h.path)},
            {"operation", q(h.operation)},
            {"result", q(h.result)},
            {"detail", q(h.detail)}};
}

QString formatSpeed(double bps) {
    if (bps < 1) return {};
    return QLocale().formattedDataSize(static_cast<qint64>(bps)) + "/s";
}

QString formatEta(qint64 sec) {
    if (sec < 0) return {};
    if (sec < 60) return QString("%1 s left").arg(sec);
    if (sec < 3600) return QString("%1 min left").arg(sec / 60 + 1);
    return QString("%1 h left").arg(sec / 3600 + 1);
}

}  // namespace

AppController::AppController(std::unique_ptr<Engine> engine, QString errorText, QString dataDir,
                             int schemaVersion, QObject* parent)
    : QObject(parent),
      engine_(std::move(engine)),
      errorText_(std::move(errorText)),
      dataDir_(std::move(dataDir)),
      schemaVersion_(schemaVersion) {
    stats_ = {{"pairedDevices", 0}, {"activeFolders", 0}, {"filesSynced", 0},
              {"pendingTransfers", 0}, {"activeTransfers", 0}, {"openConflicts", 0}};
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(150);   // coalesce bursts of engine events
    connect(&refreshTimer_, &QTimer::timeout, this, &AppController::refresh);

    if (engine_) {
        engine_->setEventHandler([this](const EngineEvent& e) { onEngineEvent(e); });
        auto started = engine_->start();
        if (!started) {
            errorText_ = q(started.error().message);
            engine_.reset();
            loading_ = false;
            return;
        }
        refresh();
    } else {
        loading_ = false;
    }
}

AppController::~AppController() {
    if (engine_) {
        engine_->setEventHandler({});
        engine_->stop();   // joins the engine thread: no callback can reach us afterwards
    }
}

QString AppController::appVersion() const { return QString::fromUtf8(kAppVersion); }

// ------------------------------------------------------------------ events (engine thread)

void AppController::onEngineEvent(const EngineEvent& e) {
    switch (e.kind) {
        case EngineEvent::Kind::PairingRequested: {
            const auto p = e.pairing;
            QMetaObject::invokeMethod(this, [this, p] {
                Q_EMIT pairingRequested(q(p.deviceId), q(p.deviceName), q(p.code), p.incoming);
            }, Qt::QueuedConnection);
            break;
        }
        case EngineEvent::Kind::PairingFinished: {
            const auto ev = e;
            QMetaObject::invokeMethod(this, [this, ev] {
                Q_EMIT pairingFinished(q(ev.pairing.deviceId), q(ev.title), q(ev.text), ev.isError);
            }, Qt::QueuedConnection);
            break;
        }
        case EngineEvent::Kind::Notification: {
            const auto ev = e;
            QMetaObject::invokeMethod(this, [this, ev] {
                Q_EMIT notification(q(ev.title), q(ev.text), ev.isError);
            }, Qt::QueuedConnection);
            break;
        }
        default: break;
    }
    QMetaObject::invokeMethod(this, [this] { scheduleRefresh(); }, Qt::QueuedConnection);
}

// ------------------------------------------------------------------ state

void AppController::scheduleRefresh() {
    if (!refreshTimer_.isActive()) refreshTimer_.start();
}

void AppController::refresh() {
    if (!engine_) return;
    if (refreshInFlight_) {   // never queue unbounded snapshots
        refreshAgain_ = true;
        return;
    }
    refreshInFlight_ = true;
    engine_->snapshotAsync([this](Snapshot s) {
        QMetaObject::invokeMethod(this, [this, snap = std::move(s)] {
            apply(snap);
            refreshInFlight_ = false;
            if (refreshAgain_) {
                refreshAgain_ = false;
                scheduleRefresh();
            }
        }, Qt::QueuedConnection);
    });
}

void AppController::apply(const Snapshot& s) {
    deviceId_ = q(s.deviceId);
    deviceName_ = q(s.deviceName);
    listenPort_ = s.listenPort;

    stats_ = {{"pairedDevices", s.stats.pairedDevices},
              {"activeFolders", s.stats.activeFolders},
              {"filesSynced", static_cast<qlonglong>(s.stats.filesSynced)},
              {"pendingTransfers", s.stats.pendingTransfers},
              {"activeTransfers", s.stats.activeTransfers},
              {"openConflicts", s.stats.openConflicts}};

    devices_.clear();
    for (const auto& d : s.devices) devices_.push_back(toMap(d));
    folders_.clear();
    for (const auto& f : s.folders) folders_.push_back(toMap(f));
    conflicts_.clear();
    for (const auto& c : s.conflicts) conflicts_.push_back(toMap(c));
    history_.clear();
    for (const auto& h : s.history) history_.push_back(toMap(h));

    transfers_.clear();
    for (const auto& t : s.transfers) {
        const double progress = t.total > 0 ? static_cast<double>(t.bytes) / static_cast<double>(t.total)
                                            : (t.status == "completed" ? 1.0 : 0.0);
        QString detail = formatBytes(static_cast<double>(t.bytes)) + " of " + formatBytes(static_cast<double>(t.total));
        const QString speed = formatSpeed(t.speedBps), eta = formatEta(t.etaSec);
        if (!speed.isEmpty()) detail += " · " + speed;
        if (!eta.isEmpty()) detail += " · " + eta;
        transfers_.push_back(QVariantMap{{"id", q(t.id)},
                                         {"folderName", q(t.folderName)},
                                         {"path", q(t.path)},
                                         {"direction", q(t.direction)},
                                         {"peerName", q(t.peerName)},
                                         {"status", q(t.status)},
                                         {"progress", progress},
                                         {"detail", detail},
                                         {"error", q(t.error)}});
    }

    const auto& st = s.settings;
    settings_ = {{"deviceName", q(st.deviceName)},
                 {"listenPort", st.listenPort},
                 {"allowPublicNetworks", st.allowPublicNetworks},
                 {"discoveryEnabled", st.discoveryEnabled},
                 {"discoveryPort", st.discoveryPort},
                 {"announceIntervalSec", st.announceIntervalSec},
                 {"maxConcurrentTransfers", st.maxConcurrentTransfers},
                 {"chunkSizeKb", st.chunkSizeKb},
                 {"maxFileSizeMb", static_cast<qlonglong>(st.maxFileSizeMb)},
                 {"reconcileIntervalSec", st.reconcileIntervalSec},
                 {"versionMaxCount", st.versionMaxCount},
                 {"versionMaxAgeDays", st.versionMaxAgeDays},
                 {"logLevel", q(st.logLevel)}};

    if (loading_) {
        loading_ = false;
        Q_EMIT loadingChanged();
    }
    Q_EMIT stateChanged();
}

QVariantList AppController::pairedDevices() const {
    QVariantList out;
    for (const auto& d : devices_) {
        if (d.toMap().value("pairingStatus").toString() == "paired") out.push_back(d);
    }
    return out;
}

// ------------------------------------------------------------------ actions

#define REQUIRE_ENGINE() if (!engine_) return

void AppController::discoverNow() {
    REQUIRE_ENGINE();
    engine_->discoverNow();
}

void AppController::pairDevice(const QString& id) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString()](Services& s) { return s.devices.startPairing(id); });
}

void AppController::approvePairing(const QString& id, bool accepted) {
    REQUIRE_ENGINE();
    engine_->async([id = id.toStdString(), accepted](Services& s) { s.devices.decidePairing(id, accepted); });
}

void AppController::unpairDevice(const QString& id) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString()](Services& s) { return s.devices.unpair(id); });
}

void AppController::removeDevice(const QString& id) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString()](Services& s) -> Result<void> {
        for (const auto& f : s.sync.folderInfos()) {
            if (f.remoteDeviceId == id) {
                return Error{ErrorCode::InvalidArgument, "Remove the folders shared with this device first."};
            }
        }
        return s.devices.forget(id);
    });
}

void AppController::addFolder(const QString& path, const QString& name, const QString& deviceId,
                              const QString& direction) {
    REQUIRE_ENGINE();
    engine_->run(
        [=, p = path.toStdString(), n = name.toStdString(), d = deviceId.toStdString(),
         dir = direction.toStdString()](Services& s) -> Result<void> {
            auto r = s.sync.addFolder(p, n, d, dir);
            if (!r) return r.error();
            return {};
        });
}

void AppController::removeFolder(const QString& id) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString()](Services& s) { return s.sync.removeFolder(id); });
}

void AppController::setFolderEnabled(const QString& id, bool enabled) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString(), enabled](Services& s) { return s.sync.setFolderEnabled(id, enabled); });
}

void AppController::setFolderPaused(const QString& id, bool paused) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString(), paused](Services& s) { return s.sync.setFolderPaused(id, paused); });
}

void AppController::setFolderDirection(const QString& id, const QString& direction) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString(), d = direction.toStdString()](Services& s) {
        return s.sync.setFolderDirection(id, d);
    });
}

void AppController::syncNow(const QString& id, bool verify) {
    REQUIRE_ENGINE();
    engine_->async([id = id.toStdString(), verify](Services& s) { s.sync.syncNow(id, verify); });
}

void AppController::pauseTransfer(const QString& id) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString()](Services& s) { return s.sync.pauseTransfer(id); });
}

void AppController::resumeTransfer(const QString& id) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString()](Services& s) { return s.sync.resumeTransfer(id); });
}

void AppController::cancelTransfer(const QString& id) {
    REQUIRE_ENGINE();
    engine_->run([id = id.toStdString()](Services& s) { return s.sync.cancelTransfer(id); });
}

void AppController::resolveConflict(int conflictId, const QString& resolution) {
    REQUIRE_ENGINE();
    engine_->run([conflictId, r = resolution.toStdString()](Services& s) { return s.sync.resolveConflict(conflictId, r); });
}

void AppController::loadVersions(const QString& folderId, const QString& path) {
    REQUIRE_ENGINE();
    engine_->async([this, f = folderId.toStdString(), p = path.toStdString()](Services& s) {
        auto r = s.sync.listVersions(f, p);
        QVariantList list;
        QString error;
        if (r) {
            for (const auto& v : *r) {
                list.push_back(QVariantMap{{"id", static_cast<qlonglong>(v.id)},
                                           {"size", static_cast<qlonglong>(v.size)},
                                           {"modifiedMs", static_cast<double>(v.modifiedMs)},
                                           {"createdMs", static_cast<double>(v.createdMs)},
                                           {"hash", q(v.hash.substr(0, 12))}});
            }
        } else {
            error = q(r.error().message);
        }
        QMetaObject::invokeMethod(this, [this, list, error] {
            if (!error.isEmpty()) Q_EMIT notification("Versions", error, true);
            Q_EMIT versionsLoaded(list);
        }, Qt::QueuedConnection);
    });
}

void AppController::restoreVersion(int versionId) {
    REQUIRE_ENGINE();
    engine_->run(
        [versionId](Services& s) { return s.sync.restoreVersion(versionId); },
        [this](const Result<void>& r) {
            QMetaObject::invokeMethod(this, [this, ok = r.ok(), msg = r ? QString() : q(r.error().message)] {
                Q_EMIT notification(ok ? "Version restored" : "Restore failed",
                                  ok ? "The previous version was put back and will sync to your other devices." : msg, !ok);
            }, Qt::QueuedConnection);
        });
}

void AppController::saveSettings(const QVariantMap& v) {
    REQUIRE_ENGINE();
    engine_->run([v](Services& s) -> Result<void> {
        Settings st = s.settings;   // start from the live values
        auto num = [&](const char* k, auto& out) {
            if (v.contains(k)) out = static_cast<std::decay_t<decltype(out)>>(v.value(k).toLongLong());
        };
        if (v.contains("deviceName")) st.deviceName = v.value("deviceName").toString().toStdString();
        if (v.contains("logLevel")) st.logLevel = v.value("logLevel").toString().toStdString();
        if (v.contains("allowPublicNetworks")) st.allowPublicNetworks = v.value("allowPublicNetworks").toBool();
        if (v.contains("discoveryEnabled")) st.discoveryEnabled = v.value("discoveryEnabled").toBool();
        num("listenPort", st.listenPort);
        num("discoveryPort", st.discoveryPort);
        num("announceIntervalSec", st.announceIntervalSec);
        num("maxConcurrentTransfers", st.maxConcurrentTransfers);
        num("chunkSizeKb", st.chunkSizeKb);
        num("maxFileSizeMb", st.maxFileSizeMb);
        num("reconcileIntervalSec", st.reconcileIntervalSec);
        num("versionMaxCount", st.versionMaxCount);
        num("versionMaxAgeDays", st.versionMaxAgeDays);
        return s.engine.updateSettings(std::move(st));
    });
}

// ------------------------------------------------------------------ helpers

QString AppController::urlToPath(const QUrl& url) const { return url.toLocalFile(); }

QString AppController::formatBytes(double bytes) const {
    return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

QString AppController::formatTime(double ms) const {
    if (ms <= 0) return "—";
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms)), QLocale::ShortFormat);
}

QString AppController::formatAgo(double ms) const {
    if (ms <= 0) return "never";
    const qint64 sec = (QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(ms)) / 1000;
    if (sec < 5) return "just now";
    if (sec < 60) return QString("%1 s ago").arg(sec);
    if (sec < 3600) return QString("%1 min ago").arg(sec / 60);
    if (sec < 86400) return QString("%1 h ago").arg(sec / 3600);
    return QString("%1 d ago").arg(sec / 86400);
}

QString AppController::baseName(const QString& path) const { return QFileInfo(path).fileName(); }
