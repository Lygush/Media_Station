// TileManager.cpp
// ─────────────────────────────────────────────────────────────────────────────
// TileManager — управление деревом плиток и файловой системой.
// Вся логика установки делегируется InstallManager (m_installer).
// ─────────────────────────────────────────────────────────────────────────────

#include "TileManager.h"
#include "NotificationModule.h"
#include "core/EventBus.h"
#include "tools/AtomicFile.h"
#include "tools/GameInfoStore.h"

#include <QStorageInfo>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>
#include <QRegularExpression>
#include <QDebug>
#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>

// ─────────────────────────────────────────────────────────────────────────────
const QStringList TileManager::s_knownScreens = { "games", "tv" };

// ═════════════════════════════════════════════════════════════════════════════
// Конструктор / деструктор
// ═════════════════════════════════════════════════════════════════════════════

TileManager::TileManager(QObject *parent)
    : QObject(parent)
    , dataDir(QCoreApplication::applicationDirPath() + "/data")
    , m_structurePath(QCoreApplication::applicationDirPath() + "/data/structure.json")
// m_installer = nullptr — инжектируется из main.cpp через setInstallManager()
{
    dataDir.mkpath(".");
    loadStructure();
}

TileManager::~TileManager() = default;

// ═════════════════════════════════════════════════════════════════════════════
// setInstallManager — инъекция зависимости из main.cpp
// ═════════════════════════════════════════════════════════════════════════════

void TileManager::setInstallManager(InstallManager *im)
{
    Q_ASSERT(im);
    m_installer = im;

    // Пробрасываем все сигналы InstallManager наружу.
    // QML подписан на TileManager.xxx, поэтому сигналы должны идти от него.
    connect(m_installer, &InstallManager::installationProgress,
            this,        &TileManager::installationProgress);
    // Сначала сохраняем результат в хранилище, затем сообщаем QML
    connect(m_installer, &InstallManager::installationFinished,
            this,        &TileManager::persistInstallResult);
    connect(m_installer, &InstallManager::installationFinished,
            this,        &TileManager::installationFinished);
    connect(m_installer, &InstallManager::installationCancelled,
            this,        &TileManager::installationCancelled);
    connect(m_installer, &InstallManager::exeScanReady,
            this,        &TileManager::exeScanReady);
    connect(m_installer, &InstallManager::exeDescriptionReady,
            this,        &TileManager::exeDescriptionReady);
    connect(m_installer, &InstallManager::dependencyCheckReady,
            this,        &TileManager::dependencyCheckReady);
    connect(m_installer, &InstallManager::launchExeUpdated,
            this,        &TileManager::launchExeUpdated);
}

// ═════════════════════════════════════════════════════════════════════════════
// IModule::initialize — подписка на события шины
// ═════════════════════════════════════════════════════════════════════════════

