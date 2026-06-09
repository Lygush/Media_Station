// modules/OverlayModule/OverlayModule.h
#pragma once

#include "core/IModule.h"
#include "core/EventBus.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace Overlay {
constexpr auto kModule          = "overlay";
constexpr auto kEventShow       = "show";
constexpr auto kEventHide       = "hide";
constexpr auto kEventVisibility = "visibilityChanged";
constexpr auto kContextAndroidTV = "androidtv";
constexpr auto kContextGame      = "game";

inline QVariantMap makeShowEvent(const QString &ctx) {
    return QVariantMap{{ QStringLiteral("context"), ctx }};
}
inline QVariantMap makeVisibilityEvent(bool visible, const QString &ctx) {
    return QVariantMap{
        { QStringLiteral("visible"), visible },
        { QStringLiteral("context"), ctx }
    };
}
}

class OverlayModule : public QObject, public IModule
{
    Q_OBJECT
    Q_PROPERTY(bool    visible READ isVisible NOTIFY visibleChanged)
    Q_PROPERTY(QString context READ context   NOTIFY contextChanged)

public:
    explicit OverlayModule(QObject *parent = nullptr);

    QString  moduleId() const override { return QStringLiteral("overlay"); }
    QString  qmlName()  const override { return QStringLiteral("OverlayModule"); }
    QObject *qmlObject()      override { return this; }

    void initialize(EventBus *bus, SettingsManager *settings) override;
    void shutdown()                                           override;
    void registerWithQml(QQmlApplicationEngine *engine)      override;

    bool    isVisible() const { return m_visible; }
    QString context()   const { return m_context; }

public slots:
    Q_INVOKABLE void show(const QString &context = QLatin1String(Overlay::kContextAndroidTV));
    Q_INVOKABLE void hide();
    Q_INVOKABLE void exitAndroidTV();

signals:
    void visibleChanged();
    void contextChanged();
    void overlayKeyPressed(const QString &key);

private:
    void handleBusEvent(const QString &mod, const QString &ev, const QVariantMap &data);
    void notifyVisibility(bool visible);

    EventBus *m_bus     = nullptr;
    bool      m_visible = false;
    QString   m_context;
    bool      m_shutdownDone = false;
};
