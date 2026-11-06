// AppController.cpp
// ─────────────────────────────────────────────────────────────────────────────
// AppController — реализация методов для системных действий.
// ─────────────────────────────────────────────────────────────────────────────

#include "AppController.h"
#include "core/EventBus.h"

#include <QGuiApplication>
#include <QWindow>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QQmlEngine>
#include <QDebug>
#include <QProcess>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <powrprof.h>   // SetSuspendState
#endif

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_watcher(new GameWatcher(this))
    , m_taskbarWatchdog(new QTimer(this))
    , m_windowLostFallback(new QTimer(this))
{
#ifdef Q_OS_WIN
    // Если прошлый запуск упал во время игры, панель задач осталась скрытой
    // (её прячет showBlackOverlay). Возвращаем её при старте.
    for (const wchar_t *cls : { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" }) {
        if (HWND h = FindWindowW(cls, nullptr); h && !IsWindowVisible(h)) {
            ShowWindow(h, SW_SHOWNOACTIVATE);
            qWarning() << "AppController: taskbar was hidden at startup — restored";
        }
    }
#endif

    // ── Watchdog: последний рубеж восстановления taskbar ──────────────────────
    m_taskbarWatchdog->setSingleShot(true);
    m_taskbarWatchdog->setInterval(kWatchdogMs);
    connect(m_taskbarWatchdog, &QTimer::timeout, this, [this]() {
        if (!m_taskbarHidden) return;
        // Игра идёт (долгая загрузка/поиск окна) — панель задач не возвращаем,
        // иначе она всплывала поверх игры через 20 с после запуска
        if (m_watcher->isGameRunning()) return;
        qWarning() << "AppController: watchdog fired — restoring taskbar";
        hideBlackOverlay();
        if (!m_watcher->isGameRunning()) {
            emit gameClosed(QString());
            emit gameRunningChanged();
        }
    });

    // ── windowLostFallback ────────────────────────────────────────────────────
    m_windowLostFallback->setSingleShot(true);
    m_windowLostFallback->setInterval(kWindowFallbackMs);
    connect(m_windowLostFallback, &QTimer::timeout, this, [this]() {
        if (!m_watcher->isGameRunning()) {
            qWarning() << "AppController: windowLostFallback fired — game gone without window";
            hideBlackOverlay();
            emit gameClosed(QString());
            emit gameRunningChanged();
        }
    });

    // ── GameWatcher → AppController ───────────────────────────────────────────
    connect(m_watcher, &GameWatcher::gameWindowAppeared,
            this, [this](const QString &tileId, bool windowFound) {
                m_windowLostFallback->stop();
                m_taskbarWatchdog->stop();   // игра на экране — сторож запуска больше не нужен
                if (!windowFound) {
                    qWarning() << "AppController: window not found for" << tileId
                               << "— fallback armed";
                    m_windowLostFallback->start();
                }
                emit gameWindowAppeared(tileId, windowFound);
            });

    connect(m_watcher, &GameWatcher::gameClosed,
            this, [this](const QString &tileId) {
                m_windowLostFallback->stop();
                hideBlackOverlay();
                emit gameClosed(tileId);
                emit gameRunningChanged();
            });

    connect(m_watcher, &GameWatcher::launchFailed,
            this, [this](const QString &tileId, const QString &reason) {
                m_windowLostFallback->stop();
                hideBlackOverlay();
                emit launchFailed(tileId, reason);
                emit gameRunningChanged();
            });
}

// ── IModule ───────────────────────────────────────────────────────────────────

void AppController::initialize(EventBus *bus, SettingsManager * /*settings*/)
{
    m_bus = bus;

    // Тип ввода из game_info.json → JoystickModule получит команду на профиль.
    // Сигнал эмитируется из рабочего потока GameWatcher, Qt::AutoConnection
    // обеспечивает корректную передачу в главный поток шины.
    connect(m_watcher, &GameWatcher::gameInputTypeDetected, bus,
        [bus](const QString &tileId, const QStringList &inputTypes) {
            emit bus->moduleEvent("game", "inputType",
                {{"tileId", tileId}, {"inputTypes", inputTypes}});
        });

    connect(m_watcher, &GameWatcher::gameWindowAppeared, bus,
        [bus](const QString &tileId, bool /*windowFound*/) {
            emit bus->moduleEvent("game", "appeared", {{"tileId", tileId}});
        });

    connect(m_watcher, &GameWatcher::gameClosed, bus,
        [bus](const QString &tileId) {
            emit bus->moduleEvent("game", "closed", {{"tileId", tileId}});
        });

    connect(m_watcher, &GameWatcher::launchFailed, bus,
        [bus](const QString &tileId, const QString &reason) {
            emit bus->moduleEvent("game", "failed",
                {{"tileId", tileId}, {"reason", reason}});
        });
}

void AppController::registerWithQml(QQmlApplicationEngine * /*engine*/)
{
    qmlRegisterSingletonInstance("AppSettings", 1, 0, "AppController", this);
}

// ── isGameRunning ─────────────────────────────────────────────────────────────

bool AppController::isGameRunning() const
{
    return m_watcher->isGameRunning();
}

// ── parentPath ────────────────────────────────────────────────────────────────

QString AppController::parentPath(const QString &path) const
{
    QString p = path;
    p.replace('\\', '/');
    if (p.endsWith('/')) p.chop(1);
    const int lastSlash = p.lastIndexOf('/');
    if (lastSlash <= 0) return QString();
    QString parent = p.left(lastSlash);
    if (parent.endsWith(':')) parent += '/';
    return parent;
}

// ── launch ────────────────────────────────────────────────────────────────────

void AppController::launch(const QString &path, const QString &tileId)
{
    QUrl url = QUrl::fromUserInput(path);
    if (url.isValid() && (url.scheme() == "http" || url.scheme() == "https")) {
        QDesktopServices::openUrl(url);
        return;
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }

    const QString suffix = info.suffix().toLower();
    const QString id     = tileId.isEmpty() ? path : tileId;

    if (suffix == "ps1") {
        emit gameRunningChanged();
        if (m_bus)
            emit m_bus->moduleEvent("game", "launching",
                {{"tileId", id}, {"exePath", info.absoluteFilePath()}});
        m_watcher->launch(path, id);
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
}

// ── showBlackOverlay / hideBlackOverlay ───────────────────────────────────────

void AppController::showBlackOverlay()
{
#ifdef Q_OS_WIN
    HWND hTray  = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND hTray2 = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
    if (hTray)  ShowWindow(hTray,  SW_HIDE);
    if (hTray2) ShowWindow(hTray2, SW_HIDE);
#endif
    m_taskbarHidden = true;
    m_taskbarWatchdog->start();
}

void AppController::hideBlackOverlay()
{
    m_taskbarWatchdog->stop();
    if (!m_taskbarHidden) return;

    m_taskbarHidden = false;
#ifdef Q_OS_WIN
    HWND hTray  = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND hTray2 = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
    if (hTray)  ShowWindow(hTray,  SW_SHOWNOACTIVATE);
    if (hTray2) ShowWindow(hTray2, SW_SHOWNOACTIVATE);
#endif
}

// ── shutdown ──────────────────────────────────────────────────────────────────

void AppController::quitApplication() const
{
    QGuiApplication::quit();
}

// ── Питание ──────────────────────────────────────────────────────────────────

namespace {
enum class PendingPower { None, Shutdown, Restart };
PendingPower g_pendingPower = PendingPower::None;

#ifdef Q_OS_WIN
// ExitWindowsEx требует привилегию SE_SHUTDOWN_NAME в токене процесса.
// У обычного пользователя она есть, но выключена — включаем.
bool enableShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)
                    && AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr)
                    && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}
