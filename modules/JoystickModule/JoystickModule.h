// JoystickModule.h
// ─────────────────────────────────────────────────────────────────────────────
// JoystickModule — управление геймпадами через DS4Windows.
//
// Пути (относительно applicationDirPath()):
//   tools/DS4Windows/DS4Windows.exe
//   tools/DS4Windows/DS4WindowsCmd.exe
//
// Профили DS4Windows:
//   "XInput"  — игры с xinput (Xbox360 эмуляция)
//   "DS4"     — игры с нативным DualShock 4 / DirectInput
//   "Default" — всё остальное, возврат после закрытия игры
//
// Кастомные имена контроллеров:
//   Хранятся в data/controller_names.json  { "AA:BB:CC:DD:EE:FF": "Player 1" }
//   При первом подключении нового MAC — запись добавляется автоматически
//   с именем по умолчанию. Пользователь может переименовать через QML.
//
// Выбор профиля при запуске игры:
//   Тип ввода определяется при установке (InstallManager → PEDependencyAnalyzer)
//   и сохраняется в game_info.json["inputTypes"].
//   GameWatcher читает этот файл и эмитирует gameInputTypeDetected →
//   AppController пробрасывает в EventBus ("game", "inputType") →
//   JoystickModule выбирает профиль по приоритету: directinput > xinput > default.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QFuture>
#include <QHash>
#include <QMutex>
#include <QTimer>
#include <QVariantList>
#include <QFutureWatcher>
#include <QMap>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDir>
#include <QSaveFile>
#include <atomic>
#include <optional>
#include "core/IModule.h"

class EventBus;
class SettingsManager;

struct ControllerInfo {
    int     slot       = 0;
    QString name;
    QString macAddress; ///< "AA:BB:CC:DD:EE:FF", верхний регистр
    int     battery    = -1;   ///< 0..100, -1 если неизвестно
    bool    charging   = false;
    QString connType;          ///< "USB" / "Bluetooth"
};

class JoystickModule : public QObject, public IModule
{
    Q_OBJECT

    Q_PROPERTY(bool         ds4Running      READ ds4Running      NOTIFY ds4RunningChanged)
    Q_PROPERTY(bool         anyConnected    READ anyConnected    NOTIFY controllersChanged)
    Q_PROPERTY(QVariantList controllers     READ controllers     NOTIFY controllersChanged)
    Q_PROPERTY(int          controllerCount READ controllerCount NOTIFY controllersChanged)

public:
    explicit JoystickModule(QObject *parent = nullptr);
    ~JoystickModule() override;

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId() const override { return "joystick"; }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown() override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return "JoystickModule"; }

    bool         ds4Running()      const { return m_ds4Running; }
    bool         anyConnected()    const { return !m_controllers.isEmpty(); }
    int          controllerCount() const { return m_controllers.size(); }
    QVariantList controllers()     const;

    Q_INVOKABLE void activate();
    Q_INVOKABLE void deactivate();

    /// Сохранить кастомное имя для MAC. Если name пустое — сбрасывает на дефолт.
    Q_INVOKABLE void setControllerName(const QString &mac, const QString &name);

signals:
    void ds4RunningChanged();
    void controllersChanged();
    void ds4NotFound();
    void ds4LaunchFailed(const QString &reason);

private slots:
    void onPollTimer();

private:
    EventBus *m_bus    = nullptr;

    bool  m_ds4Running = false;
    bool  m_activated  = false;
    bool  m_gameRunning = false;   ///< игра на экране — опрос DS4Windows приостановлен
    bool  m_androidTvShown = false; ///< Android TV на экране — опрос тоже на паузе
    std::atomic<bool> m_weStartedDs4 { false };
    std::atomic<bool> m_stopping     { false };
    /// Смена профиля DS4 в пуле потоков (держит `this`): shutdown() их дожидается
    mutable QList<QFuture<void>> m_profileTasks;   // mutable: loadProfile() const   ///< true после shutdown(): фоновые задачи обязаны быстро выйти

    QTimer *m_pollTimer = nullptr;

    QFutureWatcher<QList<ControllerInfo>> *m_startupWatcher = nullptr;
    QFutureWatcher<QList<ControllerInfo>> *m_pollWatcher    = nullptr;

    static constexpr int kPollIntervalMs      =  5'000;
    static constexpr int kDs4StartupTimeoutMs = 15'000;
    static constexpr int kCmdTimeoutMs        =  5'000;

    QList<ControllerInfo> m_controllers;

    // Пути — кешируются в initialize() в главном потоке
    QString m_ds4ExePath;
    QString m_ds4CmdPath;
    QString m_dataDirPath; ///< <appDir>/data

    // MAC → кастомное имя. Загружается из data/controller_names.json.
    QMap<QString, QString> m_customNames;

    // ── Работа с именами ──────────────────────────────────────────────────────
    void    loadCustomNames();
    void    saveCustomNames() const;
    QString resolvedName(const QString &rawName, const QString &mac) const;
    void    ensureMacRegistered(const QString &mac, const QString &rawName);

    // ── Фоновые операции ──────────────────────────────────────────────────────
    bool startupSync();
    QList<ControllerInfo> scanControllersSync() const;
    void stopDs4Sync() const;
    std::optional<ControllerInfo> querySlotSync(int slot) const;

    // Кэш неизменных полей контроллера по слоту. Раньше каждый опрос (раз в
    // 5 с) запускал до 21 процесса DS4WindowsCmd.exe: имя, MAC и тип
    // подключения перечитывались каждый раз. Теперь они берутся из кэша,
    // а батарея/зарядка/MAC обновляются раз в kDynamicRefreshCycles опросов.
    mutable QMutex                    m_slotCacheMutex;
    mutable QHash<int, ControllerInfo> m_slotCache;
    mutable int                       m_pollCycle = 0;
    static constexpr int kDynamicRefreshCycles = 6;   // 6 × 5 с = 30 с
    QString runCmdSync(const QString &cmd) const;

    // ── Главный поток ─────────────────────────────────────────────────────────
    void applyControllers(const QList<ControllerInfo> &list, bool ds4Ready);
    void loadProfile(const QString &profileName) const;

    /// Обработчик события "game"/"inputType" с шины.
    /// inputTypes — список из game_info.json["inputTypes"], например ["directinput", "xinput"].
    /// Приоритет: directinput > xinput > default.
    void onGameInputType(const QStringList &inputTypes);

    void onGameClosed();
};
