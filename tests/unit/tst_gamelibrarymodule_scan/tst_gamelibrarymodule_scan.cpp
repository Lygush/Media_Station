#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include "GameLibraryModule.h"
#include "SettingsManager.h"
#include "EventBus.h"
#include "InstallUtils.h"

class tst_GameLibraryModuleScan : public QObject
{
    Q_OBJECT

private slots:
    void init() {
        gamesDir = new QTemporaryDir();
        QVERIFY(gamesDir->isValid());

        // Создаём папку игры с game_info.json
        QString gamePath = gamesDir->path() + "/TestGame";
        QDir().mkpath(gamePath + "/game");
        QDir().mkpath(gamePath + "/launcher");
        InstallUtils::writeGameInfoFile(gamePath, "tile1", "TestGame", "installed",
                                        "game.exe", gamePath + "/launcher/icon.png");

        // Создаём папку legacy (только tile_id.txt)
        QString legacyPath = gamesDir->path() + "/LegacyGame";
        QDir().mkpath(legacyPath + "/game");
        QDir().mkpath(legacyPath + "/launcher");
        QFile tileFile(legacyPath + "/tile_id.txt");
        tileFile.open(QIODevice::WriteOnly);
        tileFile.write("legacy_tile");
        tileFile.close();
        // Создаём пустой launch.ps1, чтобы определить exe
        QFile ps1(legacyPath + "/launcher/launch.ps1");
        ps1.open(QIODevice::WriteOnly);
        ps1.write("$ExePath = Join-Path $GameDir 'game.exe'");
        ps1.close();

        settings = new SettingsManager(this);
        settings->setInstallPath(gamesDir->path());

        module = new GameLibraryModule(this);
        module->initialize(&bus, settings);
    }
    void cleanup() {
        delete module;
        delete settings;
        delete gamesDir;
    }

    void testScanFindsInstalledGame();
    void testScanFindsLegacyGame();
    void testCurrentEntries();
    void testRescanDropsDeletedGames();

private:
    QTemporaryDir *gamesDir;
    SettingsManager *settings;
    GameLibraryModule *module;
    EventBus bus;
};

void tst_GameLibraryModuleScan::testScanFindsInstalledGame()
{
    QSignalSpy spyFinished(module, &GameLibraryModule::scanFinished);
    module->scanLibrary();

    QTRY_COMPARE(spyFinished.count(), 1);
    QVariantList entries = spyFinished.first().first().toList();
    QVERIFY(entries.size() >= 1);
    bool found = false;
    for (const QVariant &v : entries) {
        QVariantMap m = v.toMap();
        if (m["name"].toString() == "TestGame") {
            found = true;
            QCOMPARE(m["type"].toString(), QString("installed"));
            break;
        }
    }
    QVERIFY(found);
}

void tst_GameLibraryModuleScan::testScanFindsLegacyGame()
{
    QSignalSpy spyFinished(module, &GameLibraryModule::scanFinished);
    module->scanLibrary();

    QTRY_COMPARE(spyFinished.count(), 1);
    QVariantList entries = spyFinished.first().first().toList();
    bool found = false;
    for (const QVariant &v : entries) {
        QVariantMap m = v.toMap();
        if (m["name"].toString() == "LegacyGame") {
            found = true;
            // Для legacy-папки (только tile_id.txt) модуль генерирует
            // game_info.json, после чего метаданные считаются полными.
            QCOMPARE(m.value("legacyNoMetadata").toBool(), false);
            QCOMPARE(m.value("executablePath").toString(), QString("game.exe"));
            QVERIFY(QFile::exists(gamesDir->path() + "/LegacyGame/game_info.json"));
            break;
        }
    }
    QVERIFY(found);
}

void tst_GameLibraryModuleScan::testCurrentEntries()
{
    // Сначала просканируем, чтобы заполнить m_entries
    QSignalSpy spyFinished(module, &GameLibraryModule::scanFinished);
    module->scanLibrary();
    QTRY_COMPARE(spyFinished.count(), 1);

    QVariantList entries = module->currentEntries();
    QVERIFY(!entries.isEmpty());
    // Проверяем, что currentEntries не пустой и содержит те же данные
}

void tst_GameLibraryModuleScan::testRescanDropsDeletedGames()
{
    QSignalSpy spy(module, &GameLibraryModule::scanFinished);
    module->scanLibrary();
    QTRY_COMPARE(spy.count(), 1);
    const int before = module->currentEntries().size();
    QVERIFY(before >= 2);

    // Игру удалили с диска вручную — после пересканирования её быть не должно
    QVERIFY(QDir(gamesDir->path() + "/LegacyGame").removeRecursively());
    module->scanLibrary();
    QTRY_COMPARE(spy.count(), 2);
    QCOMPARE(module->currentEntries().size(), before - 1);
    for (const QVariant &v : module->currentEntries())
        QVERIFY(v.toMap().value("name").toString() != "LegacyGame");
}

QTEST_MAIN(tst_GameLibraryModuleScan)
#include "tst_gamelibrarymodule_scan.moc"