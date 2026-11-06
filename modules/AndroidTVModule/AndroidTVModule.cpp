// modules/AndroidTVModule/AndroidTVModule.cpp

#include "AndroidTVModule.h"
#include "LoggerModule.h"
#include "G72KeysModule.h"
#include "OverlayModule.h"
#include "core/EventBus.h"
#include "SettingsManager.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#endif

AndroidTVModule::AndroidTVModule(QObject *parent)
    : QObject(parent)
{}

AndroidTVModule::~AndroidTVModule()
{
    // ModuleHost вызывает shutdown() до удаления объекта; флаг m_shutdownDone
    // защищает от повторного выполнения.
    shutdown();
}


void AndroidTVModule::setMediaStationOpacity(int alpha)
{
#ifdef Q_OS_WIN
    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd)) {
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd)) {
            qWarning() << "[AndroidTV] setMediaStationOpacity: MediaStation window not found";
            return;
        }
    }
    const LONG_PTR ex = GetWindowLongPtr(m_mediaStationHwnd, GWL_EXSTYLE);
    if (alpha >= 255) {
        // Полностью видимое окно прозрачность не нужна. Раньше WS_EX_LAYERED
        // стоял всегда, с запуска программы: лишняя работа композитора DWM на
        // каждый кадр и для игр, и для ТВ. Снимаем стиль, окно снова обычное.
        if (ex & WS_EX_LAYERED) {
            SetWindowLongPtr(m_mediaStationHwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
            RedrawWindow(m_mediaStationHwnd, nullptr, nullptr,
                         RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
            logToEmulatorFile("MediaStation opacity 255 (layered style removed)");
        }
        return;
    }
    // Окно должно быть WS_EX_LAYERED, иначе SetLayeredWindowAttributes ничего не делает.
    if (!(ex & WS_EX_LAYERED))
        SetWindowLongPtr(m_mediaStationHwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);

    if (!SetLayeredWindowAttributes(m_mediaStationHwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA))
        logToEmulatorFile(QString("WARNING: SetLayeredWindowAttributes(%1) failed, error=%2")
                              .arg(alpha).arg(GetLastError()));
    else
        logToEmulatorFile(QString("MediaStation opacity set to %1").arg(alpha));
#else
    Q_UNUSED(alpha)
#endif
}

void AndroidTVModule::initialize(EventBus *bus, SettingsManager *settings)
{
    m_bus      = bus;
    m_settings = settings;
    m_appDir   = QCoreApplication::applicationDirPath();
    m_avdHome  = resolveAvdHome();

    openEmulatorLogFile();
    logImportant(Logger::kLevelInfo, "initialized, appDir = " + m_appDir);
    logToEmulatorFile("AVD home: " + m_avdHome);

    // Profile manager
    m_profileManager = new AndroidTVProfileManager(this);
    m_profileManager->setAvdHome(m_avdHome);
    m_profileManager->load(m_appDir);
    logImportant(Logger::kLevelInfo, QString("Profiles loaded, active: %1").arg(m_profileManager->activeProfileId()));

    // Клавиши пульта (обработчик handleRemoteKey находится в AndroidTVInput.cpp)
    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data) {
                if (mod == QLatin1String(G72Keys::kModule) &&
                    ev  == QLatin1String(G72Keys::kEventKey))
                    handleRemoteKey(data.value(QStringLiteral("key")).toString());
            });

    // Открытие/закрытие оверлея: cooldown и прозрачность MediaStation
    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data) {
                if (mod != QLatin1String(Overlay::kModule) ||
                    ev  != QLatin1String(Overlay::kEventVisibility))
                    return;

                const bool visible = data.value(QStringLiteral("visible"), false).toBool();
                m_overlayOpen = visible;

                // Прозрачностью управляем только когда эмулятор реально показан.
                // Оверлей в контексте игры MediaStation не касается.
                if (!m_emulatorShown) {
                    m_overlayCooldown = false;
                    return;
                }

                if (visible) {
                    if (m_overlayCooldownTimer) {
                        m_overlayCooldownTimer->stop();
                        m_overlayCooldownTimer->deleteLater();
                        m_overlayCooldownTimer = nullptr;
                    }
                    m_overlayCooldown = false;
                    setMediaStationOpacity(128);
                    logToEmulatorFile("[Overlay] MediaStation semi-transparent (alpha=128)");
                    return;
                }

                m_overlayCooldown = true;
                if (!m_overlayCooldownTimer) {
                    m_overlayCooldownTimer = new QTimer(this);
                    m_overlayCooldownTimer->setSingleShot(true);
                    m_overlayCooldownTimer->setInterval(kOverlayCooldownMs);
                    connect(m_overlayCooldownTimer, &QTimer::timeout, this, [this]() {
                        m_overlayCooldown = false;
                        if (m_overlayCooldownTimer) {
                            m_overlayCooldownTimer->deleteLater();
                            m_overlayCooldownTimer = nullptr;
                        }
                        logToEmulatorFile("[Overlay] Cooldown ended, keys allowed to emulator");
                    });
                    m_overlayCooldownTimer->start();
                }

                if (m_exitRequested) {
                    logToEmulatorFile("[Overlay] Exit requested, skip opacity change");
                } else if (m_running) {
                    setMediaStationOpacity(0);
                    logToEmulatorFile("[Overlay] MediaStation transparent (emulator active)");
                } else {
                    setMediaStationOpacity(255);
                }
            });

    // Запрос остановки эмулятора (из оверлея)
    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &) {
                if (mod == QLatin1String("androidtv") && ev == QLatin1String("stopRequested")) {
                    m_exitRequested = true;
                    stop();
                }
            });

    // Игра, запущенная при работающем (скрытом) эмуляторе, не должна оказаться
    // под TOPMOST-окном MediaStation.
    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &) {
                if (mod != QLatin1String("game") || !(m_running || m_booting)) return;
                if (ev == QLatin1String("launching") || ev == QLatin1String("appeared")) {
                    stopKeepTopmostTimer();
                    releaseMediaStationTopmost();
                    logToEmulatorFile("[Game] game started — MediaStation TOPMOST released");
                } else if ((ev == QLatin1String("closed") || ev == QLatin1String("failed")) && !m_emulatorShown) {
                    forceMediaStationTopmost();
                }
            });

    // Определяем размер qcow2 сразу при запуске приложения
    QTimer::singleShot(0, this, &AndroidTVModule::refreshQcow2Size);
}