void TileManager::initialize(EventBus *bus, SettingsManager * /*settings*/)
{
    m_bus = bus;

    // ── GameLibraryModule → TileManager ──────────────────────────────────────
    connect(bus, &EventBus::moduleEvent, this,
            [this, bus](const QString &mod, const QString &ev, const QVariantMap &data)
            {
                if (mod != "gameLibrary") return;

                // ── createTile: создать плитку в нужном экране/папке ─────────────────
                if (ev == "createTile") {
                    QString storageKey = data["storageKey"].toString();
                    bool    isFolder   = data["isFolder"].toBool();

                    // storageKey должен быть экраном ("games"/"tv") или id папки.
                    // Раньше при восстановлении плиток после отмены архивации сюда
                    // приходили id из tile_id.txt, и в structure.json появлялись
                    // «экраны» с мусорными ключами.
                    if (!isFolder && !s_knownScreens.contains(storageKey)) {
                        bool folderExists = false;
                        for (const QString &screen : s_knownScreens) {
                            QJsonArray tmp;
                            if (findNodeChildren(m_structure[screen].toArray(), storageKey, tmp)) {
                                folderExists = true; break;
                            }
                        }
                        if (folderExists) {
                            isFolder = true;
                        } else {
                            // Старые установки хранят в tileLocations id плитки,
                            // а не место. Раньше такая плитка после отмены архивации
                            // просто пропадала — теперь возвращается на экран игр.
                            qWarning() << "TileManager: unknown tile location" << storageKey
                                       << "— placing tile on 'games'";
                            storageKey = QStringLiteral("games");
                            isFolder   = false;
                        }
                    }
                    const QString gameId     = data["gameId"].toString();
                    const QString tileId     = "lib_" + gameId;

                    // Не создаём дубликат если плитка уже есть.
                    if (countTilesById(tileId) > 0) {
                        qDebug() << "TileManager: tile already exists, skipping createTile:" << tileId;
                        const int count = countTilesById(tileId);
                        emit bus->moduleEvent("tileManager", "tileCountChanged",
                                              QVariantMap{{ "gameId", gameId }, { "count", count }});
                        return;
                    }

                    QVariantMap tile;
                    tile["type"]          = "shortcut";
                    tile["kind"]          = "app";
                    tile["title"]         = data["title"].toString();
                    tile["icon"]          = data["iconPath"].toString();
                    tile["path"]          = data["ps1Path"].toString();
                    tile["id"]            = tileId;
                    tile["installFolder"] = data["installPath"].toString();

                    QVariantList tiles = isFolder ? loadFolder(storageKey) : loadScreen(storageKey);
                    tiles.append(tile);
                    if (isFolder) saveFolder(storageKey, tiles);
                    else          saveScreen(storageKey, tiles);

                    qDebug() << "TileManager: created tile from library:" << tileId
                             << "in" << storageKey;

                    emit tileStructureChanged(storageKey, isFolder);

                    emit bus->moduleEvent("tileManager", "tileCountChanged",
                                          QVariantMap{{ "gameId", gameId }, { "count", countTilesById(tileId) }});
                }

                // ── removeTiles: удалить плитки из structure.json по tileId ──────────
                else if (ev == "removeTiles") {
                    const QVariantList locs = data["tileLocations"].toList();
                    const QString gameId    = data["gameId"].toString();
                    const QString tileId    = "lib_" + gameId;

                    if (!gameId.isEmpty())
                        removeTileFromStructure(tileId);

                    for (const QVariant &v : locs) {
                        const QString loc = v.toString();
                        if (!loc.isEmpty())
                            removeTileFromStructure(loc);
                    }

                    qDebug() << "TileManager: removed tiles for gameId:" << gameId
                             << "locations:" << locs;

                    emit tileStructureChanged(QString(), false);

                    if (!gameId.isEmpty())
                        emit bus->moduleEvent("tileManager", "tileCountChanged",
                                              QVariantMap{{ "gameId", gameId }, { "count", countTilesById(tileId) }});
                }

                // ── queryTileCounts ───────────────────────────────────────────────────
                else if (ev == "queryTileCounts") {
                    const QVariantList entries = data["entries"].toList();
                    for (const QVariant &v : entries) {
                        const QVariantMap e      = v.toMap();
                        const QString gameId     = e["gameId"].toString();
                        if (gameId.isEmpty()) continue;

                        int count = countTilesById("lib_" + gameId);
                        if (count == 0) {
                            const QVariantList locs = e["tileLocations"].toList();
                            for (const QVariant &l : locs) {
                                const QString loc = l.toString();
                                if (!loc.isEmpty())
                                    count += countTilesById(loc);
                            }
                        }

                        emit bus->moduleEvent("tileManager", "tileCountChanged",
                                              QVariantMap{{ "gameId", gameId }, { "count", count }});
                    }
                }
            });
}

void TileManager::shutdown()
{
    if (m_installer)
        m_installer->cancelAllInstallations();
    m_installer = nullptr;
    m_bus       = nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
// Уведомления
// ═════════════════════════════════════════════════════════════════════════════

void TileManager::notify(NotificationLevel level,
                         const QString     &msg,
                         bool               autoDismiss,
                         int                duration) const
{
    if (!m_bus) return;
    emit m_bus->moduleEvent(
        Notifications::kModule,
        Notifications::kEventShow,
        Notifications::makeEvent(msg, level, autoDismiss, duration));
}


// ─────────────────────────────────────────────────────────────────────────────
// Пути плиток относительно папки программы.
//
// В structure.json пути внутри папки программы (игры, иконки, ярлыки)
// хранятся с префиксом "$APP/". Тогда перенос всей папки MediaStation
// на другой диск или в другую папку не ломает плитки. В памяти и в QML
// пути всегда абсолютные. Префикс выбран явным, чтобы не перепутать его
// с короткими командами вроде "notepad.exe", которые трогать нельзя.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

const QString kAppPrefix = QStringLiteral("$APP/");
const QStringList &pathKeys()
{
    static const QStringList keys = { QStringLiteral("path"), QStringLiteral("icon"),
                                      QStringLiteral("installFolder") };
    return keys;
}

QString appRoot()
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath());
}

