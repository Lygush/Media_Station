// modules/AndroidTVModule/AndroidTVModule.h
#pragma once

#include "core/IModule.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <functional>
#include <QFile>
#include <QTextStream>
#include <QSize>
#include <QRegularExpression>
#include <QDateTime>
#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <QSettings>
#include <QVariantMap>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <winuser.h>
#  include <tlhelp32.h>
#  include <timeapi.h>
#endif

class SettingsManager;
class AndroidTVProfileManager;

struct AndroidTVProfile {
    QString id;
    QString name;
    QString avdName;
    QString systemImagePath;          // relative to appDir
    QString resolution;
    QString gpu;
    int     ramMb                     = 2048;
    int     cores                     = 4;
    bool    writableSystem            = false;
    bool    noBootAnim                = true;
    bool    noSnapshot                = false;
    bool    noSnapshotSave            = true;
    QString dnsServer;
    QString audioDriver               = "dsound";
    QVariant audioTimerMs             = QVariant("auto");   // int (manual) or "auto"
    QVariant dsoundLatencyMs          = QVariant("auto");   // int (manual) or "auto"
};

class AndroidTVProfileManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString activeProfileId READ activeProfileId NOTIFY activeProfileIdChanged)
    Q_PROPERTY(QVariantMap currentProfileData READ currentProfileData NOTIFY currentProfileDataChanged)

public:
    enum Roles {
        ProfileIdRole = Qt::UserRole + 1,
        NameRole,
        AvdNameRole,
        SystemImagePathRole,
        ResolutionRole,
        GpuRole,
        RamMbRole,
        CoresRole,
        WritableSystemRole,
        NoBootAnimRole,
        NoSnapshotRole,
        NoSnapshotSaveRole,
        DnsServerRole,
        AudioDriverRole,
        AudioTimerMsRole,
        DsoundLatencyMsRole,
        SizeMbRole
    };

    explicit AndroidTVProfileManager(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_profiles.size(); }
    QString activeProfileId() const { return m_activeProfileId; }

    Q_INVOKABLE QVariantMap getProfile(const QString &id) const;
    Q_INVOKABLE bool setActiveProfile(const QString &id);
    Q_INVOKABLE bool removeProfile(const QString &id);
    Q_INVOKABLE QString addProfile(const QString &systemImagePath, const QString &baseName);
    Q_INVOKABLE QStringList scanSystemImages() const;
    Q_INVOKABLE qint64 profileSizeMb(const QString &id) const;
    Q_INVOKABLE bool clearProfileData(const QString &id);
    Q_INVOKABLE bool updateProfileField(const QString &id, const QString &field, const QVariant &value);
    Q_INVOKABLE QString imageDisplayName(const QString &id) const;
    Q_INVOKABLE void save();  // save to profiles.json

    const AndroidTVProfile* activeProfile() const;
    QVariantMap currentProfileData() const;  // reactive dictionary

    void load(const QString &baseDir);

signals:
    void countChanged();
    void activeProfileIdChanged();
    void profilesChanged();
    void currentProfileDataChanged();

private:
    QString generateId() const;
    QString profilesFilePath() const;
    QList<AndroidTVProfile> m_profiles;
    QString m_activeProfileId;
    QString m_baseDir;
};

class AndroidTVModule : public QObject, public IModule
{
    Q_OBJECT
    Q_PROPERTY(bool    booting  READ isBooting  NOTIFY bootingChanged)
    Q_PROPERTY(bool    running  READ isRunning  NOTIFY runningChanged)
    Q_PROPERTY(QString status   READ status     NOTIFY statusChanged)
    Q_PROPERTY(bool    switching READ isSwitching NOTIFY switchingChanged)
    Q_PROPERTY(qint64  qcow2SizeMb READ qcow2SizeMb NOTIFY qcow2SizeMbChanged)
    Q_PROPERTY(AndroidTVProfileManager* profileManager READ profileManager CONSTANT)

public:
    explicit AndroidTVModule(QObject *parent = nullptr);
    ~AndroidTVModule();

    QString  moduleId() const override { return "androidtv"; }
    QString  qmlName()  const override { return "AndroidTV"; }
    QObject *qmlObject()      override { return this; }

    void initialize(EventBus *bus, SettingsManager *settings) override;
    void shutdown()                                           override;
    void registerWithQml(QQmlApplicationEngine *engine)      override;

    bool    isBooting()   const { return m_booting;   }
    bool    isRunning()   const { return m_running;    }
    bool    isSwitching() const { return m_switching;  }
    QString status()      const { return m_status;     }
    qint64  qcow2SizeMb() const { return m_qcow2SizeMb; }
    AndroidTVProfileManager* profileManager() const { return m_profileManager; }

