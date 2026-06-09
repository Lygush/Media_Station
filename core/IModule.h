// core/IModule.h
// ─────────────────────────────────────────────────────────────────────────────
// IModule — интерфейс модуля.
//
// Минимальный новый модуль (только два файла, больше ничего не трогать):
//
//   // JoystickModule.h
//   #include "core/IModule.h"
//   #include "core/EventBus.h"
//
//   class JoystickModule : public QObject, public IModule {
//       Q_OBJECT
//   public:
//       QString  moduleId() const override { return "joystick"; }
//       void     initialize(EventBus *bus, SettingsManager *s) override;
//       QObject *qmlObject() override { return this; }
//       QString  qmlName()   const override { return "JoystickModule"; }
//   signals:
//       void joystickConnected(int id, QString name);
//   };
//
//   // main.cpp — одна строка:
//   host.registerModule(new JoystickModule(&host));
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>

class EventBus;
class SettingsManager;
class QObject;
class QQmlApplicationEngine;

class IModule
{
public:
    virtual ~IModule() = default;

    /// Уникальный идентификатор модуля ("tileManager", "network", "joystick"...)
    virtual QString moduleId() const = 0;

    /// Вызывается ModuleHost один раз при старте, до загрузки QML.
    /// Здесь модуль подключается к шине и кеширует нужные настройки.
    virtual void initialize(EventBus *bus, SettingsManager *settings) = 0;

    /// Вызывается перед завершением приложения (в порядке, обратном регистрации).
    virtual void shutdown() {}

    /// Объект для экспорта в QML. nullptr — не экспортировать.
    virtual QObject *qmlObject() { return nullptr; }

    /// Имя в QML. Пустая строка — не экспортировать.
    virtual QString qmlName() const { return {}; }

    /// Регистрирует qmlObject() в движке QML.
    /// По умолчанию: rootContext()->setContextProperty(qmlName(), qmlObject()).
    /// Переопределить для синглтонов (qmlRegisterSingletonInstance).
    virtual void registerWithQml(QQmlApplicationEngine *engine);
};
