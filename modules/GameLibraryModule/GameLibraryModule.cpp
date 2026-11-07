// GameLibraryModule.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Реализация GameLibraryModule.
// Использует 7zr.exe (tools/7zr.exe) — облегчённая версия 7-Zip
// только для формата .7z, LZMA2, максимальное сжатие.
// ─────────────────────────────────────────────────────────────────────────────

#include "GameLibraryModule.h"
#include "SettingsManager.h"
#include "core/EventBus.h"
#include "NotificationModule.h"
#include "tools/AtomicFile.h"
#include "tools/GameInfoStore.h"
#include "tools/SevenZip.h"
#include "tools/WofCompact.h"
#include "InstallUtils.h"

#include <QStorageInfo>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QDateTime>
#include <QDebug>
#include <QtConcurrent/QtConcurrentRun>
#include <QFuture>
#include <QFutureWatcher>
#include <QPointer>
#include <QThreadPool>
#include <QSet>
#include <QTimer>

// ═════════════════════════════════════════════════════════════════════════════
// Конструктор / деструктор
// ═════════════════════════════════════════════════════════════════════════════

GameLibraryModule::GameLibraryModule(QObject *parent)
    : QObject(parent)
    , m_appDir(QCoreApplication::applicationDirPath())
{}

GameLibraryModule::~GameLibraryModule()
{
    shutdown();
}

// ═════════════════════════════════════════════════════════════════════════════
// IModule
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::initialize(EventBus *bus, SettingsManager *settings)
{
    m_bus      = bus;
    m_settings = settings;

    // Подписываемся на ответы TileManager — он сообщает актуальный счётчик
    // плиток после создания, удаления, и в ответ на queryTileCounts.
    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data)
            {
                if (mod != "tileManager" || ev != "tileCountChanged") return;
                const QString gameId = data["gameId"].toString();
                const int     count  = data["count"].toInt();
                if (gameId.isEmpty() || !m_entries.contains(gameId)) return;
                m_entries[gameId]["tileCount"] = count;
                emit libraryEntryChanged(gameId, m_entries[gameId]);
            });

    // Даём остальным модулям (TileManager) подписаться на шину
    QTimer::singleShot(2000, this, &GameLibraryModule::recoverInterruptedArchives);
}

void GameLibraryModule::shutdown()
{
    for (auto &task : m_archiveTasks)
        if (task.cancelFlag) task.cancelFlag->store(true);
    for (auto &task : m_archiveTasks)
        if (task.watcher && task.watcher->isRunning()) task.watcher->waitForFinished();
    m_archiveTasks.clear();

    // Отменяем активные извлечения.
    for (auto &task : m_extractTasks)
        if (task.cancelFlag) task.cancelFlag->store(true);
    m_extractTasks.clear();

    // compact.exe сжимает файл за файлом — прерывание безопасно, уже сжатые
    // файлы остаются сжатыми и работают
    for (auto &task : m_compressTasks) {
        if (!task.process) continue;
        task.process->disconnect(this);
        task.process->kill();
        task.process->waitForFinished(2000);
    }
    m_compressTasks.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// Вспомогательные методы
// ═════════════════════════════════════════════════════════════════════════════

QString GameLibraryModule::makeGameId(const QString &folderPath)
{
    // Детерминированный ID: первые 12 символов MD5 от нормализованного пути.
    // Совпадает с алгоритмом в InstallManager::makeFolderGameId.
    return QString::fromLatin1(
               QCryptographicHash::hash(
                   QDir::fromNativeSeparators(folderPath).toUtf8(),
                   QCryptographicHash::Md5
                   ).toHex()
               ).left(12);
}

/*static*/ QString GameLibraryModule::sizeStamp(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return {};
    if (fi.isFile())
        return QString("f:%1:%2").arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    // Время изменения каталога меняется при добавлении/удалении/переименовании
    // в нём самом; подпапки первого уровня ловят большинство обновлений игр
    qint64 h = fi.lastModified().toMSecsSinceEpoch();
    const QFileInfoList subs = QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                                                        QDir::Name);
    for (const QFileInfo &d : subs)
        h = h * 31 + d.lastModified().toMSecsSinceEpoch() + qHash(d.fileName());
    return QString("d:%1:%2").arg(subs.size()).arg(h);
}

/*static*/ QString GameLibraryModule::sizeTargetFor(const QVariantMap &info)
{
    // Тот же путь, что считает экран библиотеки (GameLibraryScreen.sizeTargetPath)
    const QString type = info.value("type").toString();
    const QString ip   = QDir::fromNativeSeparators(info.value("installPath").toString());
    if (type == QLatin1String("archive"))   return QDir::fromNativeSeparators(info.value("archivePath").toString());
    if (type == QLatin1String("installed")) return ip.isEmpty() ? QString() : ip + "/game";
    return ip;
}

qint64 GameLibraryModule::calcFolderSize(const QString &path)
{
    // Если путь указывает на файл (например .7z архив) — берём размер файла напрямую.
    // QDirIterator по пути к файлу возвращает 0, поэтому нужна отдельная ветка.
    const QFileInfo fi(path);
    if (fi.isFile())
        return fi.size();

    qint64 total = 0;
    QDirIterator it(path,
                    QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        // Место на диске, а не логический размер: у сжатых (WOF) игр меньше
        total += WofCompact::sizeOnDisk(it.fileInfo());
    }
    return total;
}

// ── game_info.json ─────────────────────────────────────────────────────────

QVariantMap GameLibraryModule::readGameInfo(const QString &folderPath) const
{
    return GameInfoStore::read(folderPath).toVariantMap();
}

bool GameLibraryModule::writeGameInfo(const QString &folderPath, const QVariantMap &info) const
{
    if (folderPath.isEmpty()) return false;

    // Поля, за которые отвечает библиотека. Только их она перезаписывает.
    // Описание, обложки, тип ввода и т.п. пишут другие модули, часто в фоне;
    // в m_entries их значения могут быть устаревшими, поэтому для таких полей
    // берём то, что уже лежит в файле, а из m_entries — только если в файле
    // поля нет (например, при первом создании файла).
    static const QSet<QString> kOwned = {
        QStringLiteral("id"),            QStringLiteral("name"),
        QStringLiteral("type"),          QStringLiteral("executablePath"),
        QStringLiteral("installPath"),   QStringLiteral("iconPath"),
        QStringLiteral("tileLocations"), QStringLiteral("installDate"),
        QStringLiteral("archivePath"),   QStringLiteral("sizeCached"),
        QStringLiteral("sizeStamp"),     QStringLiteral("legacyNoMetadata"),
        QStringLiteral("compressed"),    QStringLiteral("sizeBeforeCompress")
    };

    // В m_entries есть служебные поля времени выполнения (_archiveProgress,
    // _savedTileLocs, available, tileCount…) — в файл они не попадают.
    auto isRuntimeKey = [](const QString &k) {
        return k.startsWith(QLatin1Char('_')) || k == QLatin1String("available")
            || k == QLatin1String("tileCount") || k == QLatin1String("archiving")
            || k == QLatin1String("archiveProgress") || k == QLatin1String("archiveCurrentFile")
            || k == QLatin1String("extracting") || k == QLatin1String("extractProgress")
            || k == QLatin1String("compressing") || k == QLatin1String("compressProgress")
            || k == QLatin1String("decompressing")
            // отметку прерванной архивации ставят и снимают отдельно — не
            // возвращаем её в файл из устаревшей копии в памяти
            || k == QLatin1String("archivePending");
    };

    const QJsonObject src = QJsonObject::fromVariantMap(info);
    const bool ok = GameInfoStore::modify(folderPath, [&](QJsonObject &obj) {
        for (auto it = src.begin(); it != src.end(); ++it) {
            const QString &k = it.key();
            if (isRuntimeKey(k))
                continue;
            if (kOwned.contains(k) || !obj.contains(k))
                obj.insert(k, it.value());
        }
        return true;
    });
    if (!ok)
        qWarning() << "GameLibraryModule: cannot write game_info.json in" << folderPath;
    return ok;
}

