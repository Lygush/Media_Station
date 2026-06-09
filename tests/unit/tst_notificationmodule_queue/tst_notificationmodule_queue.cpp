#include <QtTest>
#include "NotificationModule.h"
#include "EventBus.h"

class tst_NotificationModuleQueue : public QObject
{
    Q_OBJECT

private slots:
    void init() {
        module = new NotificationModule(this);
        module->initialize(&bus, nullptr);
    }
    void cleanup() {
        delete module;
    }

    void testShowFirst();
    void testQueueOrder();
    void testPreemptionHigherLevel();
    void testAutoDismiss();

private:
    NotificationModule *module;
    EventBus bus;
};

void tst_NotificationModuleQueue::testShowFirst()
{
    QSignalSpy spyShow(module, &NotificationModule::showRequested);
    QSignalSpy spyVisible(module, &NotificationModule::visibleChanged);

    emit bus.moduleEvent("notification", "show",
                         Notifications::makeEvent("First", NotificationLevel::Info, false));

    QTRY_COMPARE(spyShow.count(), 1);
    QCOMPARE(module->visible(), true);
    QCOMPARE(module->message(), QString("First"));
}

void tst_NotificationModuleQueue::testQueueOrder()
{
    // Показываем первый
    emit bus.moduleEvent("notification", "show",
                         Notifications::makeEvent("First", NotificationLevel::Info, false));
    QTRY_COMPARE(module->visible(), true);

    // Второй того же уровня – должен встать в очередь
    emit bus.moduleEvent("notification", "show",
                         Notifications::makeEvent("Second", NotificationLevel::Info, false));

    // Закрываем первый
    module->dismiss();
    QTest::qWait(200); // ждём анимацию (в тесте onDismissed вызовется сразу)
    module->onDismissed();

    // Теперь должен показаться второй
    QTRY_COMPARE(module->message(), QString("Second"));
}

void tst_NotificationModuleQueue::testPreemptionHigherLevel()
{
    emit bus.moduleEvent("notification", "show",
                         Notifications::makeEvent("InfoMsg", NotificationLevel::Info, false));
    QTRY_COMPARE(module->message(), QString("InfoMsg"));

    // Приходит Error – должен вытеснить
    emit bus.moduleEvent("notification", "show",
                         Notifications::makeEvent("ErrorMsg", NotificationLevel::Error, false));

    QTRY_COMPARE(module->message(), QString("ErrorMsg"));
    // InfoMsg должен вернуться в начало очереди
    module->dismiss();
    module->onDismissed();
    QTRY_COMPARE(module->message(), QString("InfoMsg"));
}

void tst_NotificationModuleQueue::testAutoDismiss()
{
    QSignalSpy spyHide(module, &NotificationModule::hideRequested);
    emit bus.moduleEvent("notification", "show",
                         Notifications::makeEvent("Auto", NotificationLevel::Info, true, 500));

    QTRY_COMPARE(spyHide.count(), 1);
}

QTEST_MAIN(tst_NotificationModuleQueue)
#include "tst_notificationmodule_queue.moc"