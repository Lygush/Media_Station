// modules/AndroidTVModule/AndroidTVModule.h
#pragma once

#include "core/IModule.h"
#include <QObject>
#include <QProcess>
#include <QElapsedTimer>
#include <QProcessEnvironment>
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
    QString systemImagePath;          // относительно appDir или абсолютный путь к папке образа
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
    bool    diagnosticLogcat          = false;   // полный logcat в файл (для диагностики)
};

class AndroidTVProfileManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString activeProfileId READ activeProfileId NOTIFY activeProfileIdChanged)
    Q_PROPERTY(QVariantMap currentProfileData READ currentProfileData NOTIFY currentProfileDataChanged)
    Q_PROPERTY(bool locked READ isLocked NOTIFY lockedChanged)

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
        DiagnosticLogcatRole,
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

    // Папка AVD. Задаётся модулем после определения фактического расположения
    // (см. AndroidTVModule::resolveAvdHome()).
    void setAvdHome(const QString &avdHome) { m_avdHome = avdHome; }
    QString avdHome() const;

    // Абсолютный путь к папке системного образа профиля.
    QString systemImageDir(const AndroidTVProfile &p) const;

    // Пока эмулятор запущен, нельзя менять активный профиль и удалять его данные.
    void setLocked(bool locked);
    bool isLocked() const { return m_locked; }

signals:
    void countChanged();
    void activeProfileIdChanged();
    void profilesChanged();
    void currentProfileDataChanged();
    void lockedChanged();

private:
    QString generateId() const;
    QString profilesFilePath() const;
    QString makeUniqueAvdName(const QString &base) const;
    QString normalizeImagePath(const QString &path) const;
    QVariantMap profileToMap(const AndroidTVProfile &p) const;
    AndroidTVProfile makeDefaultProfile() const;
    QList<AndroidTVProfile> m_profiles;
    QString m_activeProfileId;
    QString m_baseDir;
    QString m_avdHome;
    bool    m_locked = false;
};

class AndroidTVModule : public QObject, public IModule
{
    Q_OBJECT
    Q_PROPERTY(bool    booting  READ isBooting  NOTIFY bootingChanged)
    Q_PROPERTY(bool    running  READ isRunning  NOTIFY runningChanged)
    // active — окно эмулятора сейчас показано пользователю (MediaStation прозрачен)
    Q_PROPERTY(bool    active   READ isActive   NOTIFY activeChanged)
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
    bool    isActive()    const { return m_emulatorShown; }
    bool    isSwitching() const { return m_switching;  }
    QString status()      const { return m_status;     }
    qint64  qcow2SizeMb() const { return m_qcow2SizeMb; }
    AndroidTVProfileManager* profileManager() const { return m_profileManager; }

#ifdef Q_OS_WIN
    void setMediaStationHwnd(HWND hwnd) { m_mediaStationHwnd = hwnd; }
#endif

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
    void activeChanged();
    void statusChanged();
    void switchingChanged();
    void bootCompleted();
    void emulatorClosed();
    void emulatorReadyBehind();
    void emulatorRestoringMediaStation();
    void qcow2SizeMbChanged();

private:
    // ── Жизненный цикл ───────────────────────────────────────────────────────
    void startDnsProxy();
    void startEmulator();
    void stopDnsProxy();
    void startBootPolling();
    void stopBootPolling();
    void onBootCompleted();
    void onEmulatorFinished(int exitCode);
    void failLaunch(const QString &userStatus, const QString &logMessage);
    void startWindowPolling();
    void stopWindowPolling();

    // adbRaw — без привязки к устройству (kill-server / start-server),
    // adb    — к нашему эмулятору (-s emulator-<port>).
    void adbRaw(const QStringList &args, std::function<void(QString)> callback = {},
                int timeoutMs = 10000);
    void adb(const QStringList &args, std::function<void(QString)> callback = {},
             int timeoutMs = 10000);
    QProcessEnvironment toolsEnvironment() const;
    QString adbSerial() const { return QStringLiteral("emulator-%1").arg(m_consolePort); }
    int  pickFreeConsolePort() const;

    void restoreMediaStation();
    void applyBlocklist();

    // ── AVD ──────────────────────────────────────────────────────────────────
    QString resolveAvdHome() const;
    bool ensureAvdExists(const AndroidTVProfile &profile);
    bool patchAvdIni(const AndroidTVProfile &profile);
    void loadEmulatorSettings(const AndroidTVProfile &profile);
    void patchAvdConfig(const AndroidTVProfile &profile, const QString &resolutionOverride = QString());
    void removeAvdLocks(const QString &avdDirPath);
    void syncAdbKeyAndRestartServer(const QString &avdName, std::function<void()> onReady = {});
    void syncAdbKeys(const QString &avdName);
    QStringList adbKeyDirs() const;                 // где могут лежать adbkey / adbkey.pub
    void checkEmulatorAdbKey(const QString &pubKey); // сверка ключа, который шлёт эмулятор
    void scanEmulatorOutput(const QString &out);     // «Boot completed», ключ adb

    // ── Окна ─────────────────────────────────────────────────────────────────
    void setMediaStationOpacity(int alpha);
    void forceMediaStationTopmost();
    void releaseMediaStationTopmost();   // HWND_NOTOPMOST: игры и другие окна снова могут быть сверху
    void startKeepTopmostTimer();
    void stopKeepTopmostTimer();
    void startPositionSyncTimer();
    void stopPositionSyncTimer();
    void setEmulatorShown(bool shown);
    void updateEmulatorPosition();

    void killEmulatorTree();
    void cleanupAvdLocks();

