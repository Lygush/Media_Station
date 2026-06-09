// notifications/NotificationModule.h
// ─────────────────────────────────────────────────────────────────────────────
// NotificationModule — модуль уведомлений.
//
// Принимает уведомления через EventBus::moduleEvent("notification", "show", ...)
// и управляет очередью с приоритетами.
//
// Отправка уведомления из любого модуля:
//
//   #include "NotificationModule.h"
//
//   emit bus->moduleEvent(
//       Notifications::kModule,
//       Notifications::kEventShow,
//       Notifications::makeEvent("Установка завершена", NotificationLevel::Info, true));
//
// Логика очереди:
//   - Одно уведомление на экране в момент времени.
//   - Новые уведомления встают в очередь.
//   - Если приходит уведомление строго выше уровнем текущего — оно вытесняет
//     текущее (текущее возвращается в начало очереди), и показывается немедленно.
//
// DEBUG_NOTIFICATIONS:
//   Если определён макрос DEBUG_NOTIFICATIONS — при старте модуля запускается
//   таймер, который каждые 3 секунды отправляет случайное уведомление.
//   Включить: добавить DEFINES += DEBUG_NOTIFICATIONS в .pro / CMakeLists.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include "core/IModule.h"
#include <QObject>
#include <QVariantMap>
#include <QQueue>

class EventBus;
class SettingsManager;
class QTimer;

// ── Уровень уведомления ───────────────────────────────────────────────────────
enum class NotificationLevel { Info = 0, Warning = 1, Error = 2 };

// ── Данные одного уведомления ─────────────────────────────────────────────────
struct NotificationItem {
    NotificationLevel level       = NotificationLevel::Info;
    QString           message;
    bool              autoDismiss = false;
    int               duration    = 4000;
};

// ── Протокол moduleEvent для уведомлений — единственный источник правды ───────
// Вынесен из класса чтобы не конфликтовать с moc (Q_OBJECT + static члены).
namespace Notifications {

    inline constexpr const char *kModule    = "notification";
    inline constexpr const char *kEventShow = "show";

    inline QString levelToString(NotificationLevel l)
    {
        switch (l) {
            case NotificationLevel::Warning: return QStringLiteral("warning");
            case NotificationLevel::Error:   return QStringLiteral("error");
            default:                         return QStringLiteral("info");
        }
    }

    inline NotificationLevel levelFromString(const QString &s)
    {
        if (s == QLatin1String("warning")) return NotificationLevel::Warning;
        if (s == QLatin1String("error"))   return NotificationLevel::Error;
        return NotificationLevel::Info;
    }

    inline QVariantMap makeEvent(const QString    &message,
                                 NotificationLevel level       = NotificationLevel::Info,
                                 bool              autoDismiss = false,
                                 int               durationMs  = 4000)
    {
        return {
            { QStringLiteral("level"),       levelToString(level) },
            { QStringLiteral("message"),     message              },
            { QStringLiteral("autoDismiss"), autoDismiss          },
            { QStringLiteral("duration"),    durationMs           },
        };
    }

} // namespace Notifications

// ─────────────────────────────────────────────────────────────────────────────

class NotificationModule : public QObject, public IModule
{
    Q_OBJECT

    Q_PROPERTY(bool    visible      READ visible      NOTIFY visibleChanged)
    Q_PROPERTY(QString level        READ level        NOTIFY currentChanged)
    Q_PROPERTY(QString message      READ message      NOTIFY currentChanged)
    Q_PROPERTY(bool    autoDismiss  READ autoDismiss  NOTIFY currentChanged)
    Q_PROPERTY(int     duration     READ duration     NOTIFY currentChanged)

public:
    explicit NotificationModule(QObject *parent = nullptr);

    QString  moduleId() const override { return QStringLiteral("notification"); }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown() override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return QStringLiteral("NotificationModule"); }

    bool    visible()     const { return m_visible; }
    QString level()       const { return Notifications::levelToString(m_current.level); }
    QString message()     const { return m_current.message; }
    bool    autoDismiss() const { return m_current.autoDismiss; }
    int     duration()    const { return m_current.duration; }

    Q_INVOKABLE void onDismissed();
    Q_INVOKABLE void dismiss();

signals:
    void visibleChanged();
    void currentChanged();
    void showRequested();
    void hideRequested();

private:
    void enqueue(const NotificationItem &item);
    void showNext();
    void scheduleAutoDismiss();

    bool                     m_visible = false;
    bool                     m_showing = false;
    NotificationItem         m_current = {};
    QQueue<NotificationItem> m_queue;
    QTimer                  *m_autoDismissTimer = nullptr;

#ifdef DEBUG_NOTIFICATIONS
    QTimer   *m_debugTimer = nullptr;
    EventBus *m_bus        = nullptr;
    void      startDebugTimer();
#endif
};
