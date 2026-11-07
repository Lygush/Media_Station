#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QStandardPaths>
#include "GameLibraryModule.h"
#include "SettingsManager.h"
#include "EventBus.h"
#include "InstallUtils.h"
#include "SevenZip.h"
#include "WofCompact.h"

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
    void testSizeCacheInvalidatedOnChange();
    void testUnpackedSize();
    void testCompactFileLine();
    void testCompressGame();
    void testParseLaunchScript();

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

// Размер кэшируется вместе с отпечатком папки; после изменения содержимого
// (обновление игры) кэш сбрасывается и размер пересчитывается
void tst_GameLibraryModuleScan::testSizeCacheInvalidatedOnChange()
{
    const QString gamePath = gamesDir->path() + "/TestGame";
    QDir().mkpath(gamePath + "/game/data");
    { QFile f(gamePath + "/game/data/a.bin"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(1000, 'x')); }

    auto entryOf = [&](const QVariantList &entries) {
        for (const QVariant &v : entries) if (v.toMap()["name"] == "TestGame") return v.toMap();
        return QVariantMap();
    };

    // 1. Скан → размера нет → считаем; сохраняется вместе с отпечатком
    QSignalSpy scan(module, &GameLibraryModule::scanFinished);
    module->scanLibrary();
    QTRY_COMPARE(scan.count(), 1);
    QVariantMap e = entryOf(scan.takeFirst().first().toList());
    QCOMPARE(e["sizeCached"].toLongLong(), -1LL);
    QSignalSpy size(module, &GameLibraryModule::folderSizeReady);
    module->requestFolderSize(e["id"].toString(), gamePath + "/game");
    QTRY_COMPARE(size.count(), 1);
    QCOMPARE(size.first().at(1).toLongLong(), 1000LL);

    QFile info(gamePath + "/game_info.json");
    QVERIFY(info.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(info.readAll()).object();
    info.close();
    QCOMPARE(saved["sizeCached"].toInteger(), 1000);
    QVERIFY(!saved["sizeStamp"].toString().isEmpty());

    // 2. Повторный скан без изменений — размер из кэша
    module->scanLibrary();
    QTRY_COMPARE(scan.count(), 1);
    QCOMPARE(entryOf(scan.takeFirst().first().toList())["sizeCached"].toLongLong(), 1000LL);

    // 3. «Обновление игры» — новая папка внутри game/ → кэш сброшен
    QThread::msleep(20);
    QDir().mkpath(gamePath + "/game/patch");
    module->scanLibrary();
    QTRY_COMPARE(scan.count(), 1);
    QCOMPARE(entryOf(scan.takeFirst().first().toList())["sizeCached"].toLongLong(), -1LL);
}

// Размер распакованного архива (для проверки места до распаковки).
// Нужен 7z: в Linux-окружении теста — системный, подложенный как tools/7zr.exe
void tst_GameLibraryModuleScan::testUnpackedSize()
{
    const QString sys7z = QStandardPaths::findExecutable("7z");
    if (sys7z.isEmpty()) QSKIP("7z not installed");
    const QString tools = QCoreApplication::applicationDirPath() + "/tools";
    QDir().mkpath(tools);
    QFile::remove(tools + "/7zr.exe");
    QVERIFY(QFile::link(sys7z, tools + "/7zr.exe"));

    QTemporaryDir src;
    QDir().mkpath(src.path() + "/game/sub");
    { QFile f(src.path() + "/game/a.bin");     QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(300000, 'a')); }
    { QFile f(src.path() + "/game/sub/b.bin"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(12345, 'b')); }
    const QString archive = src.path() + "/g.7z";
    QCOMPARE(SevenZip::compress(src.path(), "game", archive), SevenZip::Ok);
    QCOMPARE(SevenZip::unpackedSize(archive), qint64(300000 + 12345));
    QCOMPARE(SevenZip::unpackedSize(src.path() + "/missing.7z"), qint64(-1));
    QFile::remove(tools + "/7zr.exe");
}

QTEST_MAIN(tst_GameLibraryModuleScan)
#include "tst_gamelibrarymodule_scan.moc"

void tst_GameLibraryModuleScan::testCompactFileLine()
{
    QVERIFY(WofCompact::isFileLine(QStringLiteral("game.exe            1 234 567 :     456 789 = 2,7 к 1 [OK]")));
    QVERIFY(WofCompact::isFileLine(QStringLiteral("data.pak 1,234,567 : 456,789 = 2.7 to 1 [OK]")));
    QVERIFY(WofCompact::isFileLine(QString::fromUtf8("a.bin 1\u00a0000 : 500 = 2,0 к 1 [OK]")));
    QVERIFY(!WofCompact::isFileLine(QStringLiteral(" Сжатие файлов в D:\\Games\\X\\game\\")));
    QVERIFY(!WofCompact::isFileLine(QStringLiteral("Обработано 12 файлов в 3 каталогах.")));
    QVERIFY(!WofCompact::isFileLine(QString()));
}