    void setMediaStationHwnd(HWND hwnd) { m_mediaStationHwnd = hwnd; }

public slots:
    Q_INVOKABLE void launch();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void onMediaStationRestored();
    Q_INVOKABLE void applyFullscreenAndSwitch();
    Q_INVOKABLE void sendRemoteKey(const QString &key);
    Q_INVOKABLE bool resetQcow2();
    Q_INVOKABLE void refreshQcow2Size();

signals:
    void bootingChanged();
    void runningChanged();
    void statusChanged();
    void switchingChanged();
    void bootCompleted();
    void emulatorClosed();
    void emulatorReadyBehind();
    void emulatorRestoringMediaStation();
    void qcow2SizeMbChanged();

private:
    void startDnsProxy();
    void startEmulator();
    void stopDnsProxy();
    void startBootPolling();
    void stopBootPolling();
    void startWindowPolling();
    void stopWindowPolling();
    void adb(const QStringList &args, std::function<void(QString)> callback = {});
    void restoreMediaStation();
    void applyBlocklist();
    bool patchAvdIni(const AndroidTVProfile &profile);
    void loadEmulatorSettings(const AndroidTVProfile &profile);
    void patchAvdConfig(const AndroidTVProfile &profile, const QString &resolutionOverride = QString());

    void setMediaStationOpacity(int alpha);
    void forceMediaStationTopmost();
    void startKeepTopmostTimer();
    void stopKeepTopmostTimer();
    void startPositionSyncTimer();
    void stopPositionSyncTimer();

    void hideExtraEmulatorWindows(DWORD pid, HWND mainHwnd);
    void positionEmulatorBehindMediaStation(HWND hwnd);
    void updateEmulatorPosition();

    void killEmulatorTree();
    void cleanupAvdLocks();
    void cleanupAvdForProfile(const QString &avdName);

    void logImportant(const char *level, const QString &msg);
    void logToEmulatorFile(const QString &msg, bool fromEmulator = false);
    void openEmulatorLogFile();
    void closeEmulatorLogFile();

    void handleRemoteKey(const QString &key);
    static int remoteKeyToAndroidKeyCode(const QString &key);
    void notifyKeyMode(bool emulatorActive);

    HWND findEmulatorWindowByTitle() const;
    HWND findMediaStationWindow() const;
    void installWinEventHook();
    void removeWinEventHook();
    static void CALLBACK s_winEventProc(HWINEVENTHOOK hook, DWORD event,
                                        HWND hwnd, LONG idObject, LONG idChild,
                                        DWORD dwEventThread, DWORD dwmsEventTime);
    static AndroidTVModule *s_hookInstance;

    bool isSystemQcow2Valid(const AndroidTVProfile &profile);
    void repairSystemQcow2(const AndroidTVProfile &profile);

    void startLogcat();
    void stopLogcat();

    QString m_appDir;
    QString adbPath()          const;
    QString emulatorPath()     const;
    QString dnsProxyPath()     const;
    QString avdHomePath()      const;
    QString blocklistPath()    const;
    QString jdkBinPath()       const;
    QString emulatorLogPath()  const;
    QString logcatLogPath()    const;

    QString m_resolution;
    QString m_gpu;
    int     m_ramMb              = 2048;
    int     m_cores              = 4;
    bool    m_writableSystem     = false;
    bool    m_noBootAnim         = true;
    bool    m_noSnapshot         = false;
    bool    m_noSnapshotSave     = true;
    QString m_dnsServer;
    QString m_audioDriver        = "dsound";

    void setStatus(const QString &s);
    void setBooting(bool v);
    void setRunning(bool v);
    void setSwitching(bool v);

    EventBus        *m_bus      = nullptr;
    SettingsManager *m_settings = nullptr;
    bool             m_booting  = false;
    bool             m_running  = false;
    bool             m_switching = false;
    bool             m_shutdownDone = false;   // защита от двойного вызова shutdown()
    QString          m_status;
    qint64           m_qcow2SizeMb = -1;

    QProcess *m_emulatorProcess = nullptr;
    QProcess *m_dnsProxyProcess = nullptr;
    int       m_dnsProxyPort    = 0;
    QTimer   *m_bootPollTimer   = nullptr;
    int       m_bootPollCount   = 0;
    static constexpr int kBootPollIntervalMs = 2000;
    static constexpr int kBootPollMaxTries   = 120;

    QTimer   *m_windowPollTimer    = nullptr;
    QTimer   *m_keepTopmostTimer   = nullptr;
    QTimer   *m_positionSyncTimer  = nullptr;
    quint64   m_emulatorPid        = 0;
    quint64   m_initialEmulatorPid = 0;
    static constexpr int kWindowPollIntervalMs = 50;
    static constexpr int kWindowPollMaxTries   = 1200;

    QFile       m_emulatorLogFile;
    QTextStream m_emulatorLogStream;

    QFile       m_logcatLogFile;
    QTextStream m_logcatLogStream;
    QProcess   *m_logcatProcess = nullptr;

    bool m_overlayOpen        = false;
    bool m_overlayCooldown    = false;
    QTimer *m_overlayCooldownTimer = nullptr;
    static constexpr int kOverlayCooldownMs = 300;

    bool m_exitRequested  = false;
    bool m_emulatorShown  = false;

    QTimer  *m_healthCheckTimer    = nullptr;
    static constexpr qint64 kMaxQcow2SizeMb = 3072;

    void syncAdbKeyAndRestartServer(const QString &avdName, std::function<void()> onReady = {});
    void startHealthCheck();
    void stopHealthCheck();

    AndroidTVProfileManager *m_profileManager = nullptr;

#ifdef Q_OS_WIN
    HWND          m_mediaStationHwnd = nullptr;
    HWND          m_emulatorHwnd     = nullptr;
    HWINEVENTHOOK m_winEventHook     = nullptr;
    bool          m_timerResolutionSet = false;
    QSize getCurrentScreenResolution() const;
#endif
};