// ── Путь к архиву внутри папки игры ────────────────────────────────────────
// Структура: <installPath>/archive/<gameName>.7z
// Всё что относится к игре хранится внутри её папки.

QString GameLibraryModule::legacyArchivePath(const QString &installPath)
{
    const QString gameName = QDir(installPath).dirName();
    return installPath + "/archive/" + gameName + ".7z";
}

QString GameLibraryModule::archivePathForGame(const QString &installPath) const
{
    const QString dir = m_settings ? QDir::fromNativeSeparators(m_settings->archivePath()) : QString();
    if (dir.isEmpty()) return legacyArchivePath(installPath);
    // Общая папка архивов: к имени добавляем часть id (хэш пути) — игры с
    // одинаковым именем папки из разных мест не перезапишут архивы друг друга
    const QString gameName = QDir(installPath).dirName();
    return QDir(dir).filePath(gameName + "_" + makeGameId(QDir::fromNativeSeparators(installPath)).left(6) + ".7z");
}

QString GameLibraryModule::findExistingArchive(const QString &installPath) const
{
    const QString configured = archivePathForGame(installPath);
    if (QFile::exists(configured)) return configured;
    const QString legacy = legacyArchivePath(installPath);
    if (QFile::exists(legacy)) return legacy;
    return {};
}

// ── Генерация метаданных для legacy-игры (через tile_id.txt) ───────────────

QVariantMap GameLibraryModule::generateMetadataForLegacy(const QString &folderPath) const
{
    const QDir dir(folderPath);
    if (!dir.exists()) return {};

    const QString tileIdPath = folderPath + "/tile_id.txt";
    if (!QFile::exists(tileIdPath))
        return {};

    QString tileId;
    QFile f(tileIdPath);
    if (f.open(QIODevice::ReadOnly))
        tileId = QString::fromUtf8(f.readAll()).trimmed();

    const QString ps1Path = folderPath + "/launcher/launch.ps1";
    QString exeRelative;
    if (QFile::exists(ps1Path)) {
        QFile ps1(ps1Path);
        if (ps1.open(QIODevice::ReadOnly)) {
            const QString content = QString::fromUtf8(ps1.readAll());
            static const QRegularExpression rx(R"(\$ExePath\s*=\s*Join-Path\s+\$GameDir\s+'([^']+)')");
            auto m = rx.match(content);
            if (m.hasMatch()) exeRelative = m.captured(1);
        }
    }

    QString iconPath;
    const QDir launcherDir(folderPath + "/launcher");
    const QStringList icons = launcherDir.entryList(
        {"icon.png", "icon.jpg", "icon.ico", "icon.webp"}, QDir::Files);
    if (!icons.isEmpty())
        iconPath = QDir::toNativeSeparators(launcherDir.absoluteFilePath(icons.first()));

    const QString expectedArchive = findExistingArchive(folderPath);
    QString type;
    QString archivePath;
    if (!expectedArchive.isEmpty()) {
        type        = "archive";
        archivePath = QDir::toNativeSeparators(expectedArchive);
    } else if (QDir(folderPath + "/game").exists()) {
        type = "installed";
    } else {
        type = "external";
    }

    QVariantMap generated;
    generated["id"]               = makeGameId(QDir::fromNativeSeparators(folderPath));
    generated["name"]             = dir.dirName();
    generated["type"]             = type;
    generated["executablePath"]   = exeRelative;
    generated["installPath"]      = QDir::toNativeSeparators(folderPath);
    generated["iconPath"]         = iconPath;
    generated["tileLocations"]    = QVariantList{ tileId };
    generated["installDate"]      = "";
    generated["archivePath"]      = archivePath;
    generated["sizeCached"]       = -1;
    generated["available"]        = type != "archive" || !expectedArchive.isEmpty();
    generated["legacyNoMetadata"] = false;   // после генерации метаданные есть
    generated["tileCount"]        = -1;

    return generated;
}

// ── Сканирование одной папки (использует generateMetadataForLegacy) ────────

QVariantMap GameLibraryModule::scanFolder(const QString &folderPath) const
{
    const QDir dir(folderPath);
    if (!dir.exists()) return {};

    QVariantMap info = readGameInfo(folderPath);

    // ── Режим 1: game_info.json есть → читаем, проверяем актуальность ────────
    if (!info.isEmpty()) {
        const QString type = info.value("type").toString();

        if (type == "external") {
            const QString exePath = info.value("executablePath").toString();
            info["available"] = QFile::exists(exePath);
        } else if (type == "archive") {
            // Путь из game_info.json; если файла там нет — ищем в папке
            // архивов из настроек и внутри папки игры (прежнее место)
            const QString expectedArchive = findExistingArchive(folderPath);
            if (!QFile::exists(info.value("archivePath").toString())
                && !expectedArchive.isEmpty()) {
                info["archivePath"] = QDir::toNativeSeparators(expectedArchive);
            }
            info["available"] = QFile::exists(info.value("archivePath").toString());
        } else {
            // installed — проверяем что папка game/ существует и не пустая
            const QDir gameDir(folderPath + "/game");
            info["available"] = gameDir.exists()
                                && !gameDir.entryList(QDir::Files | QDir::Dirs
                                                      | QDir::NoDotAndDotDot).isEmpty();
        }

        if (!info.contains("id"))
            info["id"] = makeGameId(QDir::fromNativeSeparators(folderPath));
        info["installPath"] = QDir::toNativeSeparators(folderPath);

        // Кэш размера устарел (игру обновили, файлы добавили) — пересчитать.
        // Отпечаток — одно чтение каталога, а не обход всех файлов.
        if (info.value("sizeCached").toLongLong() >= 0) {
            const QString target = sizeTargetFor(info);
            if (!target.isEmpty() && info.value("sizeStamp").toString() != sizeStamp(target))
                info["sizeCached"] = static_cast<qlonglong>(-1);
        }
        if (!info.contains("tileCount"))
            info["tileCount"] = -1;
        // У legacy-игр, которые уже имеют JSON, флаг уже false
        if (!info.contains("legacyNoMetadata"))
            info["legacyNoMetadata"] = false;
        return info;
    }

    // ── Режим 2: tile_id.txt без game_info.json → legacy установка ───────────
    QVariantMap generated = generateMetadataForLegacy(folderPath);
    if (!generated.isEmpty()) {
        writeGameInfo(folderPath, generated);
        qDebug() << "GameLibraryModule: generated game_info.json for legacy:"
                 << dir.dirName();
        return generated;
    }

    // ── Режим 3: неизвестная папка — пропускаем ──────────────────────────────
    return {};
}

