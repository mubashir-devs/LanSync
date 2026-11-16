#pragma once
// Qt adapter over the Qt-free Engine. This is the ONLY place where core types are
// translated into QObject properties/signals for QML.
//
// Threading: the Engine runs on its own thread. Nothing here blocks the GUI
// thread: commands are posted with Engine::run(), state is pulled with
// Engine::snapshotAsync() and applied back on the GUI thread via queued calls.

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

#include "core/application/engine.hpp"

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready CONSTANT)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString errorText READ errorText CONSTANT)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString dataDir READ dataDir CONSTANT)
    Q_PROPERTY(int schemaVersion READ schemaVersion CONSTANT)
    Q_PROPERTY(QString deviceId READ deviceId NOTIFY stateChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY stateChanged)
    Q_PROPERTY(int listenPort READ listenPort NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap stats READ stats NOTIFY stateChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY stateChanged)
    Q_PROPERTY(QVariantList pairedDevices READ pairedDevices NOTIFY stateChanged)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY stateChanged)
    Q_PROPERTY(QVariantList transfers READ transfers NOTIFY stateChanged)
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY stateChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY stateChanged)

public:
    // `engine` may be null; then `errorText` explains why. The controller owns the
    // engine and stops it first on destruction (so no event can outlive us).
    AppController(std::unique_ptr<lansync::app::Engine> engine, QString errorText, QString dataDir,
                  int schemaVersion, QObject* parent = nullptr);
    ~AppController() override;

    bool ready() const { return engine_ != nullptr; }
    bool loading() const { return loading_; }
    QString errorText() const { return errorText_; }
    QString appVersion() const;
    QString dataDir() const { return dataDir_; }
    int schemaVersion() const { return schemaVersion_; }
    QString deviceId() const { return deviceId_; }
    QString deviceName() const { return deviceName_; }
    int listenPort() const { return listenPort_; }
    QVariantMap stats() const { return stats_; }
    QVariantList devices() const { return devices_; }
    QVariantList pairedDevices() const;
    QVariantList folders() const { return folders_; }
    QVariantList transfers() const { return transfers_; }
    QVariantList conflicts() const { return conflicts_; }
    QVariantList history() const { return history_; }
    QVariantMap settings() const { return settings_; }

    // ---- actions (all non-blocking; failures surface as notification() signals)
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void discoverNow();
    Q_INVOKABLE void pairDevice(const QString& deviceId);
    Q_INVOKABLE void approvePairing(const QString& deviceId, bool accepted);
    Q_INVOKABLE void unpairDevice(const QString& deviceId);
    Q_INVOKABLE void removeDevice(const QString& deviceId);
    Q_INVOKABLE void addFolder(const QString& path, const QString& name, const QString& deviceId,
                               const QString& direction);
    Q_INVOKABLE void removeFolder(const QString& folderId);
    Q_INVOKABLE void setFolderEnabled(const QString& folderId, bool enabled);
    Q_INVOKABLE void setFolderPaused(const QString& folderId, bool paused);
    Q_INVOKABLE void setFolderDirection(const QString& folderId, const QString& direction);
    Q_INVOKABLE void syncNow(const QString& folderId, bool verifyContent = false);
    Q_INVOKABLE void pauseTransfer(const QString& id);
    Q_INVOKABLE void resumeTransfer(const QString& id);
    Q_INVOKABLE void cancelTransfer(const QString& id);
    Q_INVOKABLE void resolveConflict(int conflictId, const QString& resolution);
    Q_INVOKABLE void loadVersions(const QString& folderId, const QString& path);   // -> versionsLoaded
    Q_INVOKABLE void restoreVersion(int versionId);
    Q_INVOKABLE void saveSettings(const QVariantMap& values);

    // ---- helpers for QML
    Q_INVOKABLE QString urlToPath(const QUrl& url) const;
    Q_INVOKABLE QString formatBytes(double bytes) const;
    Q_INVOKABLE QString formatTime(double ms) const;
    Q_INVOKABLE QString formatAgo(double ms) const;
    Q_INVOKABLE QString baseName(const QString& path) const;

Q_SIGNALS:
    void stateChanged();
    void loadingChanged();
    void pairingRequested(const QString& deviceId, const QString& deviceName, const QString& code,
                          bool incoming);
    void pairingFinished(const QString& deviceId, const QString& deviceName, const QString& text,
                         bool isError);
    void notification(const QString& title, const QString& text, bool isError);
    void versionsLoaded(const QVariantList& versions);

private:
    void scheduleRefresh();
    void apply(const lansync::app::Snapshot& s);
    void onEngineEvent(const lansync::app::EngineEvent& e);   // engine thread!

    std::unique_ptr<lansync::app::Engine> engine_;
    QString errorText_, dataDir_;
    int schemaVersion_{0};
    bool loading_{true};
    QTimer refreshTimer_;
    bool refreshInFlight_{false};
    bool refreshAgain_{false};

    QString deviceId_, deviceName_;
    int listenPort_{0};
    QVariantMap stats_, settings_;
    QVariantList devices_, folders_, transfers_, conflicts_, history_;
};
