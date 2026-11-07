// modules/AndroidTVModule/AndroidTVModule.h
#pragma once

#include "core/IModule.h"
#include <QSet>
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
#include <QTcpSocket>
#include <QByteArray>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <winuser.h>
#  include <tlhelp32.h>
#  include <timeapi.h>
#endif

class SettingsManager;
class AndroidTVProfileManager;
class EmulatorGrpcClient;

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
    bool    consoleInput              = true;    // кнопки пульта через консоль эмулятора
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
        ConsoleInputRole,
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
    /// deleteData — удалить и данные профиля: папку <avdName>.avd (диск
    /// Android, до нескольких ГБ) и <avdName>.ini. Раньше они оставались на
    /// диске навсегда (аудит §18 п.16).
    Q_INVOKABLE bool removeProfile(const QString &id, bool deleteData = false);
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
    // Блокировка рекламы (AdGuard Home, аудит §23)
    Q_PROPERTY(bool    adBlockEnabled READ adBlockEnabled WRITE setAdBlockEnabled NOTIFY adBlockChanged)
    /// missing | notInstalled | stopped | starting | running | unknown
    Q_PROPERTY(QString adBlockState   READ adBlockState   NOTIFY adBlockChanged)
    Q_PROPERTY(bool    adBlockBusy    READ adBlockBusy    NOTIFY adBlockChanged)
    /// Загрузка AdGuard Home: -1 — не идёт, 0..1 — прогресс
    Q_PROPERTY(qreal   adBlockDownload READ adBlockDownload NOTIFY adBlockChanged)

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
    bool    adBlockEnabled() const;
    void    setAdBlockEnabled(bool on);
    QString adBlockState()   const { return m_adBlockState; }
    bool    adBlockBusy()    const { return m_adBlockBusy; }
    qreal   adBlockDownload() const { return m_adBlockDownload; }
    /// grpc.token из файла обнаружения эмулятора (открыто для тестов)
    static QByteArray grpcTokenFromDiscovery(const QByteArray &ini, int port);

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
    Q_INVOKABLE void refreshAdBlockState();
    /// Поставить/удалить службу AdGuard Home (запрос прав администратора)
    Q_INVOKABLE void downloadAdBlock();
    Q_INVOKABLE void installAdBlock();
    Q_INVOKABLE void uninstallAdBlock();
    Q_INVOKABLE void openAdBlockPanel();

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
    void adBlockChanged();