// ═════════════════════════════════════════════════════════════════════════════
// scanLibrary
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::scanLibrary()
{
    if (m_scanning) return;
    m_scanning = true;
    emit scanningChanged();

    const QString basePath = m_settings ? m_settings->installPath() : QString();

    // Собираем gameId активных задач до запуска фонового потока.
    // Папки с активными задачами пропускаем при скане — они изменяются прямо сейчас.
    QStringList activePaths;
    for (auto it = m_archiveTasks.constBegin(); it != m_archiveTasks.constEnd(); ++it) {
        if (m_entries.contains(it.key()))
            activePaths << QDir::fromNativeSeparators(
                m_entries[it.key()].value("installPath").toString());
    }
    for (auto it = m_extractTasks.constBegin(); it != m_extractTasks.constEnd(); ++it) {
        if (m_entries.contains(it.key()))
            activePaths << QDir::fromNativeSeparators(
                m_entries[it.key()].value("installPath").toString());
    }

    const QPointer<GameLibraryModule> self(this);
    QThreadPool::globalInstance()->start([this, self, basePath, activePaths]() {
        QVariantList results;

        if (!basePath.isEmpty()) {
            const QDir base(basePath);
            if (base.exists()) {
                const QFileInfoList entries = base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QFileInfo &entry : entries) {
                    const QString fp = QDir::fromNativeSeparators(entry.absoluteFilePath());

                    // Пропускаем папки с активными задачами.
                    if (activePaths.contains(fp)) continue;

                    QVariantMap info = scanFolder(fp);
                    if (info.isEmpty()) continue;

                    const QString gameId = info.value("id").toString();
                    results.append(info);

                    if (self) QMetaObject::invokeMethod(self.data(), [this, gameId, info]() {
                        // Не перезаписываем если идёт активная задача для этой игры.
                        if (!m_archiveTasks.contains(gameId) && !m_extractTasks.contains(gameId))
                            m_entries[gameId] = info;
                    }, Qt::QueuedConnection);
                }
            } else {
                qWarning() << "GameLibraryModule: install path not found:" << basePath;
            }
        }

        if (self) QMetaObject::invokeMethod(self.data(), [this, results]() {
            // Игры, которых больше нет на диске (удалены вручную, сменён путь
            // установки), раньше оставались в m_entries навсегда и появлялись
            // в currentEntries() и подсчёте плиток.
            QSet<QString> seen;
            for (const QVariant &v : results)
                seen.insert(v.toMap().value("id").toString());
            for (auto it = m_entries.begin(); it != m_entries.end(); ) {
                const bool busy = m_archiveTasks.contains(it.key())
                               || m_extractTasks.contains(it.key());
                if (!busy && !seen.contains(it.key()))
                    it = m_entries.erase(it);
                else
                    ++it;
            }

            m_scanning = false;
            emit scanningChanged();
            emit scanFinished(results);

            // Запрашиваем у TileManager актуальные счётчики плиток для всех игр.
            // Передаём tileLocations чтобы TileManager мог найти legacy-плитки
            // у которых id не "lib_<gameId>" а старый формат из tile_id.txt.
            if (m_bus && !m_entries.isEmpty()) {
                QVariantList entries;
                for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
                    QVariantMap e;
                    e["gameId"]        = it.key();
                    e["tileLocations"] = it.value().value("tileLocations");
                    entries.append(e);
                }
                emit m_bus->moduleEvent("gameLibrary", "queryTileCounts",
                                        QVariantMap{{ "entries", entries }});
            }
        }, Qt::QueuedConnection);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// requestFolderSize
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::requestFolderSize(const QString &gameId, const QString &folderPath)
{
    const QPointer<GameLibraryModule> self(this);
    QThreadPool::globalInstance()->start([this, self, gameId, folderPath]() {
        // Отпечаток — до подсчёта: изменения во время обхода дадут пересчёт в следующий раз
        const QString stamp = sizeStamp(folderPath);
        const qint64 bytes = calcFolderSize(folderPath);

        if (self) QMetaObject::invokeMethod(self.data(), [this, gameId, bytes, stamp]() {
            if (m_entries.contains(gameId)) {
                m_entries[gameId]["sizeCached"] = bytes;
                m_entries[gameId]["sizeStamp"]  = stamp;
                const QString ip = m_entries[gameId].value("installPath").toString();
                if (!ip.isEmpty()) writeGameInfo(ip, m_entries[gameId]);
            }
            emit folderSizeReady(gameId, bytes);
        }, Qt::QueuedConnection);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// currentEntries — состояние без скана диска
// ═════════════════════════════════════════════════════════════════════════════

QVariantList GameLibraryModule::currentEntries() const
{
    QVariantList result;
    for (const QVariantMap &entry : m_entries) {
        QVariantMap e = entry;

        // Проставляем флаги активных задач чтобы UI мог восстановить прогресс-бары.
        const QString gameId = e.value("id").toString();

        if (m_archiveTasks.contains(gameId)) {
            e["archiving"]        = true;
            e["archiveProgress"]  = e.value("_archiveProgress", 0.0);
            e["archiveCurrentFile"] = e.value("_archiveCurrentFile", QString());
        } else {
            e["archiving"]        = false;
            e["archiveProgress"]  = 0.0;
            e["archiveCurrentFile"] = QString();
        }

        if (m_compressTasks.contains(gameId)) {
            const CompressTask &ct = m_compressTasks[gameId];
            e["compressing"]      = true;
            e["decompressing"]    = !ct.compress;
            e["compressProgress"] = ct.progress;
        } else {
            e["compressing"]      = false;
            e["decompressing"]    = false;
            e["compressProgress"] = 0.0;
        }

        if (m_extractTasks.contains(gameId)) {
            e["extracting"]      = true;
            e["extractProgress"] = m_extractProgress.value(gameId, 0.0);
        } else {
            e["extracting"]      = false;
            e["extractProgress"] = 0.0;
        }

        result.append(e);
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// tileCount
// ═════════════════════════════════════════════════════════════════════════════

int GameLibraryModule::tileCount(const QString &gameId) const
{
    // Значение синхронизируется через шину (tileManager/tileCountChanged).
    // До первой синхронизации (сразу после скана) возвращает -1,
    // что QML интерпретирует как "неизвестно" — статус пересчитается
    // когда придёт tileCountChanged от TileManager.
    if (!m_entries.contains(gameId)) return 0;
    const QVariant v = m_entries[gameId].value("tileCount");
    if (!v.isValid()) return -1;   // ещё не синхронизировано
    return v.toInt();
}

// ═════════════════════════════════════════════════════════════════════════════
// createTile
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::createTile(const QString &gameId,
                                   const QString &storageKey,
                                   bool           isFolder)
{
    if (!m_entries.contains(gameId)) {
        qWarning() << "GameLibraryModule::createTile: gameId not found:" << gameId;
        emit tileCreated(gameId, false);
        return;
    }

    const QVariantMap &entry = m_entries[gameId];
    const QString type       = entry.value("type").toString();

    if (type == "archive") {
        qWarning() << "GameLibraryModule::createTile: cannot create tile for archive:" << gameId;
        emit tileCreated(gameId, false);
        return;
    }

    const QString installPath = entry.value("installPath").toString();
    const QString ps1Path     = QDir::toNativeSeparators(installPath + "/launcher/launch.ps1");

    const QVariantMap data {
        { "gameId",      gameId },
        { "storageKey",  storageKey },
        { "isFolder",    isFolder },
        { "title",       entry.value("name").toString() },
        { "iconPath",    entry.value("iconPath").toString() },
        { "ps1Path",     ps1Path },
        { "installPath", installPath }
    };

    if (m_bus)
        emit m_bus->moduleEvent("gameLibrary", "createTile", data);

    // Обновляем tileLocations в game_info.json
    QVariantList locs = m_entries[gameId].value("tileLocations").toList();
    if (!locs.contains(storageKey)) {
        locs.append(storageKey);
        m_entries[gameId]["tileLocations"] = locs;
        writeGameInfo(installPath, m_entries[gameId]);
    }

    emit tileCreated(gameId, true);
}

// ═════════════════════════════════════════════════════════════════════════════
// removeTilesFromStructure
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::removeTilesFromStructure(const QStringList &tileLocations,
                                                 const QString     &gameId)
{
    if (!m_bus) return;
    // Передаём gameId чтобы TileManager мог удалить плитку по "lib_<gameId>".
    QVariantMap data;
    data["tileLocations"] = QVariant(tileLocations);
    data["gameId"]        = gameId;
    emit m_bus->moduleEvent("gameLibrary", "removeTiles", data);
}

// ═════════════════════════════════════════════════════════════════════════════
// archiveGame
// Порядок: удалить плитки → 7zr a LZMA2 mx=9 → при успехе удалить папку
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::archiveGame(const QString &gameId)
{
    if (!m_entries.contains(gameId)) {
        emit archiveFinished(gameId, false);
        return;
    }
    if (m_archiveTasks.contains(gameId) || m_extractTasks.contains(gameId)
        || m_compressTasks.contains(gameId)) return;

    // Проверяем архиватор ДО удаления плиток — иначе при его отсутствии
    // плитки пропадали, а игра оставалась неархивированной.
    if (!SevenZip::isAvailable()) {
        qWarning() << "GameLibraryModule::archiveGame: 7zr.exe not found";
        if (m_bus)
            emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                    Notifications::makeEvent(
                                        "Архиватор 7zr.exe не найден в папке tools",
                                        NotificationLevel::Error, true, 5000));
        emit archiveFinished(gameId, false);
        return;
    }

    QVariantMap entry         = m_entries[gameId];
    const QString installPath = QDir::fromNativeSeparators(
        entry.value("installPath").toString());

    // Архив — в папку архивов из настроек (можно на другом диске) или, если
    // она не задана, внутрь папки игры: <installPath>/archive/<gameName>.7z
    const QString archiveFinal = archivePathForGame(installPath);
    const QString archiveDir   = QFileInfo(archiveFinal).absolutePath();
    // Пустую папку удаляем только свою (<игра>/archive), не общую папку архивов
    const bool    ownArchiveDir = (archiveFinal == legacyArchivePath(installPath));
    const qint64  knownSize     = entry.value("sizeCached").toLongLong();
    auto failReason = std::make_shared<QString>();

    // Сохраняем tileLocations до удаления — для восстановления при отмене.
    QStringList savedTileLocs;
    const QVariantList tileLocs = entry.value("tileLocations").toList();
    for (const QVariant &v : tileLocs)
        savedTileLocs << v.toString();
    m_entries[gameId]["_savedTileLocs"] = QVariant(savedTileLocs);

    // Список мест плиток записываем в файл ДО их удаления. Если программу
    // закроют или она упадёт во время архивации, обработчик завершения не
    // выполнится, и раньше плитки пропадали навсегда — при следующем запуске
    // recoverInterruptedArchives() вернёт их по этой отметке.
    {
        QJsonObject mark;
        mark["archivePending"] = QJsonArray::fromStringList(savedTileLocs);
        GameInfoStore::update(installPath, mark);
    }

    // 1. Удаляем плитки.
    removeTilesFromStructure(savedTileLocs, gameId);

    // 2. Запускаем архивирование.
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto *watcher   = new QFutureWatcher<bool>(this);

    ArchiveTask task;
    task.watcher    = watcher;
    task.cancelFlag = cancelFlag;
    m_archiveTasks.insert(gameId, task);
    emit archivingChanged();

    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, gameId, installPath, archiveDir, archiveFinal, cancelFlag,
             ownArchiveDir, failReason]()
            {
                const bool cancelled = cancelFlag->load();
                const bool ok        = !cancelled && watcher->result();

                m_archiveTasks.remove(gameId);
                emit archivingChanged();

                if (cancelled) {
                    QFile::remove(archiveFinal);
                    if (ownArchiveDir) QDir(archiveDir).rmdir(".");
                    restoreTilesAfterArchive(gameId);
                    qDebug() << "GameLibraryModule: archive cancelled, tiles restored:" << gameId;
                    if (m_bus) {
                        const QString name = m_entries.contains(gameId)
                        ? m_entries[gameId].value("name").toString() : gameId;
                        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                Notifications::makeEvent(
                                                    QString("Архивация «%1» отменена").arg(name),
                                                    NotificationLevel::Info, true, 3000));
                    }
                    emit archiveFinished(gameId, false);

                } else if (ok) {
                    if (m_entries.contains(gameId)) {
                        m_entries[gameId]["type"]          = QString("archive");
                        m_entries[gameId]["archivePath"]   = QDir::toNativeSeparators(archiveFinal);
                        m_entries[gameId]["tileLocations"] = QVariantList{};
                        m_entries[gameId]["sizeCached"]    = static_cast<qlonglong>(-1);
                        m_entries[gameId].remove("_archiveProgress");
                        m_entries[gameId].remove("_archiveCurrentFile");
                        // game_info.json остаётся в installPath — папка игры не удаляется.
                        writeGameInfo(installPath, m_entries[gameId]);
                    }
                    clearArchivePending(installPath);
                    // Удаляем только папку game/ — launcher/ и game_info.json остаются.
                    QDir(installPath + "/game").removeRecursively();
                    qDebug() << "GameLibraryModule: archived successfully:" << gameId;
                    if (m_bus) {
                        const QString name = m_entries.contains(gameId)
                        ? m_entries[gameId].value("name").toString() : gameId;
                        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                Notifications::makeEvent(
                                                    QString("«%1» архивирована").arg(name),
                                                    NotificationLevel::Info, true, 4000));
                    }
                    // Уведомляем QML об обновлённых метаданных — в т.ч. archivePath.
                    // QML подхватит archivePath и сразу запросит размер файла.
                    if (m_entries.contains(gameId))
                        emit libraryEntryChanged(gameId, m_entries[gameId]);
                    emit archiveFinished(gameId, true);

                } else {
                    // Не хватило места — архива ещё нет, удалять нечего
                    if (failReason->isEmpty()) QFile::remove(archiveFinal);
                    if (ownArchiveDir) QDir(archiveDir).rmdir(".");
                    // Игра осталась на месте — возвращаем её плитки (раньше
                    // они восстанавливались только при отмене).
                    restoreTilesAfterArchive(gameId);
                    qWarning() << "GameLibraryModule: archive failed:" << gameId;
                    if (m_bus) {
                        const QString name = m_entries.contains(gameId)
                        ? m_entries[gameId].value("name").toString() : gameId;
                        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                Notifications::makeEvent(
                                                    failReason->isEmpty()
                                                        ? QString("Ошибка архивации «%1»").arg(name)
                                                        : QString("«%1» не архивирована: %2").arg(name, *failReason),
                                                    NotificationLevel::Error, true, 7000));
                    }
                    emit archiveFinished(gameId, false);
                }

                watcher->deleteLater();
            });

    const QPointer<GameLibraryModule> self(this);
    QFuture<bool> future = QtConcurrent::run(
        [this, self, gameId, installPath, archiveDir, archiveFinal, cancelFlag,
         knownSize, failReason]() -> bool
        {
            if (cancelFlag->load()) return false;

            if (!SevenZip::isAvailable()) {
                qWarning() << "GameLibraryModule: 7zr.exe not found";
                return false;
            }

            if (!QDir().mkpath(archiveDir)) {
                *failReason = QString("не удалось создать папку %1").arg(QDir::toNativeSeparators(archiveDir));
                return false;
            }

            // Проверка места ДО начала: раньше нехватка обнаруживалась через
            // десятки минут сжатия. Сжатый архив не больше исходных файлов,
            // поэтому с запасом требуем их размер + 1 ГБ.
            qint64 needBytes = knownSize;
            if (needBytes <= 0) {
                needBytes = 0;
                QDirIterator it(installPath + "/game", QDir::Files | QDir::Hidden | QDir::System,
                                QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    if (cancelFlag->load()) return false;
                    it.next();
                    needBytes += it.fileInfo().size();
                }
            }
            needBytes += qint64(1) << 30;
            const QStorageInfo storage(archiveDir);   // свежие данные — при создании
            const qint64 freeBytes = storage.bytesAvailable();
            if (freeBytes >= 0 && freeBytes < needBytes) {
                auto gb = [](qint64 b) { return QString::number(double(b) / (1024.0 * 1024 * 1024), 'f', 1); };
                *failReason = QString("мало места на диске %1 — нужно до %2 ГБ, свободно %3 ГБ")
                                  .arg(storage.rootPath(), gb(needBytes), gb(freeBytes));
                qWarning() << "GameLibraryModule: archive aborted:" << *failReason;
                return false;
            }

            QFile::remove(archiveFinal);

            const SevenZip::Result r = SevenZip::compress(
                installPath,
                QStringLiteral("game"),
                archiveFinal,
                cancelFlag.get(),
                [this, self, gameId](int pct, const QString &curFile) {
                    const qreal progress = pct / 100.0;
                    if (self) QMetaObject::invokeMethod(self.data(),
                        [this, gameId, progress, curFile]() {
                            if (m_entries.contains(gameId)) {
                                m_entries[gameId]["_archiveProgress"]    = progress;
                                m_entries[gameId]["_archiveCurrentFile"] = curFile;
                            }
                            emit archiveProgress(gameId, progress, curFile);
                        }, Qt::QueuedConnection);
                });

            if (r == SevenZip::Cancelled) return false;
            if (r != SevenZip::Ok) {
                qWarning() << "GameLibraryModule: compress failed:"
                           << SevenZip::resultToString(r);
                return false;
            }
            return QFile::exists(archiveFinal);
        });

    watcher->setFuture(future);
}

