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
#include "tools/SevenZip.h"
#include "InstallUtils.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
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
        total += it.fileInfo().size();
    }
    return total;
}

// ── game_info.json ─────────────────────────────────────────────────────────

QVariantMap GameLibraryModule::readGameInfo(const QString &folderPath) const
{
    const QString infoPath = folderPath + "/game_info.json";
    QFile f(infoPath);
    if (!f.open(QIODevice::ReadOnly)) return {};

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "GameLibraryModule: bad game_info.json at" << infoPath;
        return {};
    }
    return doc.object().toVariantMap();
}

bool GameLibraryModule::writeGameInfo(const QString &folderPath, const QVariantMap &info) const
{
    if (folderPath.isEmpty()) return false;
    QDir().mkpath(folderPath);

    // В m_entries есть служебные поля времени выполнения (_archiveProgress,
    // _savedTileLocs, available, tileCount…). Раньше они попадали в game_info.json,
    // и после перезапуска, например, устаревший tileCount не пересчитывался.
    QVariantMap clean = info;
    for (auto it = clean.begin(); it != clean.end(); ) {
        const QString &k = it.key();
        if (k.startsWith(QLatin1Char('_')) || k == QLatin1String("available")
            || k == QLatin1String("tileCount") || k == QLatin1String("archiving")
            || k == QLatin1String("archiveProgress") || k == QLatin1String("archiveCurrentFile")
            || k == QLatin1String("extracting") || k == QLatin1String("extractProgress"))
            it = clean.erase(it);
        else
            ++it;
    }

    const QString infoPath = folderPath + "/game_info.json";
    // AtomicFile: запись через временный файл с атомарной заменой (раньше было
    // remove + rename — при сбое между ними файл терялся)
    if (!AtomicFile::writeJson(infoPath, QJsonDocument(QJsonObject::fromVariantMap(clean)))) {
        qWarning() << "GameLibraryModule: cannot write" << infoPath;
        return false;
    }
    return true;
}

// ── Путь к архиву внутри папки игры ────────────────────────────────────────
// Структура: <installPath>/archive/<gameName>.7z
// Всё что относится к игре хранится внутри её папки.

QString GameLibraryModule::archivePathForGame(const QString &installPath) const
{
    const QString gameName = QDir(installPath).dirName();
    return installPath + "/archive/" + gameName + ".7z";
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

    const QString expectedArchive = archivePathForGame(folderPath);
    QString type;
    QString archivePath;
    if (QFile::exists(expectedArchive)) {
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
    generated["available"]        = type != "archive" || QFile::exists(expectedArchive);
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
            // Архив всегда ищем внутри папки игры: <installPath>/archive/<name>.7z
            const QString expectedArchive = archivePathForGame(folderPath);
            // Поддержка старого формата: если archivePath указывает куда-то ещё — мигрируем
            if (!QFile::exists(info.value("archivePath").toString())
                && QFile::exists(expectedArchive)) {
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
        const qint64 bytes = calcFolderSize(folderPath);

        if (self) QMetaObject::invokeMethod(self.data(), [this, gameId, bytes]() {
            if (m_entries.contains(gameId)) {
                m_entries[gameId]["sizeCached"] = bytes;
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
    if (m_archiveTasks.contains(gameId) || m_extractTasks.contains(gameId)) return;

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

    // Новая структура: архив всегда внутри папки игры.
    // <installPath>/archive/<gameName>.7z
    const QString archiveDir   = installPath + "/archive";
    const QString archiveFinal = archivePathForGame(installPath);

    // Сохраняем tileLocations до удаления — для восстановления при отмене.
    QStringList savedTileLocs;
    const QVariantList tileLocs = entry.value("tileLocations").toList();
    for (const QVariant &v : tileLocs)
        savedTileLocs << v.toString();
    m_entries[gameId]["_savedTileLocs"] = QVariant(savedTileLocs);

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
            [this, watcher, gameId, installPath, archiveDir, archiveFinal, cancelFlag]()
            {
                const bool cancelled = cancelFlag->load();
                const bool ok        = !cancelled && watcher->result();

                m_archiveTasks.remove(gameId);
                emit archivingChanged();

                if (cancelled) {
                    QFile::remove(archiveFinal);
                    QDir(archiveDir).rmdir(".");
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
                    QFile::remove(archiveFinal);
                    QDir(archiveDir).rmdir(".");
                    // Игра осталась на месте — возвращаем её плитки (раньше
                    // они восстанавливались только при отмене).
                    restoreTilesAfterArchive(gameId);
                    qWarning() << "GameLibraryModule: archive failed:" << gameId;
                    if (m_bus) {
                        const QString name = m_entries.contains(gameId)
                        ? m_entries[gameId].value("name").toString() : gameId;
                        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                Notifications::makeEvent(
                                                    QString("Ошибка архивации «%1»").arg(name),
                                                    NotificationLevel::Error, true, 5000));
                    }
                    emit archiveFinished(gameId, false);
                }

                watcher->deleteLater();
            });

    const QPointer<GameLibraryModule> self(this);
    QFuture<bool> future = QtConcurrent::run(
        [this, self, gameId, installPath, archiveDir, archiveFinal, cancelFlag]() -> bool
        {
            if (cancelFlag->load()) return false;

            if (!SevenZip::isAvailable()) {
                qWarning() << "GameLibraryModule: 7zr.exe not found";
                return false;
            }

            QDir().mkpath(archiveDir);
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
    if (m_extractTasks.contains(gameId) || m_archiveTasks.contains(gameId)) return;

    const QString installPath = QDir::fromNativeSeparators(
        m_entries[gameId].value("installPath").toString());

    // Находим архив — сначала из game_info.json, потом по стандартному пути.
    QString archivePath = QDir::fromNativeSeparators(
        m_entries[gameId].value("archivePath").toString());
    if (!QFile::exists(archivePath)) {
        const QString expected = QDir::fromNativeSeparators(archivePathForGame(installPath));
        if (QFile::exists(expected)) {
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
    QThreadPool::globalInstance()->start([this, self, gameId, installPath, archivePath,
                                          keepArchive, storageKey, isFolder, cancelFlag]() {

        const SevenZip::Result r = SevenZip::extract(
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
        if (!ok)
            QDir(installPath + "/game").removeRecursively();

        if (self) QMetaObject::invokeMethod(self.data(),
                                  [this, gameId, ok, cancelled, installPath, archivePath,
                                   keepArchive, storageKey, isFolder]()
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
                                                                          QString("Ошибка распаковки «%1». Проверьте место на диске.").arg(name),
                                                                          NotificationLevel::Error, true, 5000));
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