void AndroidTVModule::registerWithQml(QQmlApplicationEngine *engine)
{
    if (engine && !qmlName().isEmpty() && qmlObject()) {
        engine->rootContext()->setContextProperty(qmlName(), qmlObject());
        qDebug() << "AndroidTVModule registered in QML as" << qmlName();
    }
}

void AndroidTVModule::shutdown()
{
    if (m_shutdownDone) return;
    m_shutdownDone = true;

    if (m_booting)
        logImportant(Logger::kLevelWarning,
                     QString("application closing while Android TV is still booting "
                             "(%1 s, status \"%2\", last adb: %3)")
                         .arg(m_launchTimer.isValid() ? m_launchTimer.elapsed() / 1000 : 0)
                         .arg(m_status,
                              m_lastBootPollOut.isEmpty() ? QStringLiteral("-") : m_lastBootPollOut.simplified()));

    // 1. Таймеры — они могут запускать новые операции
    stopKeepTopmostTimer();
    stopBootPolling();
    stopWindowPolling();
    stopPositionSyncTimer();
    stopHealthCheck();
    removeWinEventHook();

    if (m_overlayCooldownTimer) {
        m_overlayCooldownTimer->stop();
        delete m_overlayCooldownTimer;
        m_overlayCooldownTimer = nullptr;
    }

    // 2. Вспомогательные процессы adb
    stopKeyShell();
    stopConsole();
    stopLogcat();

    // 3. Эмулятор — синхронно, но быстро (TerminateProcess по всему дереву).
    //    Обработчики отключаем: после shutdown() они не должны трогать UI/шину.
    if (m_emulatorProcess)
        m_emulatorProcess->disconnect(this);

    // Сначала просим эмулятор закрыться самому («emu kill»): так он успевает
    // дописать userdata-qcow2. Сразу TerminateProcess при выходе из программы
    // во время просмотра мог оставить образ в «грязном» состоянии.
#ifdef Q_OS_WIN
    if ((m_running || m_booting) && m_emulatorPid && QFile::exists(adbPath())) {
        QProcess emuKill;
        emuKill.setProcessEnvironment(toolsEnvironment());
        emuKill.start(adbPath(), {"-s", adbSerial(), "emu", "kill"});
        emuKill.waitForFinished(1500);
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(m_emulatorPid))) {
            const DWORD r = WaitForSingleObject(h, 3000);
            CloseHandle(h);
            logToEmulatorFile(r == WAIT_OBJECT_0 ? "shutdown: emulator exited after 'emu kill'"
                                                 : "shutdown: emulator still running, killing");
        }
    }
#endif
    killEmulatorTree();

    // 4. DNS-прокси
    stopDnsProxy();

    // 5. adb kill-server — без ожидания (startDetached не блокирует GUI)
    if (QFile::exists(adbPath()))
        QProcess::startDetached(adbPath(), {"kill-server"});

    delete m_emulatorProcess;
    m_emulatorProcess = nullptr;

#ifdef Q_OS_WIN
    if (m_timerResolutionSet) {
        timeEndPeriod(1);
        m_timerResolutionSet = false;
    }