void GameLibraryModule::restoreTilesAfterArchive(const QString &gameId)
{
    if (!m_entries.contains(gameId)) return;
    const QStringList locs = m_entries[gameId].value("_savedTileLocs").toStringList();
    if (!locs.isEmpty()) {
        QVariantList locsVar;
        for (const QString &l : locs) locsVar << l;
        m_entries[gameId]["tileLocations"] = locsVar;
        writeGameInfo(m_entries[gameId].value("installPath").toString(), m_entries[gameId]);
        for (const QString &sk : locs)
            createTile(gameId, sk, false);
    }
    m_entries[gameId].remove("_archiveProgress");
    m_entries[gameId].remove("_archiveCurrentFile");
    m_entries[gameId].remove("_savedTileLocs");
    clearArchivePending(m_entries[gameId].value("installPath").toString());
}

void GameLibraryModule::clearArchivePending(const QString &installPath)
{
    if (installPath.isEmpty()) return;
    GameInfoStore::modify(installPath, [](QJsonObject &obj) {
        if (!obj.contains("archivePending"))
            return false;
        obj.remove("archivePending");
        return true;
    });
}

// Архивация, прерванная закрытием программы или сбоем: в game_info.json
// осталась отметка archivePending со списком мест плиток. Игра при этом
// не тронута (папка game/ удаляется только после успешной архивации) —
// убираем недописанный архив и возвращаем плитки.
void GameLibraryModule::recoverInterruptedArchives()
{
    const QString basePath = m_settings ? m_settings->installPath() : QString();
    if (basePath.isEmpty()) return;

    const QPointer<GameLibraryModule> self(this);
    QThreadPool::globalInstance()->start([this, self, basePath]() {
        QStringList pending;
        const QDir base(basePath);
        for (const QFileInfo &fi : base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString fp = QDir::fromNativeSeparators(fi.absoluteFilePath());
            if (GameInfoStore::read(fp).contains("archivePending"))
                pending << fp;
        }
        if (pending.isEmpty() || !self) return;

        QMetaObject::invokeMethod(self.data(), [this, pending]() {
            for (const QString &fp : pending) {
                const QVariantMap info = scanFolder(fp);
                const QString gameId = info.value("id").toString();
                if (gameId.isEmpty()
                    || m_archiveTasks.contains(gameId) || m_extractTasks.contains(gameId))
                    continue;

                const QStringList locs = GameInfoStore::read(fp)
                                             .value("archivePending").toVariant().toStringList();
                m_entries[gameId] = info;

                if (info.value("type").toString() == QLatin1String("archive")) {
                    // Архивация успела завершиться — отметка просто лишняя
                    clearArchivePending(fp);
                    continue;
                }

                qWarning() << "GameLibraryModule: interrupted archive found, restoring tiles:" << fp;
                const QString partial = archivePathForGame(fp);
                QFile::remove(partial);
                QDir(QFileInfo(partial).absolutePath()).rmdir(".");

                m_entries[gameId]["_savedTileLocs"] = locs;
                restoreTilesAfterArchive(gameId);   // снимет отметку
            }
        }, Qt::QueuedConnection);
    });
}