// compact.exe подменяется скриптом: проверяем аргументы, рабочую папку,
// прогресс по строкам вывода, флаг compressed в game_info.json и снятие сжатия
void tst_GameLibraryModuleScan::testCompressGame()
{
#ifdef Q_OS_WIN
    QSKIP("fake compact is a shell script");
#else
    const QString gamePath = gamesDir->path() + "/TestGame";
    for (int i = 0; i < 4; ++i) {
        QFile f(gamePath + QString("/game/f%1.bin").arg(i));
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(100, 'x'));
    }
    QTemporaryDir tools;
    const QString log = tools.path() + "/args.txt";
    const QString fake = tools.path() + "/compact";
    {
        QFile f(fake); QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QString("#!/bin/bash\necho \"$PWD|$*\" >> '%1'\n"
                        "echo ' Сжатие файлов в '$PWD\n"
                        "for x in *; do echo \"$x  100 :  50 = 2,0 к 1 [OK]\"; sleep 0.05; done\n"
                        "echo 'Обработано 4 файлов.'\nexit 0\n").arg(log).toUtf8());
    }
    QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    qputenv("MS_COMPACT_EXE", fake.toLocal8Bit());
    QVERIFY(module->compressAvailable());

    QSignalSpy scan(module, &GameLibraryModule::scanFinished);
    module->scanLibrary();
    QTRY_COMPARE(scan.count(), 1);
    QString id;
    for (const QVariant &v : scan.first().first().toList())
        if (v.toMap()["name"] == "TestGame") id = v.toMap()["id"].toString();
    QVERIFY(!id.isEmpty());

    QSignalSpy prog(module, &GameLibraryModule::compressProgress);
    QSignalSpy done(module, &GameLibraryModule::compressFinished);
    module->compressGame(id, true);
    // Во время сжатия архивация не запускается
    QSignalSpy arch(module, &GameLibraryModule::archivingChanged);
    module->archiveGame(id);
    QCOMPARE(arch.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
    QCOMPARE(done.first().at(1).toBool(), true);    // success
    QCOMPARE(done.first().at(2).toBool(), true);    // compressed
    QVERIFY(prog.count() >= 2);                      // 0 и промежуточные

    QFile argsFile(log); QVERIFY(argsFile.open(QIODevice::ReadOnly));
    const QString first = QString::fromUtf8(argsFile.readLine()).trimmed();
    argsFile.close();
    QCOMPARE(first, QDir(gamePath + "/game").absolutePath() + "|/c /s /a /i /exe:lzx");

    auto readInfo = [&]() {
        QFile info(gamePath + "/game_info.json"); info.open(QIODevice::ReadOnly);
        return QJsonDocument::fromJson(info.readAll()).object();
    };
    QJsonObject saved = readInfo();
    QCOMPARE(saved["compressed"].toBool(), true);
    QCOMPARE(saved["sizeCached"].toInteger(), 400);   // вне Windows — логический размер

    // Состояние переживает пересканирование
    scan.clear();
    module->scanLibrary();
    QTRY_COMPARE(scan.count(), 1);
    for (const QVariant &v : scan.first().first().toList())
        if (v.toMap()["id"] == id) QCOMPARE(v.toMap()["compressed"].toBool(), true);

    done.clear();
    module->compressGame(id, false);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
    QCOMPARE(done.first().at(2).toBool(), false);
    saved = readInfo();
    QCOMPARE(saved["compressed"].toBool(), false);
    QVERIFY(!saved.contains("sizeBeforeCompress"));
    QVERIFY(argsFile.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(argsFile.readAll()).trimmed().split('\n');
    QCOMPARE(lines.last().section('|', 1), QString("/u /s /a /i /exe"));
    qunsetenv("MS_COMPACT_EXE");
#endif
}

// launch.ps1 → exe и рабочая папка для запуска без PowerShell
void tst_GameLibraryModuleScan::testParseLaunchScript()
{
    QTemporaryDir d;
    const QString root = d.path() + "/Assassin's Creed [GOTY]";
    QString exe, dir;

    QVERIFY(!InstallUtils::createLaunchScript(root, "bin/x64/Game's.exe", "app").isEmpty());
    QVERIFY(InstallUtils::parseLaunchScript(root + "/launcher/launch.ps1", &exe, &dir));
    QCOMPARE(exe, QDir::cleanPath(root + "/game/bin/x64/Game's.exe"));
    QCOMPARE(dir, QDir::cleanPath(root + "/game"));

    const QString ext = d.path() + "/Ext";
    QVERIFY(!InstallUtils::createLaunchScript(ext, "C:/Program Files/Tool's/run.exe", "external").isEmpty());
    QVERIFY(InstallUtils::parseLaunchScript(ext + "/launcher/launch.ps1", &exe, &dir));
    QCOMPARE(exe, QString("C:/Program Files/Tool's/run.exe"));
    // Папка exe — как её записал createLaunchScript (на Windows это C:/Program Files/Tool's)
    QCOMPARE(dir, QDir::cleanPath(QFileInfo("C:/Program Files/Tool's/run.exe").absolutePath()));

    // Неподдерживаемый тип — не запускаем напрямую
    const QString bad = d.path() + "/Bad";
    InstallUtils::createLaunchScript(bad, "x.cue", "ps1");
    QVERIFY(!InstallUtils::parseLaunchScript(bad + "/launcher/launch.ps1", &exe, &dir));

    // Скрипт изменён вручную (своя команда запуска) — тоже через PowerShell
    QFile f(root + "/launcher/launch.ps1");
    QVERIFY(f.open(QIODevice::ReadOnly));
    QString text = QString::fromUtf8(f.readAll());
    f.close();
    text.replace("Start-Process -FilePath $ExePath -WorkingDirectory $WorkDir",
                 "Start-Process -FilePath $ExePath -ArgumentList '-dx11' -WorkingDirectory $WorkDir");
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(text.toUtf8());
    f.close();
    QVERIFY(!InstallUtils::parseLaunchScript(root + "/launcher/launch.ps1", &exe, &dir));
    QVERIFY(!InstallUtils::parseLaunchScript(d.path() + "/missing.ps1", &exe, &dir));
}
