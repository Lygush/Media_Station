// AppController.h
// ─────────────────────────────────────────────────────────────────────────────
// AppController — управление запуском приложений и игровым режимом.
//
// Отвечает за:
//   - Запуск игр через GameWatcher (PowerShell-скрипт + PID-мониторинг)
//   - Скрытие/восстановление taskbar во время перехода к игре (Win32)
//   - Завершение работы приложения
//
// Гарантия восстановления taskbar (баг-фикс):
//   hideBlackOverlay() вызывается НАПРЯМУЮ в C++ при gameClosed и launchFailed,
//   не полагаясь на QML-анимацию. Это важно: если игра крашится сразу после
//   старта, fadeToBlack (600мс) и fadeInMediaCenter запускаются одновременно,
//   их ScriptAction может не выполниться. C++ вызов — первичный, QML — дублирует.
//   Watchdog-таймер — последний рубеж: восстанавливает taskbar если через
//   kWatchdogMs после showBlackOverlay игра уже не запущена.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QTimer>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include "GameWatcher.h"
#include "core/IModule.h"

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

class AppController : public QObject, public IModule
{
    Q_OBJECT
    Q_PROPERTY(bool gameRunning READ isGameRunning NOTIFY gameRunningChanged)

public:
    explicit AppController(QObject *parent = nullptr);

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId() const override { return "appController"; }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return "AppController"; }
    /// Завершение приложения: остановка наблюдения за игрой и возврат панели
    /// задач. Раньше этот метод был скрыт одноимённым Q_INVOKABLE и никогда
    /// не вызывался — при выходе во время игры панель задач оставалась скрытой.
    void     shutdown() override;
    /// Регистрируется как QML-синглтон (не контекстное свойство).
    void     registerWithQml(QQmlApplicationEngine *engine) override;

    // ── API ───────────────────────────────────────────────────────────────────
    Q_INVOKABLE QString parentPath(const QString &path) const;
    Q_INVOKABLE void    launch(const QString &path, const QString &tileId = QString());
    Q_INVOKABLE void    quitApplication() const;
    Q_INVOKABLE void    showBlackOverlay();
    Q_INVOKABLE void    hideBlackOverlay();

    bool isGameRunning() const;

signals:
    void gameWindowAppeared(const QString &tileId, bool windowFound);
    void gameClosed(const QString &tileId);
    void launchFailed(const QString &tileId, const QString &reason);
    void gameRunningChanged();

private:
    GameWatcher *m_watcher;
    EventBus    *m_bus = nullptr;   // для эмиссии игровых событий в шину

    // ── Защита taskbar ────────────────────────────────────────────────────────
    bool    m_taskbarHidden  = false;
    QTimer *m_taskbarWatchdog    = nullptr;
    QTimer *m_windowLostFallback = nullptr; ///< восстановление если игра закрылась без окна

    static constexpr int kWatchdogMs      = 20'000; ///< таймаут watchdog (мс)
    static constexpr int kWindowFallbackMs = 5'000; ///< ожидание окна после gameWindowAppeared(false)


};