void GameLibraryModule::cancelArchive(const QString &gameId)
{
    auto it = m_archiveTasks.find(gameId);
    if (it == m_archiveTasks.end()) return;
    if (it->cancelFlag) it->cancelFlag->store(true);
}

// ═════════════════════════════════════════════════════════════════════════════
// extractGame — распаковывает archive/<n>.7z обратно в game/
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::extractGame(const QString &gameId,
                                    bool           keepArchive,
                                    const QString &storageKey,
                                    bool           isFolder)
{
    if (!m_entries.contains(gameId)) {
        emit extractFinished(gameId, false);
        return;
    }
    if (m_extractTasks.contains(gameId) || m_archiveTasks.contains(gameId)
        || m_compressTasks.contains(gameId)) return;

    const QString installPath = QDir::fromNativeSeparators(
        m_entries[gameId].value("installPath").toString());

    // Находим архив — сначала из game_info.json, потом по стандартному пути.
    QString archivePath = QDir::fromNativeSeparators(
        m_entries[gameId].value("archivePath").toString());
    if (!QFile::exists(archivePath)) {
        const QString expected = QDir::fromNativeSeparators(findExistingArchive(installPath));
        if (!expected.isEmpty()) {
            archivePath = expected;
            m_entries[gameId]["archivePath"] = QDir::toNativeSeparators(expected);
        } else {
            qWarning() << "GameLibraryModule::extractGame: archive not found:" << archivePath;
            emit extractFinished(gameId, false);
            return;
        }
    }

    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    ExtractTask etask;
    etask.cancelFlag  = cancelFlag;
    etask.archivePath = archivePath;
    etask.destFolder  = installPath + "/game";
    m_extractTasks.insert(gameId, etask);

    const QPointer<GameLibraryModule> self(this);
    auto failReason = std::make_shared<QString>();
    QThreadPool::globalInstance()->start([this, self, gameId, installPath, archivePath,
                                          keepArchive, storageKey, isFolder, cancelFlag, failReason]() {

        // Место на диске установки — до распаковки (раньше нехватка была
        // видна только по ошибке посреди распаковки). Размер — из списка
        // файлов архива; не удалось узнать — распаковываем как раньше.
        const qint64 need = SevenZip::unpackedSize(archivePath);
        if (need > 0) {
            const qint64 margin = qint64(512) << 20;
            const QStorageInfo storage(installPath);
            const qint64 freeBytes = storage.bytesAvailable();
            if (freeBytes >= 0 && freeBytes < need + margin) {
                auto gb = [](qint64 b) { return QString::number(double(b) / (1024.0 * 1024 * 1024), 'f', 1); };
                *failReason = QString("мало места на диске %1 — нужно %2 ГБ, свободно %3 ГБ")
                                  .arg(storage.rootPath(), gb(need + margin), gb(freeBytes));
                qWarning() << "GameLibraryModule: extract aborted:" << *failReason;
            }
        }

        const SevenZip::Result r = !failReason->isEmpty() ? SevenZip::ExitError : SevenZip::extract(
            archivePath,
            installPath,   // workDir: 7zr восстановит подпапку game/ на месте
            cancelFlag.get(),
            [this, self, gameId](int pct, const QString &) {
                const qreal progress = pct / 100.0;
                if (self) QMetaObject::invokeMethod(self.data(), [this, gameId, progress]() {
                    m_extractProgress[gameId] = progress;
                    emit extractProgress(gameId, progress);
                }, Qt::QueuedConnection);
            });

        const bool cancelled = (r == SevenZip::Cancelled);
        const bool ok        = (r == SevenZip::Ok);

        // Недораспакованная game/ бесполезна, архив остаётся на месте.
        // Удаляем здесь, а не в GUI-потоке: при выходе из программы
        // обработчик ниже уже не вызывается, и мусор оставался на диске.
        // (если распаковка не начиналась из-за места — удалять нечего)
        if (!ok && failReason->isEmpty())
            QDir(installPath + "/game").removeRecursively();

        if (self) QMetaObject::invokeMethod(self.data(),
                                  [this, gameId, ok, cancelled, installPath, archivePath,
                                   keepArchive, storageKey, isFolder, failReason]()
                                  {
                                      m_extractTasks.remove(gameId);
                                      m_extractProgress.remove(gameId);

                                      if (cancelled) {
                                          qDebug() << "GameLibraryModule: extract cancelled:" << gameId;
                                          if (m_bus) {
                                              emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                                      Notifications::makeEvent(
                                                                          "Распаковка отменена",
                                                                          NotificationLevel::Info, true, 3000));
                                          }
                                          emit extractFinished(gameId, false);
                                          return;
                                      }

                                      if (ok && m_entries.contains(gameId)) {
                                          m_entries[gameId]["type"]        = QString("installed");
                                          m_entries[gameId]["archivePath"] = QString();
                                          m_entries[gameId]["available"]   = true;
                                          m_entries[gameId]["sizeCached"]  = static_cast<qlonglong>(-1);
                                          // installPath не меняется.

                                          if (!keepArchive) {
                                              QFile::remove(archivePath);
                                              QDir(installPath + "/archive").rmdir(".");
                                          }

                                          writeGameInfo(installPath, m_entries[gameId]);

                                          // Из архива файлы выходят обычными (не сжатыми WOF)
                                          if (m_entries[gameId].value("compressed").toBool()) {
                                              m_entries[gameId]["compressed"] = false;
                                              m_entries[gameId].remove("sizeBeforeCompress");
                                              writeGameInfo(installPath, m_entries[gameId]);
                                          }
                                          if (!storageKey.isEmpty())
                                              createTile(gameId, storageKey, isFolder);

                                          requestFolderSize(gameId,
                                                            QDir::toNativeSeparators(installPath + "/game"));
                                          if (m_bus) {
                                              const QString name = m_entries[gameId].value("name").toString();
                                              emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                                      Notifications::makeEvent(
                                                                          QString("«%1» разархивирована").arg(name),
                                                                          NotificationLevel::Info, true, 4000));
                                          }
                                          emit libraryEntryChanged(gameId, m_entries[gameId]);
                                      } else if (!ok) {
                                          qWarning() << "GameLibraryModule: extract failed:" << gameId;
                                          if (m_bus) {
                                              const QString name = m_entries.contains(gameId)
                                                  ? m_entries[gameId].value("name").toString() : gameId;
                                              emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                                      Notifications::makeEvent(
                                                                          failReason->isEmpty()
                                                                              ? QString("Ошибка распаковки «%1». Проверьте место на диске.").arg(name)
                                                                              : QString("«%1» не распакована: %2").arg(name, *failReason),
                                                                          NotificationLevel::Error, true, 7000));
                                          }
                                      }
                                      emit extractFinished(gameId, ok);
                                  }, Qt::QueuedConnection);
    });
}