private:
    // ── Жизненный цикл ───────────────────────────────────────────────────────
    void startEmulator();
    /// Проверка AdGuard Home перед запуском эмулятора → m_adBlockDns
    void startAdBlockProbe();
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
    void setMediaStationOpacity(int alpha);          // сразу, отменяет плавный переход
    // Плавно (≈60 Гц) меняет прозрачность окна MediaStation от текущей до `to`
    // за durationMs; done вызывается в конце (не вызывается, если переход отменён)
    void fadeMediaStationOpacity(int to, int durationMs, std::function<void()> done = {});
    void stopMediaStationFade();
    void applyMediaStationAlpha(int alpha);
    void bringMediaStationToForeground();
    // ── Схема B (аудит §1.2 B): во время просмотра ТВ эмулятор — активное окно
    // (повышенный квант планировщика, без EcoQoS, прямой вывод без композиции
    // поверх), а MediaStation скрыт (SW_HIDE) и показывается только под
    // оверлеем как полупрозрачное затемнение.
    void hideMediaStationForTv();     // MediaStation → SW_HIDE, эмулятор активный
    void showMediaStationOverTv();    // MediaStation снова над ТВ (alpha 0, без активации)
    void focusEmulator();
    // Наш хук клавиатуры — снова первым в цепочке (после активации эмулятора)
    void requestKeyHookReinstall();
    void parkCursor();                // курсор — к краю экрана на время ТВ
    void restoreCursor();
    void revealWhenEmulatorPainted();   // проявить ТВ, когда окно эмулятора перестало быть белым
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
    // Консоль эмулятора (TCP localhost:<consolePort>): `event send` кладёт
    // нажатие прямо в виртуальную клавиатуру — без JVM в Android.
    static int androidKeyToLinuxKey(int androidKeyCode);
    bool sendKeyViaConsole(int androidKeyCode);
    void startConsole();
    void stopConsole();
    void onConsoleReadyRead();
    void failConsole(const QString &reason);
    static QByteArray readConsoleAuthToken();
    /// Токен gRPC: эмулятор с -grpc-use-token пишет его в файл обнаружения
    /// %TEMP%/avd/running/pid_<PID>.ini (grpc.token=…), а не в токен консоли
    QByteArray readGrpcToken(QString *source = nullptr) const;
    QString m_emuDiscoveryFile;     // «Advertising in: …» из вывода эмулятора
    void queueAdbKey(int androidKeyCode);   // старый путь: adb shell input keyevent

    // ── Быстрый ввод через sendevent (аудит 7а) ──────────────────────────────
    // `input keyevent` — запуск JVM в госте на каждую пачку нажатий (рывки
    // звука при листании); консоль эмулятора 36.4 события не доставляет.
    // sendevent — маленькая утилита без JVM: пишет событие прямо в устройство
    // ввода клавиатуры (/dev/input/eventN). При показе ТВ одним вызовом
    // `getevent -pl` + проверка прав записи находим подходящее устройство.
    enum class EvdevState { Unknown, Probing, Ready, Unavailable };
    void startEvdevProbe();
    void onEvdevProbeOutput(const QString &out);
    void queueEvdevKey(int linuxKey);
    int  evdevLinuxKey(int androidKeyCode) const;   // -1 — этой клавишей не отправить
    void resetEvdev();
    /// Разбор вывода `getevent -pl` + строк «WRITABLE <путь>»: путь лучшего
    /// устройства и коды его клавиш (из нужных нам). Пусто — не нашли.
    /// SU_OK в выводе — доступен `su 0` (образы без Google Play — userdebug):
    /// тогда годится любое устройство, а команды идут через `su 0 sh -c`.
    static QString pickEvdevDevice(const QString &probeOutput, QSet<int> *keys, QString *name,
                                   bool *viaSu = nullptr);
    // ── gRPC эмулятора (EmulatorGrpcClient) — основной путь ввода ────────────
    // Эмулятор запускается с `-grpc <порт> -grpc-use-token`. При показе ТВ —
    // getStatus; на первом нажатии — проверка через getevent, как у консоли.
    enum class GrpcState { Off, Checking, Ready, Failed };
    void startGrpcCheck();
    void startGrpcProbe(const QList<int> &androidKeys);
    void finishGrpcProbe(bool sawKey);
    void failGrpc(const QString &reason, const QList<int> &resend = {});
    bool sendKeyViaGrpc(int androidKeyCode);
    static int grpcEvdevKey(int androidKeyCode);     // -1 — не этим путём
    int  pickFreeGrpcPort() const;
    bool grpcDisabled() const;                       // флаги -grpc не поддерживаются этим эмулятором
    bool grpcKnownBroken() const;
    void rememberGrpcBroken();

    // Консоль эмулятора не доставляет события — запоминаем для этой сборки
    // эмулятора, чтобы не тратить 1,8 с на проверку при каждом запуске ТВ
    QString emulatorBuildKey() const;
    bool    consoleKnownBroken() const;
    void    rememberConsoleBroken();