#endif

    // 6. Локи AVD и лог-файл
    cleanupAvdLocks();
    closeEmulatorLogFile();

    // 7. Восстанавливаем окно MediaStation
#ifdef Q_OS_WIN
    if (m_mediaStationHwnd && IsWindow(m_mediaStationHwnd)) {
        const LONG_PTR ex = GetWindowLongPtr(m_mediaStationHwnd, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED)
            SetWindowLongPtr(m_mediaStationHwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        SetWindowPos(m_mediaStationHwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        m_mediaStationHwnd = nullptr;
    }
#endif

    // 8. Никаких событий на шину после этой точки
    m_bus = nullptr;
}

void AndroidTVModule::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

void AndroidTVModule::setBooting(bool v)
{
    if (m_booting == v) return;
    m_booting = v;
    emit bootingChanged();
    updateProfileLock();
}

void AndroidTVModule::setRunning(bool v)
{
    if (m_running == v) return;
    m_running = v;
    emit runningChanged();
    updateProfileLock();
}

void AndroidTVModule::setSwitching(bool v)
{
    if (m_switching == v) return;
    m_switching = v;
    emit switchingChanged();
}

void AndroidTVModule::setEmulatorShown(bool shown)
{
    if (m_emulatorShown == shown) return;
    m_emulatorShown = shown;
    emit activeChanged();
    // Другим модулям (опрос геймпадов) — ТВ на экране или нет
    if (m_bus)
        emit m_bus->moduleEvent(QStringLiteral("androidtv"), QStringLiteral("shown"),
                                QVariantMap{{"shown", shown}});
}

void AndroidTVModule::updateProfileLock()
{
    if (m_profileManager)
        m_profileManager->setLocked(m_booting || m_running);
}

#ifdef Q_OS_WIN
void AndroidTVModule::killEmulatorTree()
{
    const DWORD roots[] = { static_cast<DWORD>(m_initialEmulatorPid),
                            static_cast<DWORD>(m_emulatorPid) };

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        QHash<DWORD, DWORD>   parentOf;   // pid → ppid
        QHash<DWORD, QString> nameOf;
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                parentOf.insert(pe.th32ProcessID, pe.th32ParentProcessID);
                nameOf.insert(pe.th32ProcessID, QString::fromWCharArray(pe.szExeFile));
            } while (Process32NextW(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);

        // Всё дерево: emulator.exe → qemu-system-*.exe → crashpad_handler и т.д.
        QList<DWORD> order;
        QSet<DWORD>  seen;
        for (DWORD r : roots) {
            if (r && parentOf.contains(r) && !seen.contains(r)) {
                seen.insert(r);
                order.append(r);
            }
        }
        for (int i = 0; i < order.size(); ++i) {
            const DWORD parent = order.at(i);
            for (auto it = parentOf.constBegin(); it != parentOf.constEnd(); ++it) {
                if (it.value() == parent && it.key() != parent && !seen.contains(it.key())) {
                    seen.insert(it.key());
                    order.append(it.key());
                }
            }
        }

        // Снизу вверх: сначала самые глубокие потомки
        for (int i = order.size() - 1; i >= 0; --i) {
            const DWORD pid = order.at(i);
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProc) {
                TerminateProcess(hProc, 1);
                CloseHandle(hProc);
                logToEmulatorFile(QString("Terminated process %1 (%2)").arg(pid).arg(nameOf.value(pid)));
            }
        }
    }

    if (m_emulatorProcess && m_emulatorProcess->state() != QProcess::NotRunning) {
        m_emulatorProcess->kill();
        m_emulatorProcess->waitForFinished(2000);
    }
}
#else
void AndroidTVModule::killEmulatorTree()
{
    if (m_emulatorProcess && m_emulatorProcess->state() != QProcess::NotRunning) {
        m_emulatorProcess->kill();
        m_emulatorProcess->waitForFinished(2000);
    }
}
#endif

void AndroidTVModule::removeAvdLocks(const QString &avdDirPath)
{
    // Эмулятор создаёт *.lock. На Windows это ПАПКИ (внутри файл pid),
    // на других ОС — файлы. Удаляем и то, и другое.
    QDir d(avdDirPath);
    if (!d.exists()) return;
    const QFileInfoList locks = d.entryInfoList(
        {"*.lock"}, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &fi : locks) {
        const bool ok = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively()
                                   : QFile::remove(fi.absoluteFilePath());
        logToEmulatorFile((ok ? "Removed stale AVD lock: " : "WARNING: could not remove AVD lock: ")
                          + fi.fileName());
    }
}

void AndroidTVModule::cleanupAvdLocks()
{
    QDir avdDir(avdHomePath());
    const QStringList avds = avdDir.entryList(QStringList() << "*.avd", QDir::Dirs);
    for (const QString &avd : avds)
        removeAvdLocks(avdDir.filePath(avd));
}