void GameLibraryModule::cancelExtract(const QString &gameId)
{
    auto it = m_extractTasks.find(gameId);
    if (it == m_extractTasks.end()) return;
    if (it->cancelFlag) it->cancelFlag->store(true);
}

void GameLibraryModule::removeFromLibrary(const QString &gameId)
{
    if (!m_entries.contains(gameId)) return;

    const QString installPath = m_entries[gameId].value("installPath").toString();
    QFile::remove(installPath + "/game_info.json");
    m_entries.remove(gameId);

    qDebug() << "GameLibraryModule: removed from library:" << gameId;
}

void GameLibraryModule::deleteGame(const QString &gameId)
{
    if (!m_entries.contains(gameId)) return;

    // Нельзя удалять папку, в которую прямо сейчас пишет/из которой читает 7z
    if (m_archiveTasks.contains(gameId) || m_extractTasks.contains(gameId)) {
        qWarning() << "GameLibraryModule::deleteGame: task in progress for" << gameId;
        if (m_bus)
            emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                    Notifications::makeEvent(
                                        "Сначала отмените архивацию/распаковку этой игры",
                                        NotificationLevel::Warning, true, 4000));
        return;
    }

    const QVariantMap entry   = m_entries[gameId];
    const QString installPath = entry.value("installPath").toString();
    const QString name        = entry.value("name").toString();

    // Удаляем плитки через шину.
    QStringList locs;
    const QVariantList tileLocs = entry.value("tileLocations").toList();
    for (const QVariant &v : tileLocs)
        locs << v.toString();
    removeTilesFromStructure(locs, gameId);

    // Удаляем всю папку игры (для всех типов, включая external).
    // Только если это действительно папка игры MediaStation.
    const QString folder = QDir::fromNativeSeparators(installPath);
    if (InstallUtils::isManagedGameFolder(folder))
        QDir(folder).removeRecursively();
    else
        qWarning() << "GameLibraryModule::deleteGame: refusing to delete" << folder;

    m_entries.remove(gameId);

    if (m_bus) {
        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                Notifications::makeEvent(
                                    QString("«%1» удалена").arg(name),
                                    NotificationLevel::Info, true, 3000));
    }

    qDebug() << "GameLibraryModule: deleted game:" << gameId << entry.value("type").toString();
}

