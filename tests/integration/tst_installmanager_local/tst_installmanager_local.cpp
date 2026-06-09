#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include "InstallManager.h"
#include "EventBus.h"

class tst_InstallManagerLocal : public QObject
{
    Q_OBJECT

private slots:
    void init() {
        sourceDir = new QTemporaryDir();
        targetBase = new QTemporaryDir();
        QVERIFY(sourceDir->isValid());
        QVERIFY(targetBase->isValid());

        // Создаём тестовые файлы в sourceDir
        QDir src(sourceDir->path());
        src.mkdir("sub");
        QFile exe(src.filePath("game.exe"));
        exe.open(QIODevice::WriteOnly);
        exe.write("fake exe content");
        exe.close();
        QFile exe2(src.filePath("sub/helper.exe"));
        exe2.open(QIODevice::WriteOnly);
        exe2.write("helper");
        exe2.close();
        QFile icon(src.filePath("icon.png"));
        icon.open(QIODevice::WriteOnly);
        icon.write("fake icon");
        icon.close();

        // Настраиваем менеджер
        installer = new InstallManager(this);
        installer->initialize(&bus, nullptr); // SettingsManager не нужен, передаём nullptr
        // В InstallManager есть m_appDir – он берётся из QCoreApplication::applicationDirPath().
        // Для теста мы не можем его изменить. Но можно сделать setter.
        // В реальном коде добавьте метод setAppDirForTesting(const QString &path).
        // Пока пропустим.
    }
    void cleanup() {
        delete installer;
        delete sourceDir;
        delete targetBase;
    }

    void testLocalInstall();
    void testCancelInstallation();

private:
    QTemporaryDir *sourceDir;
    QTemporaryDir *targetBase;
    InstallManager *installer;
    EventBus bus;
};

void tst_InstallManagerLocal::testLocalInstall()
{
    QSignalSpy spyProgress(installer, &InstallManager::installationProgress);
    QSignalSpy spyFinished(installer, &InstallManager::installationFinished);

    installer->startInstallation("tile123", "TestGame",
                                 sourceDir->path(), "game.exe",
                                 sourceDir->path() + "/icon.png",
                                 targetBase->path());

    // Ждём завершения (максимум 10 секунд)
    QTRY_COMPARE(spyFinished.count(), 1);

    bool success = spyFinished.first().at(1).toBool();
    QVERIFY(success);

    QString installFolder = spyFinished.first().at(4).toString();
    QVERIFY(QFile::exists(installFolder + "/launcher/launch.ps1"));
    QVERIFY(QFile::exists(installFolder + "/game/game.exe"));
    QVERIFY(QFile::exists(installFolder + "/game_info.json"));

    // Прогресс должен был обновляться
    QVERIFY(spyProgress.count() >= 1);
}

void tst_InstallManagerLocal::testCancelInstallation()
{
    QSignalSpy spyCancelled(installer, &InstallManager::installationCancelled);
    QSignalSpy spyFinished(installer, &InstallManager::installationFinished);

    installer->startInstallation("tile456", "CancelGame",
                                 sourceDir->path(), "game.exe",
                                 sourceDir->path() + "/icon.png",
                                 targetBase->path());

    // Даём немного времени на начало копирования
    QTest::qWait(100);
    installer->cancelInstallation("tile456");

    QTRY_COMPARE(spyCancelled.count(), 1);
    QCOMPARE(spyFinished.count(), 0);
}

QTEST_MAIN(tst_InstallManagerLocal)
#include "tst_installmanager_local.moc"