QString resolveAppPath(const QString &stored)
{
    if (!stored.startsWith(kAppPrefix))
        return stored;
    return QDir::toNativeSeparators(appRoot() + "/" + stored.mid(kAppPrefix.size()));
}

QString toAppRelative(const QString &path)
{
    if (path.isEmpty() || path.startsWith(kAppPrefix) || path.contains(QLatin1String("://")))
        return path;
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (!QDir::isAbsolutePath(clean))
        return path;
    const QString root = appRoot();
    if (clean.compare(root, Qt::CaseInsensitive) == 0)
        return path;
    if (!clean.startsWith(root + "/", Qt::CaseInsensitive))
        return path;
    return kAppPrefix + clean.mid(root.size() + 1);
}

// Рекурсивно по дереву плиток (папки хранят детей в "children")
QJsonArray mapTreePaths(const QJsonArray &arr, QString (*fn)(const QString &))
{
    QJsonArray out;
    for (const QJsonValue &v : arr) {
        if (!v.isObject()) { out.append(v); continue; }
        QJsonObject obj = v.toObject();
        for (const QString &k : pathKeys())
            if (obj.value(k).isString())
                obj[k] = fn(obj.value(k).toString());
        if (obj.value("children").isArray())
            obj["children"] = mapTreePaths(obj.value("children").toArray(), fn);
        out.append(obj);
    }
    return out;
}

QJsonObject mapStructurePaths(const QJsonObject &structure, QString (*fn)(const QString &))
{
    QJsonObject out = structure;
    for (auto it = structure.begin(); it != structure.end(); ++it)
        if (it.value().isArray())
            out[it.key()] = mapTreePaths(it.value().toArray(), fn);
    return out;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Загрузка / сохранение structure.json
// ═════════════════════════════════════════════════════════════════════════════

void TileManager::loadStructure()
{
    QFile f(m_structurePath);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (doc.isObject()) {
            m_structure = mapStructurePaths(doc.object(), resolveAppPath);
            for (const QString &screen : s_knownScreens)
                if (!m_structure.contains(screen))
                    m_structure[screen] = QJsonArray();
            return;
        }
        // Повреждённый файл: раньше первая же запись молча затирала все плитки
        // пользователя. Сохраняем копию, чтобы данные можно было восстановить.
        const QString backup = m_structurePath + ".corrupted_"
                               + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        QFile::copy(m_structurePath, backup);
        qWarning() << "TileManager: structure.json is corrupted, backup saved to" << backup;
    }
    for (const QString &screen : s_knownScreens)
        m_structure[screen] = QJsonArray();
}

void TileManager::saveStructure() const
{
    if (!AtomicFile::writeJson(m_structurePath,
                               QJsonDocument(mapStructurePaths(m_structure, toAppRelative)))) {
        // Критичная ошибка — данные не сохранятся. Уведомляем пользователя.
        notify(NotificationLevel::Error,
               "Ошибка: не удалось сохранить структуру плиток");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Рекурсивные хелперы дерева
// ═════════════════════════════════════════════════════════════════════════════

bool TileManager::findNodeChildren(const QJsonArray &arr, const QString &id, QJsonArray &out)
{
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        if (obj["id"].toString() == id) { out = obj["children"].toArray(); return true; }
        if (obj["type"].toString() == "folder")
            if (findNodeChildren(obj["children"].toArray(), id, out)) return true;
    }
    return false;
}

bool TileManager::setNodeChildren(QJsonArray &arr, const QString &id, const QVariantList &tiles)
{
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject obj = arr[i].toObject();
        if (obj["id"].toString() == id) {
            obj["children"] = mergeChildren(obj["children"].toArray(), tiles);
            arr[i] = obj; return true;
        }
        if (obj["type"].toString() == "folder") {
            QJsonArray children = obj["children"].toArray();
            if (setNodeChildren(children, id, tiles)) {
                obj["children"] = children; arr[i] = obj; return true;
            }
        }
    }
    return false;
}

