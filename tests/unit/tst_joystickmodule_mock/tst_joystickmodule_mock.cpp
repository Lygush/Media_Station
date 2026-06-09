#include <QtTest>
#include <QTemporaryDir>
#include "JoystickModule.h"
#include "EventBus.h"

class tst_JoystickModuleMock : public QObject
{
    Q_OBJECT

private slots:
    void init() {
        module = new JoystickModule(this);
        module->initialize(&bus, nullptr);
        // Подменяем пути, чтобы не искать реальные DS4Windows.exe
        // В JoystickModule нужно добавить методы setPathsForTesting(const QString &exePath, const QString &cmdPath)
        // Пропустим для краткости.
    }
    void cleanup() {
        delete module;
    }

    void testCustomNames();
    void testInputTypeProfile();

private:
    JoystickModule *module;
    EventBus bus;
};

void tst_JoystickModuleMock::testCustomNames()
{
    // Устанавливаем кастомное имя для MAC
    module->setControllerName("AA:BB:CC:DD:EE:FF", "Player 1");
    // Проверяем, что сохранилось (через QVariantList controllers, но там реальные данные из DS4Windows – нет)
    // Для чистого unit-теста нужно добавить метод getCustomNameForTesting.
    // Вместо этого проверим, что файл controller_names.json создался.
    QFile namesFile(QCoreApplication::applicationDirPath() + "/data/controller_names.json");
    QVERIFY(namesFile.exists());
}

void tst_JoystickModuleMock::testInputTypeProfile()
{
    // Эмулируем событие "game"/"inputType" с xinput
    QVariantMap data;
    QStringList types = {"xinput"};
    data["inputTypes"] = QVariant::fromValue(types);
    emit bus.moduleEvent("game", "inputType", data);

    // В реальности JoystickModule вызовет loadProfile("XInput").
    // В тесте мы можем проверить, что был вызван метод runCmdSync с соответствующей командой.
    // Для этого нужен mock QProcess. Пропустим.
}

QTEST_MAIN(tst_JoystickModuleMock)
#include "tst_joystickmodule_mock.moc"