#endif
} // namespace

void AppController::powerAction(const QString &action)
{
    qInfo() << "[AppController] power action:" << action;
    if (action == QLatin1String("sleep")) {
#ifdef Q_OS_WIN
        // SetSuspendState(hibernate=FALSE, force=FALSE, wakeupEventsDisabled=FALSE)
        if (!SetSuspendState(FALSE, FALSE, FALSE))
            qWarning() << "[AppController] SetSuspendState failed, error" << GetLastError();
#endif
        return;
    }
    if (action == QLatin1String("shutdown"))      g_pendingPower = PendingPower::Shutdown;
    else if (action == QLatin1String("restart"))  g_pendingPower = PendingPower::Restart;
    else { qWarning() << "[AppController] unknown power action" << action; return; }

    // Сначала обычный выход: main() завершит модули (эмулятор — через
    // `emu kill`, DS4Windows, хуки) и только потом выключит компьютер.
    QGuiApplication::quit();
}

/*static*/ void AppController::executePendingPowerAction()
{
    if (g_pendingPower == PendingPower::None) return;
#ifdef Q_OS_WIN
    const bool restart = (g_pendingPower == PendingPower::Restart);
    const UINT flags = restart ? EWX_REBOOT : (EWX_SHUTDOWN | EWX_POWEROFF);
    if (enableShutdownPrivilege()
        && ExitWindowsEx(flags, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER
                                | SHTDN_REASON_FLAG_PLANNED))
        return;
    qWarning() << "[AppController] ExitWindowsEx failed, error" << GetLastError()
               << "- trying shutdown.exe";
    QProcess::startDetached(QStringLiteral("shutdown.exe"),
                            {restart ? QStringLiteral("/r") : QStringLiteral("/s"),
                             QStringLiteral("/t"), QStringLiteral("0")});
#endif
}

void AppController::shutdown()
{
    m_taskbarWatchdog->stop();
    m_windowLostFallback->stop();
    m_watcher->stopAll();
    if (m_taskbarHidden)
        hideBlackOverlay();   // возвращает панель задач
}