bool TileManager::removeNodeById(QJsonArray &arr, const QString &id)
{
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject obj = arr[i].toObject();
        if (obj["id"].toString() == id) { arr.removeAt(i); return true; }
        if (obj["type"].toString() == "folder") {
            QJsonArray children = obj["children"].toArray();
            if (removeNodeById(children, id)) {
                obj["children"] = children; arr[i] = obj; return true;
            }
        }
    }
    return false;
}

QJsonArray TileManager::mergeChildren(const QJsonArray &existing, const QVariantList &tiles)
{
    QMap<QString, QJsonObject> existingById;
    for (const QJsonValue &v : existing) {
        const QJsonObject obj = v.toObject();
        const QString eid = obj["id"].toString();
        if (!eid.isEmpty()) existingById[eid] = obj;
    }
    QJsonArray result;
    for (const QVariant &v : tiles) {
        QJsonObject obj = QJsonObject::fromVariantMap(v.toMap());
        if (obj["type"].toString() == "folder") {
            const QString eid = obj["id"].toString();
            if (existingById.contains(eid) && existingById[eid].contains("children"))
                obj["children"] = existingById[eid]["children"];
            else if (!obj.contains("children"))
                obj["children"] = QJsonArray();
        }
        result.append(obj);
    }
    return result;
}