// ═════════════════════════════════════════════════════════════════════════════
// openFolder
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::openFolder(const QString &folderPath)
{
#ifdef Q_OS_WIN
    const QString path = QFileInfo(folderPath).isFile()
                             ? QDir::toNativeSeparators(QFileInfo(folderPath).absolutePath())
                             : QDir::toNativeSeparators(folderPath);
    QProcess::startDetached("explorer.exe", { path });
#else
    Q_UNUSED(folderPath)
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// reassignPath
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::reassignPath(const QString &gameId, const QString &newExePath)
{
    if (!m_entries.contains(gameId)) return;

    const QString installPath = m_entries[gameId].value("installPath").toString();
    m_entries[gameId]["executablePath"] = newExePath;
    m_entries[gameId]["available"]      = QFile::exists(newExePath);

    writeGameInfo(installPath, m_entries[gameId]);
    emit libraryEntryChanged(gameId, m_entries[gameId]);
}

// ═════════════════════════════════════════════════════════════════════════════
// regenerateMetadata
// ═════════════════════════════════════════════════════════════════════════════

void GameLibraryModule::regenerateMetadata(const QString &gameId)
{
    if (!m_entries.contains(gameId)) {
        qWarning() << "GameLibraryModule::regenerateMetadata: unknown gameId" << gameId;
        return;
    }

    const QString installPath = m_entries[gameId].value("installPath").toString();
    QVariantMap newInfo = generateMetadataForLegacy(installPath);
    if (newInfo.isEmpty()) {
        qWarning() << "GameLibraryModule::regenerateMetadata: cannot generate metadata for" << installPath;
        return;
    }

    // Сохраняем старый tileCount, если он был уже загружен (чтобы не сбросить)
    int oldTileCount = m_entries[gameId].value("tileCount", -1).toInt();
    newInfo["tileCount"] = oldTileCount;

    // Обновляем запись и записываем на диск
    m_entries[gameId] = newInfo;
    writeGameInfo(installPath, newInfo);

    // Уведомляем QML
    emit libraryEntryChanged(gameId, newInfo);
}

// ═════════════════════════════════════════════════════════════════════════════
// compressGame — прозрачное сжатие Windows (WOF) папки game/
// ═════════════════════════════════════════════════════════════════════════════

bool GameLibraryModule::compressAvailable() const
{
    return WofCompact::isAvailable();
}

QString GameLibraryModule::compressTargetDir(const QString &installPath)
{
    const QString game = QDir(installPath).filePath(QStringLiteral("game"));
    return QFileInfo(game).isDir() ? game : installPath;
}

void GameLibraryModule::compressGame(const QString &gameId, bool compress)
{
    if (!m_entries.contains(gameId)) { emit compressFinished(gameId, false, false); return; }
    if (m_compressTasks.contains(gameId) || m_archiveTasks.contains(gameId)
        || m_extractTasks.contains(gameId)) return;

    QVariantMap &entry = m_entries[gameId];
    const bool wasCompressed  = entry.value("compressed").toBool();
    const QString installPath = QDir::fromNativeSeparators(entry.value("installPath").toString());
    const QString name        = entry.value("name").toString();
    const QString dir         = compressTargetDir(installPath);

    auto notify = [this](const QString &text, NotificationLevel level, int ms) {
        if (m_bus)
            emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                    Notifications::makeEvent(text, level, true, ms));
    };

    if (entry.value("type").toString() != QLatin1String("installed") || !QFileInfo(dir).isDir()) {
        emit compressFinished(gameId, false, wasCompressed);
        return;
    }
    if (!WofCompact::isAvailable()) {
        notify(QStringLiteral("Сжатие недоступно: не найден compact.exe"), NotificationLevel::Error, 5000);
        emit compressFinished(gameId, false, wasCompressed);
        return;
    }

    // Снятие сжатия возвращает файлам полный размер — нужно место
    const qint64 sizeNow    = entry.value("sizeCached").toLongLong();
    const qint64 sizeBefore = entry.value("sizeBeforeCompress").toLongLong();
    if (!compress && sizeBefore > sizeNow && sizeNow > 0) {
        const QStorageInfo st(dir);
        const qint64 need = sizeBefore - sizeNow + (qint64(512) << 20);
        if (st.isValid() && st.bytesAvailable() >= 0 && st.bytesAvailable() < need) {
            notify(QString("«%1»: мало места на диске %2, чтобы снять сжатие — нужно %3 ГБ, свободно %4 ГБ")
                       .arg(name, st.rootPath())
                       .arg(double(need) / (1 << 30), 0, 'f', 1)
                       .arg(double(st.bytesAvailable()) / (1 << 30), 0, 'f', 1),
                   NotificationLevel::Error, 8000);
            emit compressFinished(gameId, false, wasCompressed);
            return;
        }
    }

    auto *proc = new QProcess(this);
    proc->setProgram(WofCompact::program());
    proc->setArguments(WofCompact::arguments(compress));
    proc->setWorkingDirectory(dir);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    CompressTask task;
    task.process    = proc;
    task.compress   = compress;
    task.sizeBefore = compress ? sizeNow : sizeBefore;
    m_compressTasks.insert(gameId, task);

    // Сколько файлов — для прогресса (вывод compact — строка на файл)
    QPointer<GameLibraryModule> self(this);
    (void)QtConcurrent::run([self, gameId, dir, proc]() {
        int n = 0;
        QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) { it.next(); ++n; }
        if (self) QMetaObject::invokeMethod(self.data(), [self, gameId, proc, n]() {
            auto it = self->m_compressTasks.find(gameId);
            if (it != self->m_compressTasks.end() && it->process == proc) it->totalFiles = qMax(1, n);
        }, Qt::QueuedConnection);
    });

    auto buffer = std::make_shared<QByteArray>();
    connect(proc, &QProcess::readyRead, this, [this, gameId, proc, buffer]() {
        buffer->append(proc->readAll());
        auto it = m_compressTasks.find(gameId);
        if (it == m_compressTasks.end()) return;
        int nl;
        while ((nl = buffer->indexOf('\n')) >= 0) {
            // Вывод в кодировке консоли (OEM); для счёта строк кодировка не важна
            const QString line = QString::fromLocal8Bit(buffer->left(nl));
            buffer->remove(0, nl + 1);
            if (WofCompact::isFileLine(line)) ++it->doneFiles;
        }
        if (it->totalFiles > 0) {
            const qreal p = qMin<qreal>(0.99, qreal(it->doneFiles) / it->totalFiles);
            if (p - it->progress >= 0.005) {
                it->progress = p;
                emit compressProgress(gameId, p);
            }
        }
    });
    connect(proc, &QProcess::finished, this, [this, gameId](int code, QProcess::ExitStatus st) {
        finishCompress(gameId, st == QProcess::NormalExit && code == 0);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, gameId](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finishCompress(gameId, false);
    });

    qInfo() << "GameLibraryModule:" << (compress ? "compressing" : "decompressing") << gameId << dir;
    proc->start();
    emit compressProgress(gameId, 0.0);
}

