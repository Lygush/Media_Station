// core/EventBus.h
// ─────────────────────────────────────────────────────────────────────────────
// EventBus — центральная шина событий между C++ модулями.
//
// Правила:
//   1. Модули общаются только через шину — не держат указатели друг на друга.
//   2. Для новых модулей (джойстик, батарея...) использовать moduleEvent() —
//      EventBus.h при этом не нужно менять.
//   3. Типизированные сигналы добавляются только когда событие затрагивает
//      несколько существующих модулей одновременно.
//
// Пример нового модуля (EventBus.h не трогать):
//   emit m_bus->moduleEvent("joystick", "connected", {{"id",0}, {"name","Xbox"}});
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

class EventBus : public QObject
{
    Q_OBJECT
public:
    explicit EventBus(QObject *parent = nullptr) : QObject(parent) {}

signals:
    // ── Сетевая установка: NetworkListener → TileManager ─────────────────────
    // Эмитируется NetworkListener после успешной распаковки 7z-архива.
    // TileManager подписывается и вызывает свой finalizeInstallation().
    void networkExtractFinished(const QString &tileId,
                                const QString &title,
                                const QString &gameRootPath,
                                const QString &exeRelPath,
                                const QString &iconRelPath,
                                const QString &kind);

    // ── Настройки ─────────────────────────────────────────────────────────────
    // Эмитируется ModuleHost когда любой параметр в SettingsManager меняется.
    // Модули кешируют нужные значения и обновляют кеш по этому сигналу.
    void settingChanged(const QString &key, const QVariant &value);

    // ── Универсальный канал для новых модулей ─────────────────────────────────
    // Новый модуль НЕ трогает EventBus.h.
    // Подписка с фильтрацией по namespace:
    //
    //   connect(bus, &EventBus::moduleEvent, this,
    //       [](const QString &mod, const QString &ev, const QVariantMap &d) {
    //           if (mod != "joystick") return;
    //           if (ev == "connected") handleConnected(d["id"].toInt());
    //       });
    void moduleEvent(const QString &module,
                     const QString &event,
                     const QVariantMap &data);
};