QVariantList TileManager::stripChildren(const QJsonArray &arr)
{
    QVariantList result;
    result.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        QJsonObject obj = v.toObject();
        obj.remove("children");

        // ── Обложка игры (Фаза 1) ──────────────────────────────────────────────
        const QString installFolder = resolveAppPath(obj["installFolder"].toString());
        if (!installFolder.isEmpty()) {
            const QString coverPath = GameInfoStore::resolve(
                installFolder, GameInfoStore::read(installFolder)["coverPath"].toString());
            if (!coverPath.isEmpty())
                obj["coverPath"] = coverPath;
        }

        result.append(obj.toVariantMap());
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// Публичный API — загрузка / сохранение плиток
// ═════════════════════════════════════════════════════════════════════════════

QVariantList TileManager::loadScreen(const QString &screen)
{
    return stripChildren(m_structure[screen].toArray());
}

void TileManager::saveScreen(const QString &screen, const QVariantList &tiles)
{
    m_structure[screen] = mergeChildren(m_structure[screen].toArray(), tiles);
    saveStructure();
}

QVariantList TileManager::loadFolder(const QString &folderId)
{
    for (const QString &screen : s_knownScreens) {
        QJsonArray children;
        if (findNodeChildren(m_structure[screen].toArray(), folderId, children))
            return stripChildren(children);
    }
    return {};
}

void TileManager::saveFolder(const QString &folderId, const QVariantList &tiles)
{
    for (const QString &screen : s_knownScreens) {
        QJsonArray arr = m_structure[screen].toArray();
        if (setNodeChildren(arr, folderId, tiles)) {
            m_structure[screen] = arr;
            saveStructure();
            return;
        }
    }
    qWarning() << "TileManager::saveFolder: folder not found:" << folderId;
}

void TileManager::deleteFolder(const QString &folderId)
{
    bool changed = false;
    for (const QString &screen : s_knownScreens) {
        QJsonArray arr = m_structure[screen].toArray();
        if (removeNodeById(arr, folderId)) { m_structure[screen] = arr; changed = true; }
    }
    if (changed) saveStructure();
}

// ═════════════════════════════════════════════════════════════════════════════
// Список экранов и папок
// ═════════════════════════════════════════════════════════════════════════════

QVariantList TileManager::listScreens() const
{
    QVariantList result;
    for (const QString &key : s_knownScreens) {
        QString title = key; title[0] = title[0].toUpper();
        result.append(QVariantMap{ {"storageKey", key}, {"isFolder", false}, {"title", title} });
    }
    return result;
}

QVariantList TileManager::listFolderTiles(const QString &storageKey, bool isFolder) const
{
    QJsonArray arr;
    if (!isFolder) {
        arr = m_structure[storageKey].toArray();
    } else {
        for (const QString &screen : s_knownScreens) {
            QJsonArray children;
            if (findNodeChildren(m_structure[screen].toArray(), storageKey, children))
            { arr = children; break; }
        }
    }
    QVariantList result;
    for (const QJsonValue &v : std::as_const(arr)) {
        QJsonObject obj = v.toObject();
        if (obj["type"].toString() == "folder") {
            obj.remove("children");
            result.append(obj.toVariantMap());
        }
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// countTilesById
// ═════════════════════════════════════════════════════════════════════════════

int TileManager::countTilesByIdInArray(const QJsonArray &arr, const QString &tileId)
{
    int count = 0;
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        if (obj["id"].toString() == tileId) ++count;
        if (obj["type"].toString() == "folder")
            count += countTilesByIdInArray(obj["children"].toArray(), tileId);
    }
    return count;
}

int TileManager::countTilesById(const QString &tileId) const
{
    if (tileId.isEmpty()) return 0;
    int total = 0;
    for (const QString &screen : s_knownScreens)
        total += countTilesByIdInArray(m_structure[screen].toArray(), tileId);
    return total;
}

// ═════════════════════════════════════════════════════════════════════════════
// Файловая система
// ═════════════════════════════════════════════════════════════════════════════

void TileManager::listDirAsync(const QString &requestId, const QString &path,
                               const QString &filterExts)
{
    QPointer<TileManager> self(this);
    // listDir/listDrives не трогают состояние объекта — безопасно в пуле потоков
    (void)QtConcurrent::run([self, requestId, path, filterExts]() {
        const QVariantList entries = path.isEmpty() ? TileManager::listDrivesImpl()
                                                    : TileManager::listDirImpl(path, filterExts);
        if (self)
            QMetaObject::invokeMethod(self.data(), [self, requestId, entries]() {
                if (self) emit self->dirListReady(requestId, entries);
            }, Qt::QueuedConnection);
    });
}

QVariantList TileManager::listDrives() { return listDrivesImpl(); }

QVariantList TileManager::listDir(const QString &path, const QString &filterExts)
{
    return listDirImpl(path, filterExts);
}

QVariantList TileManager::listDrivesImpl()
{
    QVariantList result;
    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &vol : volumes) {
        if (!vol.isValid() || !vol.isReady()) continue;
        QString rootPath = QDir::fromNativeSeparators(vol.rootPath());
        if (!rootPath.endsWith('/')) rootPath += '/';
        QString label = vol.displayName().trimmed();
#ifdef Q_OS_WIN
        const QString drive = (rootPath.length() >= 2 && rootPath[1] == ':')
                                  ? rootPath.left(2).toUpper() : QString();
        QString displayName;
        if (drive.isEmpty()) {
            displayName = label.isEmpty() ? rootPath : label + " (" + rootPath + ")";
        } else {
            displayName = (label.isEmpty() || label.compare(drive, Qt::CaseInsensitive) == 0)
            ? drive : label + " (" + drive + ")";
        }
#else
        const QString displayName = label.isEmpty() ? rootPath : label + " (" + rootPath + ")";
#endif
        result.append(QVariantMap{ {"name", displayName}, {"path", rootPath}, {"isDir", true} });
    }
    if (result.isEmpty())
        result.append(QVariantMap{ {"name", "C:"}, {"path", "C:/"}, {"isDir", true} });
    return result;
}

QVariantList TileManager::listDirImpl(const QString &path, const QString &filterExts)
{
    QVariantList result;
    const QDir dir(path);
    if (!dir.exists()) return result;

    auto toEntry = [](const QFileInfo &info, bool isDir) {
        return QVariantMap{
            {"name",  info.fileName()},
            {"path",  QDir::fromNativeSeparators(info.absoluteFilePath())},
            {"isDir", isDir}
        };
    };

    const QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                 QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : dirs)
        result.append(toEntry(info, true));

    QStringList filters;
    const QStringList exts = filterExts.split(',', Qt::SkipEmptyParts);
    for (const QString &ext : exts)
        filters << "*." + ext.trimmed() << "*." + ext.trimmed().toUpper();

    const auto fileList = filters.isEmpty()
                              ? dir.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase)
                              : dir.entryInfoList(filters, QDir::Files, QDir::Name | QDir::IgnoreCase);

    for (const QFileInfo &info : fileList)
        result.append(toEntry(info, false));

    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// removeTileFromStructure