void GameLibraryModule::cancelCompress(const QString &gameId)
{
    auto it = m_compressTasks.find(gameId);
    if (it == m_compressTasks.end() || !it->process) return;
    it->cancelled = true;
    it->process->kill();
}

void GameLibraryModule::finishCompress(const QString &gameId, bool ok)
{
    auto it = m_compressTasks.find(gameId);
    if (it == m_compressTasks.end()) return;
    const CompressTask task = *it;
    m_compressTasks.erase(it);
    if (task.process) task.process->deleteLater();

    if (!m_entries.contains(gameId)) return;
    QVariantMap &entry = m_entries[gameId];
    const QString installPath = QDir::fromNativeSeparators(entry.value("installPath").toString());
    const QString name        = entry.value("name").toString();

    // Прерванное сжатие — частичное: часть файлов уже сжата, поэтому считаем
    // игру сжатой (можно снять сжатие). Прерванное снятие — тоже частичное.
    const bool compressed = task.compress ? true : !(ok && !task.cancelled);
    entry["compressed"] = compressed;
    if (compressed) {
        if (task.compress && task.sizeBefore > 0 && !entry.contains("sizeBeforeCompress"))
            entry["sizeBeforeCompress"] = task.sizeBefore;
    } else {
        entry.remove("sizeBeforeCompress");
    }
    // Время изменения папок от сжатия не меняется — размер пересчитываем явно
    entry["sizeCached"] = -1;
    writeGameInfo(installPath, entry);

    QPointer<GameLibraryModule> self(this);
    const bool compress = task.compress, cancelled = task.cancelled;
    const qint64 before = task.sizeBefore;
    const QString sizePath = sizeTargetFor(entry);
    (void)QtConcurrent::run([self, gameId, sizePath, name, ok, compress, cancelled, before, compressed]() {
        const qint64 bytes = calcFolderSize(sizePath);
        const QString stamp = sizeStamp(sizePath);
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, gameId, bytes, stamp, name, ok, compress,
                                                cancelled, before, compressed]() {
            if (!self->m_entries.contains(gameId)) return;
            QVariantMap &e = self->m_entries[gameId];
            e["sizeCached"] = bytes;
            e["sizeStamp"]  = stamp;
            self->writeGameInfo(QDir::fromNativeSeparators(e.value("installPath").toString()), e);
            emit self->folderSizeReady(gameId, bytes);

            const auto gb = [](qint64 b) { return QString::number(double(b) / (1 << 30), 'f', 1); };
            QString text;
            NotificationLevel level = NotificationLevel::Info;
            if (cancelled)
                text = compress ? QString("Сжатие «%1» остановлено — уже сжатые файлы работают").arg(name)
                                : QString("Снятие сжатия «%1» остановлено").arg(name);
            else if (!ok) {
                text  = QString("«%1»: %2 с ошибками — часть файлов могла быть занята")
                            .arg(name, compress ? QStringLiteral("сжатие завершилось")
                                                : QStringLiteral("снятие сжатия завершилось"));
                level = NotificationLevel::Warning;
            } else if (compress && before > 0)
                text = QString("«%1» сжата: %2 → %3 ГБ").arg(name, gb(before), gb(bytes));
            else
                text = compress ? QString("«%1» сжата").arg(name)
                                : QString("«%1»: сжатие снято").arg(name);
            if (self->m_bus)
                emit self->m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                              Notifications::makeEvent(text, level, true, 6000));
            emit self->compressFinished(gameId, ok && !cancelled, compressed);
        }, Qt::QueuedConnection);
    });
}
