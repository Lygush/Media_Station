// notifications/NotificationModule.cpp
#include "NotificationModule.h"
#include "core/EventBus.h"

#include <QTimer>
#include <QDebug>

#ifdef DEBUG_NOTIFICATIONS
#include <QRandomGenerator>
#endif

// ─────────────────────────────────────────────────────────────────────────────

NotificationModule::NotificationModule(QObject *parent)
    : QObject(parent)
{
    m_autoDismissTimer = new QTimer(this);
    m_autoDismissTimer->setSingleShot(true);
    connect(m_autoDismissTimer, &QTimer::timeout, this, &NotificationModule::dismiss);
}

// ── IModule ───────────────────────────────────────────────────────────────────

void NotificationModule::initialize(EventBus *bus, SettingsManager * /*settings*/)
{
#ifdef DEBUG_NOTIFICATIONS
    m_bus = bus;
#endif

    connect(bus, &EventBus::moduleEvent, this,
        [this](const QString &mod, const QString &ev, const QVariantMap &data)
        {
            if (mod != Notifications::kModule || ev != Notifications::kEventShow) return;

            NotificationItem item;
            item.level       = Notifications::levelFromString(data.value("level", "info").toString());
            item.message     = data.value("message").toString();
            item.autoDismiss = data.value("autoDismiss", false).toBool();
            item.duration    = data.value("duration", 4000).toInt();

            if (item.message.isEmpty()) {
                qWarning() << "NotificationModule: received empty message, ignoring";
                return;
            }

            enqueue(item);
        });

#ifdef DEBUG_NOTIFICATIONS
    startDebugTimer();
#endif
}

void NotificationModule::shutdown()
{
    m_autoDismissTimer->stop();
    m_queue.clear();
    m_visible = false;
    emit visibleChanged();
}

// ── Очередь ───────────────────────────────────────────────────────────────────

void NotificationModule::enqueue(const NotificationItem &item)
{
    // Одинаковые сообщения подряд (например, повторяющиеся ошибки сети) не
    // копим — иначе пользователю приходилось закрывать десятки копий
    if (m_visible && m_current.message == item.message && m_current.level == item.level)
        return;
    for (const NotificationItem &q : std::as_const(m_queue))
        if (q.message == item.message && q.level == item.level) return;
    static constexpr int kMaxQueue = 20;
    if (m_queue.size() >= kMaxQueue) {
        qWarning() << "NotificationModule: queue full, dropping:" << item.message;
        return;
    }

    if (!m_visible) {
        // Ничего не показывается — показываем немедленно.
        m_current = item;
        m_visible = true;
        emit currentChanged();
        emit visibleChanged();
        emit showRequested();
        scheduleAutoDismiss();
        return;
    }

    // Вытеснение: новое уведомление строго выше уровнем текущего.
    if (static_cast<int>(item.level) > static_cast<int>(m_current.level)) {
        m_autoDismissTimer->stop();
        // Временные Info-уведомления при вытеснении теряются — они не важны.
        // Постоянные (без автоскрытия) и Warning/Error возвращаются в начало
        // очереди: раньше постоянное Info («Логи отправлены» и т.п.) пропадало
        // бесследно, если в этот момент приходила ошибка.
        if (m_current.level != NotificationLevel::Info || !m_current.autoDismiss)
            m_queue.prepend(m_current);
        m_current = item;
        emit currentChanged();
        // visibleChanged не нужен — уведомление уже видно, QML обновит данные.
        // Запрашиваем у QML «рестарт» анимации появления.
        emit showRequested();
        scheduleAutoDismiss();
        return;
    }

    // Иначе — встаём в хвост очереди.
    m_queue.enqueue(item);
}

void NotificationModule::scheduleAutoDismiss()
{
    m_autoDismissTimer->stop();
    if (m_current.autoDismiss)
        m_autoDismissTimer->start(m_current.duration);
}

// ── Закрытие ─────────────────────────────────────────────────────────────────

void NotificationModule::dismiss()
{
    if (!m_visible) return;
    m_autoDismissTimer->stop();
    emit hideRequested();   // QML запускает анимацию; по её окончании вызывает onDismissed()
}

void NotificationModule::onDismissed()
{
    m_visible = false;
    emit visibleChanged();

    if (!m_queue.isEmpty()) {
        m_current = m_queue.dequeue();
        m_visible = true;
        emit currentChanged();
        emit visibleChanged();
        emit showRequested();
        scheduleAutoDismiss();
    }
}

// ── DEBUG ─────────────────────────────────────────────────────────────────────

#ifdef DEBUG_NOTIFICATIONS
void NotificationModule::startDebugTimer()
{
    m_debugTimer = new QTimer(this);
    m_debugTimer->setInterval(3000);

    static const QStringList messages = {
        "Диск C: заполнен на 90%",
        "Игра успешно установлена",
        "Ошибка подключения к серверу",
        "Обновление доступно",
        "Джойстик отключён",
        "Файл конфигурации не найден",
        "Установка завершена успешно",
        "Критическая ошибка запуска",
    };

    connect(m_debugTimer, &QTimer::timeout, this, [this]() {
        int lvlIdx = QRandomGenerator::global()->bounded(3);
        int msgIdx = QRandomGenerator::global()->bounded(messages.size());
        bool autoDismiss = QRandomGenerator::global()->bounded(2) == 0;

        static const NotificationLevel lvlMap[] = {
            NotificationLevel::Info, NotificationLevel::Warning, NotificationLevel::Error
        };
        emit m_bus->moduleEvent(
            Notifications::kModule,
            Notifications::kEventShow,
            Notifications::makeEvent(messages[msgIdx], lvlMap[lvlIdx], autoDismiss, 4000));
    });

    m_debugTimer->start();
    qDebug() << "NotificationModule: DEBUG mode active — random notifications every 3s";
}
#endif
