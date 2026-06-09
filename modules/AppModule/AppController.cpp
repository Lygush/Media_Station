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

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_watcher(new GameWatcher(this))
    , m_taskbarWatchdog(new QTimer(this))
    , m_windowLostFallback(new QTimer(this))
{
    // ── Watchdog: последний рубеж восстановления taskbar ──────────────────────
    m_taskbarWatchdog->setSingleShot(true);
    m_taskbarWatchdog->setInterval(kWatchdogMs);
    connect(m_taskbarWatchdog, &QTimer::timeout, this, [this]() {
        if (!m_taskbarHidden) return;
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

void AppController::shutdown() const
{
    QGuiApplication::quit();
}
