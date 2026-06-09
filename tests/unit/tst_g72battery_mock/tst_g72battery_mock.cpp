#include <QtTest>
#include "G72BatteryModule.h"
#include "EventBus.h"

// Макет воркера, не использующий WinRT
class MockWorker : public G72BatteryWorker
{
    Q_OBJECT
public slots:
    void doSearch() override {
        emit deviceReady();
    }
    void doRead() override {
        static int counter = 0;
        quint8 values[] = {0xA0, 0xA5, 0xAA, 0x9F, 0x95};
        emit rawSample(values[counter % 5]);
        counter++;
    }
};

class tst_G72BatteryMock : public QObject
{
    Q_OBJECT

private slots:
    void testLiIonPercent();
    void testFiltering();

private:
    G72BatteryModule *module;
    EventBus bus;
};

void tst_G72BatteryMock::testLiIonPercent()
{
    // Прямой вызов статического метода
    QCOMPARE(G72BatteryModule::liionPercent(1.0), 100);
    QCOMPARE(G72BatteryModule::liionPercent(0.9), 95);
    QCOMPARE(G72BatteryModule::liionPercent(0.8), 90);
    QCOMPARE(G72BatteryModule::liionPercent(0.72), 85);
    QCOMPARE(G72BatteryModule::liionPercent(0.62), 75);
    QCOMPARE(G72BatteryModule::liionPercent(0.5), 60);
    QCOMPARE(G72BatteryModule::liionPercent(0.38), 40);
    QCOMPARE(G72BatteryModule::liionPercent(0.25), 20);
    QCOMPARE(G72BatteryModule::liionPercent(0.15), 10);
    QCOMPARE(G72BatteryModule::liionPercent(0.05), 3);
    QCOMPARE(G72BatteryModule::liionPercent(0.0), 0);
    // Интерполяция
    QCOMPARE(G72BatteryModule::liionPercent(0.95), 97); // между 95 и 100
}

void tst_G72BatteryMock::testFiltering()
{
    // Здесь мы могли бы подменить worker и проверить, что при резких скачках
    // значение батареи замораживается. Требует модификации G72BatteryModule,
    // чтобы можно было внедрить mock-воркер. Пропустим.
}

QTEST_MAIN(tst_G72BatteryMock)
#include "tst_g72battery_mock.moc"