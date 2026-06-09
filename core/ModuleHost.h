// core/ModuleHost.h
// ─────────────────────────────────────────────────────────────────────────────
// ModuleHost — реестр и владелец всех модулей.
//
// Использование в main.cpp:
//
//   QQmlApplicationEngine engine;
//   qmlRegisterSingletonType(...);   // Settings.qml — до ModuleHost
//
//   ModuleHost host(&engine);
//   host.registerModule(new TileManager(&host));
//   host.registerModule(new NetworkListener(&host));
//   host.registerModule(new AppController(&host));
//   host.initializeAll();            // до engine.load()
//
//   engine.load(...);
//   app.exec();
//   // деструктор host вызывает shutdownAll() автоматически
//
// ModuleHost владеет:
//   - EventBus        — шина событий между модулями
//   - SettingsManager — настройки (передаётся в initialize каждого модуля)
//   - Все IModule*    — через QObject parent
//
// Завершение:
//   shutdownAll() вызывает shutdown() каждого модуля в порядке, обратном
//   регистрации, с общим таймаутом kTotalShutdownTimeoutMs.
//   Если модуль не завершился за kModuleShutdownWarnMs — пишем предупреждение,
//   но продолжаем завершение остальных.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <vector>
#include <QObject>
#include <QElapsedTimer>
#include "EventBus.h"
#include "SettingsManager.h"

class IModule;
class QQmlApplicationEngine;

class ModuleHost : public QObject
{
    Q_OBJECT
public:
    explicit ModuleHost(QQmlApplicationEngine *engine, QObject *parent = nullptr);
    ~ModuleHost() override;

    /// Регистрирует модуль. Вызывать до initializeAll().
    void registerModule(IModule *module);

    /// Инициализирует все модули и регистрирует их QML-объекты.
    /// Вызывать после registerModule() и ДО engine.load().
    void initializeAll();

    /// Вызывается деструктором автоматически.
    void shutdownAll();

    EventBus        *bus()      { return &m_bus; }
    SettingsManager *settings() { return &m_settings; }

private:
    // Предупреждение если один модуль завершается дольше этого времени (мс).
    static constexpr int kModuleShutdownWarnMs  = 1000;

    // Жёсткий лимит на весь процесс завершения (мс).
    // Если все модули не завершились за это время — завершаем принудительно.
    static constexpr int kTotalShutdownTimeoutMs = 8000;

    QQmlApplicationEngine  *m_engine;
    EventBus                m_bus;
    SettingsManager         m_settings;
    std::vector<IModule *>  m_modules;
    bool                    m_initialized  = false;
    bool                    m_shuttingDown = false;
};
