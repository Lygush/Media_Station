// modules/OverlayModule/OverlayModule.cpp
#include "OverlayModule.h"
#include "G72KeysModule.h"   // для G72Keys::k* констант

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDebug>
#include <QTimer>   // для отложенного закрытия

OverlayModule::OverlayModule(QObject *parent) : QObject(parent) {}

// ── IModule ───────────────────────────────────────────────────────────────────

void OverlayModule::initialize(EventBus *bus, SettingsManager *)
{
    m_bus = bus;
    connect(bus, &EventBus::moduleEvent, this, &OverlayModule::handleBusEvent);
    qDebug() << "[Overlay] initialized";
}

void OverlayModule::shutdown()
{
    if (m_shutdownDone) return;
    m_shutdownDone = true;
    if (m_visible) hide();
}

void OverlayModule::registerWithQml(QQmlApplicationEngine *engine)
{
    if (engine)
        engine->rootContext()->setContextProperty(qmlName(), this);
}

// ── Публичный API ─────────────────────────────────────────────────────────────

void OverlayModule::show(const QString &context)
{
    // Обновляем контекст
    if (m_context != context) {
        m_context = context;
        emit contextChanged();
    }

    // Если уже видим — ничего не делаем (избегаем двойных событий на шину)
    if (m_visible) return;

    m_visible = true;
    emit visibleChanged();
    notifyVisibility(true);

    qDebug() << "[Overlay] shown, context:" << context;
}

void OverlayModule::hide()
{
    if (!m_visible) return;

    m_visible = false;
    emit visibleChanged();
    notifyVisibility(false);

    qDebug() << "[Overlay] hidden";
}

void OverlayModule::exitAndroidTV()
{
    // ВАЖЕН ПОРЯДОК:
    // Сначала уведомляем AndroidTVModule о запросе выхода (он установит m_exitRequested = true),
    // и только потом закрываем оверлей. Иначе AndroidTVModule в обработчике
    // visibilityChanged(false) увидит m_exitRequested == false и вызовет
    // bringEmulatorBackAfterOverlay() — эмулятор мигнёт перед остановкой.
    if (m_bus) {
        emit m_bus->moduleEvent(
            QStringLiteral("androidtv"),
            QStringLiteral("stopRequested"),
            QVariantMap{});
    }

    // Теперь закрываем оверлей — AndroidTVModule уже знает о выходе
    hide();

    qDebug() << "[Overlay] exitAndroidTV requested";
}

// ── Приватные методы ──────────────────────────────────────────────────────────

void OverlayModule::notifyVisibility(bool visible)
{
    if (!m_bus) return;
    emit m_bus->moduleEvent(
        QLatin1String(Overlay::kModule),
        QLatin1String(Overlay::kEventVisibility),
        Overlay::makeVisibilityEvent(visible, m_context));
}

// ── Обработчик событий шины ───────────────────────────────────────────────────
void OverlayModule::handleBusEvent(
    const QString &mod, const QString &ev, const QVariantMap &data)
{
    // ── Входящий запрос: показать оверлей ────────────────────────────────────
    if (mod == QLatin1String(Overlay::kModule)) {
        if (ev == QLatin1String(Overlay::kEventShow)) {
            show(data.value(QStringLiteral("context"),
                            QLatin1String(Overlay::kContextAndroidTV)).toString());
            return;
        }
        if (ev == QLatin1String(Overlay::kEventHide)) {
            hide();
            return;
        }
    }

    // ── Кнопки пульта — только когда оверлей открыт ──────────────────────────
    if (!m_visible) return;

    if (mod != QLatin1String(G72Keys::kModule) ||
        ev  != QLatin1String(G72Keys::kEventKey)) return;

    const QString key = data.value(QStringLiteral("key")).toString();

    // Кнопки закрытия оверлея — откладываем закрытие, чтобы другие обработчики
    // (например, AndroidTVModule) успели увидеть m_visible == true и не отправили
    // эту клавишу в эмулятор.
    if (key == QLatin1String(G72Keys::kBack) ||
        key == QLatin1String(G72Keys::kMenu))
    {
        QTimer::singleShot(0, this, &OverlayModule::hide);
        return;
    }

    // Навигационные кнопки — пробрасываем в QML для управления фокусом
    // QML-компонент OverlayScreen.qml подключается к этому сигналу
    emit overlayKeyPressed(key);
}
