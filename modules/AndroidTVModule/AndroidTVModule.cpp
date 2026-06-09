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

#ifdef Q_OS_WIN
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

AndroidTVModule::AndroidTVModule(QObject *parent)
    : QObject(parent)
    , m_switching(false)
    , m_exitRequested(false)
    , m_overlayCooldown(false)
    , m_emulatorShown(false)
    , m_overlayCooldownTimer(nullptr)
{}

AndroidTVModule::~AndroidTVModule()
{
    // Деструктор только вызывает shutdown() — вся логика там.
    // Это важно: ModuleHost вызывает shutdown() до удаления объекта,
    // поэтому здесь уже всё должно быть завершено.
    // Флаг m_shutdownDone в shutdown() защищает от двойного выполнения.
    shutdown();
}


void AndroidTVModule::setMediaStationOpacity(int alpha)
{
#ifdef Q_OS_WIN
    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd)) {
        qWarning() << "[AndroidTV] setMediaStationOpacity: invalid HWND, trying to find again";
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd)) {
            qWarning() << "[AndroidTV] setMediaStationOpacity: still invalid HWND, abort";
            return;
        }
    }
    SetLayeredWindowAttributes(m_mediaStationHwnd, 0, alpha, LWA_ALPHA);
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

    openEmulatorLogFile();
    logImportant(Logger::kLevelInfo, "initialized, appDir = " + m_appDir);

    // Profile manager
    m_profileManager = new AndroidTVProfileManager(this);
    m_profileManager->load(m_appDir);
    logImportant(Logger::kLevelInfo, QString("Profiles loaded, active: %1").arg(m_profileManager->activeProfileId()));

#ifdef Q_OS_WIN
    m_mediaStationHwnd = findMediaStationWindow();
    forceMediaStationTopmost();