#ifdef Q_OS_WIN
    void hideExtraEmulatorWindows(DWORD pid, HWND mainHwnd);
    void positionEmulatorBehindMediaStation(HWND hwnd);
    // Окно эмулятора: без рамки, на месте MediaStation, сразу под ним
    // (HWND_NOTOPMOST). show = показать (SW_SHOWNOACTIVATE) или скрыть.
    void placeEmulatorUnderMediaStation(HWND hwnd, bool show);
    // Активировать окно другого процесса (AttachThreadInput к текущему
    // активному потоку — иначе Windows может отклонить SetForegroundWindow)
    bool forceForeground(HWND hwnd);
    // Снимок клиентской области окна эмулятора (PrintWindow) — доля белых и
    // чёрных точек на сетке 32×18. false — снимок не получился.
    bool sampleEmulatorContent(HWND hwnd, int *whitePct, int *blackPct) const;
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
    class AdGuardHome *m_adGuard = nullptr;
    QString   m_adBlockState;
    bool      m_adBlockBusy        = false;
    qreal     m_adBlockDownload    = -1.0;
    bool      m_adBlockProbePending = false;
    QString   m_adBlockDns;               // "127.0.0.1", если AdGuard Home ответил
    std::function<void()> m_afterAdBlockProbe;
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
    // Android_TV.log сбрасывается на диск пачками раз в 250 мс, а не после
    // каждой строки (при запуске эмулятор выдаёт сотни строк — сотни
    // синхронных записей в GUI-потоке)
    QTimer     *m_logFlushTimer = nullptr;
    static constexpr int kLogFlushMs = 250;

    QFile       m_logcatLogFile;
    QTextStream m_logcatLogStream;
    QProcess   *m_logcatProcess = nullptr;

    QProcess   *m_keyShell = nullptr;
    // Объединение нажатий: `input keyevent` в Android запускает отдельную JVM
    // (сотни мс). Пока команда выполняется, новые нажатия копятся и уходят
    // одной командой; очередь ограничена, чтобы удержание кнопки не давало
    // «хвост» навигации после отпускания.
    QList<int>    m_pendingKeys;
    QList<int>    m_pendingEvdev;          // коды клавиш Linux для sendevent
    EvdevState    m_evdevState  = EvdevState::Unknown;
    QString       m_evdevPath;
    bool          m_evdevViaSu  = false;
    QSet<int>     m_evdevKeys;
    QList<int>    m_evdevWaiting;          // нажатия, пришедшие во время проверки
    QProcess     *m_evdevProbe  = nullptr;
    static constexpr int kEvdevProbeTimeoutMs = 4000;
    bool          m_keyBatchInFlight = false;
    QElapsedTimer m_keyBatchSent;
    QString       m_keyShellBuf;
    static constexpr int kMaxPendingKeys   = 8;
    static constexpr int kKeyBatchStuckMs  = 2000;

    enum class ConsoleState { Off, Connecting, Banner, Auth, Ready, Failed };
    QTcpSocket   *m_console       = nullptr;
    ConsoleState  m_consoleState  = ConsoleState::Off;
    QByteArray    m_consoleBuf;
    QList<int>    m_consolePending;   // нажатия до готовности консоли
    bool          m_consoleInput  = true;   // из профиля
    bool          m_consoleAuthNeeded  = false;
    bool          m_consoleReadyLogged = false;
    int           m_consoleKoCount     = 0;
    // Самопроверка консоли: команды `event send` эмулятор принимает с «OK»,
    // но это не значит, что событие дошло до Android (06.11 так и было:
    // консоль «работала», стрелки — нет). На первом нажатии слушаем
    // `adb shell getevent`; не увидели событие — переключаемся на adb.
    QProcess     *m_consoleProbe       = nullptr;
    bool          m_consoleVerified    = false;

    EmulatorGrpcClient *m_grpc        = nullptr;
    GrpcState     m_grpcState         = GrpcState::Off;
    int           m_grpcPort          = 0;       // 0 — эмулятор запущен без gRPC
    bool          m_grpcVerified      = false;
    QProcess     *m_grpcProbe         = nullptr;
    bool          m_grpcProbeSawKey   = false;
    QList<int>    m_grpcProbeKeys;               // отправлены во время проверки (коды Android)
    QList<int>    m_grpcWaiting;                 // пришли, пока идёт getStatus
    int           m_grpcStatusTries   = 0;
    static constexpr int kGrpcProbeMs       = 1500;
    static constexpr int kGrpcStatusRetries = 5;
    bool          m_consoleProbeSawKey = false;
    QList<int>    m_consoleProbeKeys;          // что отправили во время проверки (повтор через adb)
    static constexpr int kConsoleProbeMs = 1500;
    void startConsoleProbe(const QList<int> &keys);
    void finishConsoleProbe(bool sawKey);
    static QByteArray linuxKeyName(int linuxKey);

    bool m_overlayOpen        = false;
    bool m_overlayCooldown    = false;
    QTimer *m_overlayCooldownTimer = nullptr;
    static constexpr int kOverlayCooldownMs = 300;

    bool m_exitRequested  = false;
    bool m_emulatorShown  = false;
    bool m_msHiddenForTv  = false;   // MediaStation спрятан, на экране только эмулятор
    bool m_cursorParked   = false;
    int  m_savedCursorX   = 0;
    int  m_savedCursorY   = 0;

    // Плавная прозрачность MediaStation (показ ТВ, оверлей)
    int           m_msAlpha        = 255;
    QTimer       *m_fadeTimer      = nullptr;
    QElapsedTimer m_fadeClock;
    int           m_fadeFrom       = 255;
    int           m_fadeTo         = 255;
    int           m_fadeDurationMs = 0;
    std::function<void()> m_fadeDone;
    // Проявление ТВ: ждём, пока окно эмулятора перестанет быть белым
    QTimer       *m_revealTimer = nullptr;
    QElapsedTimer m_revealClock;
    int           m_revealChecks = 0;
    static constexpr int kRevealMinWaitMs  = 150;    // минимум после ShowWindow
    static constexpr int kRevealPollMs     = 100;
    static constexpr int kRevealMaxWaitMs  = 3000;   // дальше проявляем как есть
    static constexpr int kRevealFadeMs     = 250;
    static constexpr int kOverlayDimFadeMs      = 160;
    static constexpr int kOverlaySettleMs       = 50;    // = задержка выезда панели в OverlayScreen.qml

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