    // ── Логи ─────────────────────────────────────────────────────────────────
    void logImportant(const char *level, const QString &msg);
    void logToEmulatorFile(const QString &msg, bool fromEmulator = false);
    void openEmulatorLogFile();
    void closeEmulatorLogFile();

    // ── Ввод ─────────────────────────────────────────────────────────────────
    void handleRemoteKey(const QString &key);
    static int remoteKeyToAndroidKeyCode(const QString &key);
    void notifyKeyMode(bool emulatorActive);
    void sendAndroidKey(int keyCode);
    void flushPendingKeys();
    void startKeyShell();
    void stopKeyShell();

#ifdef Q_OS_WIN
    void hideExtraEmulatorWindows(DWORD pid, HWND mainHwnd);
    void positionEmulatorBehindMediaStation(HWND hwnd);
    bool isEmulatorProcess(DWORD pid) const;
    bool isEmulatorMainWindow(HWND hwnd) const;
    HWND findEmulatorMainWindow(bool allowFallback) const;
    HWND findMediaStationWindow() const;
    QSize getCurrentScreenResolution() const;
    static void CALLBACK s_winEventProc(HWINEVENTHOOK hook, DWORD event,
                                        HWND hwnd, LONG idObject, LONG idChild,
                                        DWORD dwEventThread, DWORD dwmsEventTime);
#endif
    void installWinEventHook();
    void removeWinEventHook();
    static AndroidTVModule *s_hookInstance;

    bool isSystemQcow2Valid(const AndroidTVProfile &profile);
    void repairSystemQcow2(const AndroidTVProfile &profile);

    void startLogcat();
    void stopLogcat();

    QString m_appDir;
    QString m_avdHome;
    QString adbPath()          const;
    QString emulatorPath()     const;
    QString dnsProxyPath()     const;
    QString avdHomePath()      const;
    QString avdDirPath(const QString &avdName) const;
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
    void updateProfileLock();

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
    int       m_consolePort     = 5554;
    QString   m_launchAvdName;               // AVD, с которым запущен текущий эмулятор
    QTimer   *m_bootPollTimer   = nullptr;
    int       m_bootPollCount   = 0;
    bool      m_bootPollInFlight = false;
    QElapsedTimer m_launchTimer;             // время с начала launch() — для журнала
    QString   m_lastBootPollOut;             // последний ответ getprop (offline/unauthorized…)
    int       m_emulatorLinesForwarded = 0;  // строки эмулятора с ошибками, отправленные в основной лог
    bool      m_emuReportedBoot = false;     // эмулятор написал «Boot completed in N ms»
    int       m_unauthorizedPolls = 0;       // подряд ответов adb «unauthorized» после загрузки
    bool      m_adbUnauthorized = false;     // запущен без доступа adb (пульт через adb не работает)
    static constexpr int kBootPollIntervalMs = 2000;
    static constexpr int kBootPollMaxTries   = 150;   // 5 минут

    QTimer   *m_windowPollTimer    = nullptr;
    QTimer   *m_keepTopmostTimer   = nullptr;
    QTimer   *m_positionSyncTimer  = nullptr;
    quint64   m_emulatorPid        = 0;
    quint64   m_priorityAppliedPid = 0;
    qint64    m_audioDelayedWrites = 0;   // прошлые значения метрики звука (health check)
    qint64    m_audioUnderruns     = 0;   // кому уже выставлен приоритет (не повторяем)
    // Повышенный класс приоритета и отказ от «экономии энергии» Windows для
    // процесса эмулятора (без этого фоновый qemu попадает на E-ядра/EcoQoS).
    void applyEmulatorProcessPriority(quint64 pid);
    quint64   m_initialEmulatorPid = 0;
    static constexpr int kWindowPollIntervalMs = 100;
    static constexpr int kWindowPollMaxTries   = 3000;  // 5 минут
    static constexpr int kWindowFallbackAfter  = 150;   // через 15 с ищем окно без проверки заголовка

    QFile       m_emulatorLogFile;
    QTextStream m_emulatorLogStream;

    QFile       m_logcatLogFile;
    QTextStream m_logcatLogStream;
    QProcess   *m_logcatProcess = nullptr;

    QProcess   *m_keyShell = nullptr;
    // Объединение нажатий: `input keyevent` в Android запускает отдельную JVM
    // (сотни мс). Пока команда выполняется, новые нажатия копятся и уходят
    // одной командой; очередь ограничена, чтобы удержание кнопки не давало
    // «хвост» навигации после отпускания.
    QList<int>    m_pendingKeys;
    bool          m_keyBatchInFlight = false;
    QElapsedTimer m_keyBatchSent;
    QString       m_keyShellBuf;
    static constexpr int kMaxPendingKeys   = 8;
    static constexpr int kKeyBatchStuckMs  = 2000;

    bool m_overlayOpen        = false;
    bool m_overlayCooldown    = false;
    QTimer *m_overlayCooldownTimer = nullptr;
    static constexpr int kOverlayCooldownMs = 300;

    bool m_exitRequested  = false;
    bool m_emulatorShown  = false;

    QTimer  *m_healthCheckTimer    = nullptr;
    static constexpr qint64 kMaxQcow2SizeMb = 3072;

    void startHealthCheck();
    void stopHealthCheck();

    AndroidTVProfileManager *m_profileManager = nullptr;

#ifdef Q_OS_WIN
    HWND          m_mediaStationHwnd = nullptr;
    HWND          m_emulatorHwnd     = nullptr;
    HWINEVENTHOOK m_winEventHook     = nullptr;
    bool          m_timerResolutionSet = false;
#endif
    bool          m_diagnosticLogcat   = false;   // из профиля: писать ли logcat
};