// ═════════════════════════════════════════════════════════════════════════════

void TileManager::removeTileFromStructure(const QString &tileId)
{
    if (tileId.isEmpty()) return;
    bool changed = false;
    for (const QString &screen : s_knownScreens) {
        QJsonArray arr = m_structure[screen].toArray();
        // removeNodeById удаляет только первое вхождение — у игры может быть
        // несколько ярлыков в разных папках одного экрана
        bool removedAny = false;
        while (removeNodeById(arr, tileId)) removedAny = true;
        if (removedAny) { m_structure[screen] = arr; changed = true; }
    }
    if (changed) saveStructure();
}

int TileManager::updateNodesById(QJsonArray &arr, const QString &id, const QJsonObject &fields)
{
    int changed = 0;
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject obj = arr[i].toObject();
        bool dirty = false;
        if (obj["id"].toString() == id) {
            for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
                obj[it.key()] = it.value();
            dirty = true;
            ++changed;
        }
        if (obj["type"].toString() == "folder") {
            QJsonArray children = obj["children"].toArray();
            const int n = updateNodesById(children, id, fields);
            if (n > 0) { obj["children"] = children; dirty = true; changed += n; }
        }
        if (dirty) arr[i] = obj;
    }
    return changed;
}

bool TileManager::nodeHasEmptyPath(const QJsonArray &arr, const QString &id, bool &found)
{
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        if (obj["id"].toString() == id) {
            found = true;
            if (obj["path"].toString().isEmpty()) return true;
        }
        if (obj["type"].toString() == "folder"
            && nodeHasEmptyPath(obj["children"].toArray(), id, found))
            return true;
    }
    return false;
}

void TileManager::persistInstallResult(const QString &tileId, bool success,
                                       const QString &ps1Path, const QString &iconPath,
                                       const QString &installFolder)
{
    if (tileId.isEmpty()) return;

    if (!success) {
        // Удаляем только плитку, которая так и не была установлена (без пути).
        // Неудачная переустановка не должна стирать рабочий ярлык.
        bool found = false, emptyPath = false;
        for (auto it = m_structure.constBegin(); it != m_structure.constEnd(); ++it)
            if (it.value().isArray() && nodeHasEmptyPath(it.value().toArray(), tileId, found))
                emptyPath = true;
        if (found && emptyPath)
            removeTileFromStructure(tileId);
        return;
    }

    QJsonObject fields;
    fields["path"]            = ps1Path;
    fields["installing"]      = false;
    fields["installProgress"] = 0.0;
    fields["installPhase"]    = QString();
    if (!iconPath.isEmpty())      fields["icon"]          = iconPath;
    if (!installFolder.isEmpty()) fields["installFolder"] = installFolder;

    bool changed = false;
    for (const QString &key : m_structure.keys()) {
        if (!m_structure[key].isArray()) continue;
        QJsonArray arr = m_structure[key].toArray();
        if (updateNodesById(arr, tileId, fields) > 0) {
            m_structure[key] = arr;
            changed = true;
        }
    }
    if (changed) saveStructure();
}

// ═════════════════════════════════════════════════════════════════════════════
// readGameInfo — чтение game_info.json из папки установки
// ═════════════════════════════════════════════════════════════════════════════

QVariantMap TileManager::readGameInfo(const QString &installFolder) const
{
    const QString root = resolveAppPath(installFolder);
    if (root.isEmpty())
        return {};

    // Обложка/фон/трейлер хранятся относительно папки игры — для QML разворачиваем
    QVariantMap info = GameInfoStore::read(root).toVariantMap();
    GameInfoStore::resolveMediaPaths(root, info);
    return info;
}