#endif

    // Клавиши пульта (обработчик handleRemoteKey находится в AndroidTVInput.cpp)
    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data) {
                if (mod == QLatin1String(G72Keys::kModule) &&
                    ev  == QLatin1String(G72Keys::kEventKey))
                    handleRemoteKey(data.value(QStringLiteral("key")).toString());
            });

    // Обработка открытия/закрытия оверлея с cooldown и управлением прозрачностью
    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data) {
                if (mod != QLatin1String(Overlay::kModule) ||
                    ev  != QLatin1String(Overlay::kEventVisibility))
                    return;

                const bool visible = data.value(QStringLiteral("visible"), false).toBool();
                m_overlayOpen = visible;

                if (visible) {
                    if (m_overlayCooldownTimer) {
                        m_overlayCooldownTimer->stop();
                        m_overlayCooldownTimer->deleteLater();
                        m_overlayCooldownTimer = nullptr;
                    }
                    m_overlayCooldown = false;
                    setMediaStationOpacity(128);
                    logToEmulatorFile("[Overlay] MediaStation set to semi-transparent (alpha=128)");
                } else {
                    m_overlayCooldown = true;
                    if (!m_overlayCooldownTimer) {
                        m_overlayCooldownTimer = new QTimer(this);
                        m_overlayCooldownTimer->setSingleShot(true);
                        m_overlayCooldownTimer->setInterval(kOverlayCooldownMs);
                        connect(m_overlayCooldownTimer, &QTimer::timeout, this, [this]() {
                            m_overlayCooldown = false;
                            m_overlayCooldownTimer->deleteLater();
                            m_overlayCooldownTimer = nullptr;
                            logToEmulatorFile("[Overlay] Cooldown ended, keys allowed to emulator");
                        });
                        m_overlayCooldownTimer->start();
                    }
                    logToEmulatorFile("[Overlay] Overlay closed, cooldown started");

                    if (m_running && !m_exitRequested) {
                        setMediaStationOpacity(0);
                        logToEmulatorFile("[Overlay] MediaStation set to transparent (emulator active)");
                    } else if (m_exitRequested) {
                        logToEmulatorFile("[Overlay] Exit requested, skip opacity change");
                        m_exitRequested = false;
                    } else {
                        setMediaStationOpacity(255);
                        logToEmulatorFile("[Overlay] MediaStation set to opaque (emulator inactive)");
                    }
                }
            });

    // Запрос остановки эмулятора
    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &) {
                if (mod == QLatin1String("androidtv") && ev == QLatin1String("stopRequested")) {
                    m_exitRequested = true;
                    stop();
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
    // Защита от двойного вызова (деструктор + ModuleHost::shutdownAll)
    if (m_shutdownDone) return;
    m_shutdownDone = true;

    // 1. Останавливаем таймеры — они могут запускать новые операции
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

    // 2. Убиваем эмулятор — синхронно, но быстро (TerminateProcess)
    killEmulatorTree();

    // 3. Останавливаем DNS-прокси
    stopDnsProxy();

    // 4. adb kill-server — запускаем без ожидания завершения.
    // kill-server завершает adb daemon, это нужно чтобы не оставлять
    // зависший процесс. startDetached не блокирует GUI.
    if (QFile::exists(adbPath())) {
        QProcess::startDetached(adbPath(), {"kill-server"});
    }

    // 5. Удаляем объект процесса эмулятора (процесс уже убит в killEmulatorTree)
    delete m_emulatorProcess;
    m_emulatorProcess = nullptr;

    // 6. Чистим локи AVD и лог-файл
    cleanupAvdLocks();
    closeEmulatorLogFile();

    // 7. Восстанавливаем окно MediaStation
#ifdef Q_OS_WIN
    if (m_mediaStationHwnd && IsWindow(m_mediaStationHwnd)) {
        setMediaStationOpacity(255);
        SetWindowPos(m_mediaStationHwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(m_mediaStationHwnd, SW_SHOW);
        m_mediaStationHwnd = nullptr;
    }
#endif

    // 8. Обнуляем шину — никаких новых событий после этой точки
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
}

void AndroidTVModule::setRunning(bool v)
{
    if (m_running == v) return;
    m_running = v;
    emit runningChanged();
}

void AndroidTVModule::setSwitching(bool v)
{
    if (m_switching == v) return;
    m_switching = v;
    emit switchingChanged();
}

#ifdef Q_OS_WIN
void AndroidTVModule::killEmulatorTree()
{
    if (!m_emulatorPid) return;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe)) {
        do {
            if (pe.th32ParentProcessID == static_cast<DWORD>(m_emulatorPid) ||
                pe.th32ProcessID == static_cast<DWORD>(m_emulatorPid)) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 1);
                    CloseHandle(hProc);
                    logToEmulatorFile(QString("Terminated process %1 (%2)").arg(pe.th32ProcessID).arg(pe.szExeFile));
                }
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);

    if (m_emulatorProcess && m_emulatorProcess->state() == QProcess::Running) {
        m_emulatorProcess->kill();
        m_emulatorProcess->waitForFinished(2000);
    }
}
#else
void AndroidTVModule::killEmulatorTree()
{
    if (m_emulatorProcess && m_emulatorProcess->state() == QProcess::Running) {
        m_emulatorProcess->kill();
        m_emulatorProcess->waitForFinished(2000);
    }
}
#endif

void AndroidTVModule::cleanupAvdLocks()
{
    QDir avdDir(avdHomePath());
    QStringList avds = avdDir.entryList(QStringList() << "*.avd", QDir::Dirs);
    for (const QString &avd : avds) {
        QDir d(avdDir.filePath(avd));
        QStringList locks = d.entryList({"*.lock"}, QDir::Files);
        for (const QString &lock : locks) {
            if (QFile::remove(d.absoluteFilePath(lock)))
                logToEmulatorFile("Removed stale AVD lock: " + lock);
        }
    }
}

void AndroidTVModule::cleanupAvdForProfile(const QString &avdName)
{
    // Not needed; we just remove locks in cleanupAvdLocks()
    Q_UNUSED(avdName);
}
