// TileManager.h
// ─────────────────────────────────────────────────────────────────────────────
// TileManager — управление деревом плиток медиацентра.
//
// Отвечает за:
//   - Загрузку/сохранение дерева плиток (structure.json)
//   - Просмотр файловой системы и дисков
//   - Делегирование установки / сканирования → InstallManager
//
// InstallManager теперь — отдельный IModule. TileManager держит
// не-owning указатель m_installer, который инжектируется из main.cpp
// через setInstallManager() после создания обоих модулей.
//
// Все инлайн-делегаты ниже требуют, чтобы setInstallManager() был
// вызван до первого обращения из QML.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include "core/IModule.h"
#include "InstallManager.h"
#include "NotificationModule.h"   // для NotificationLevel и Notifications::

class TileManager : public QObject, public IModule
{
    Q_OBJECT

public:
    explicit TileManager(QObject *parent = nullptr);
    ~TileManager();

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId() const override { return "tileManager"; }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown() override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return "TileManager"; }

    // ── Инъекция InstallManager (вызывается из main.cpp) ─────────────────────
    // Должна быть вызвана до initialize() и до первого обращения из QML.
    void setInstallManager(InstallManager *im);

    // ── Загрузка / сохранение плиток ─────────────────────────────────────────
    Q_INVOKABLE QVariantList loadScreen(const QString &screen);
    Q_INVOKABLE void         saveScreen(const QString &screen, const QVariantList &tiles);
    Q_INVOKABLE QVariantList loadFolder(const QString &folderId);
    Q_INVOKABLE void         saveFolder(const QString &folderId, const QVariantList &tiles);
    Q_INVOKABLE void         deleteFolder(const QString &folderId);

    // ── Файловая система ──────────────────────────────────────────────────────
    Q_INVOKABLE QVariantList listDir(const QString &path, const QString &filterExts = {});
    Q_INVOKABLE QVariantList listDrives();
    // То же в фоновом потоке, результат — сигнал dirListReady. Синхронные
    // версии на сетевом или «уснувшем» диске замораживали интерфейс на секунды.
    // path пустой → список дисков.
    Q_INVOKABLE void listDirAsync(const QString &requestId, const QString &path,
                                  const QString &filterExts = {});

    // ── Список экранов и папок ────────────────────────────────────────────────
    Q_INVOKABLE QVariantList listScreens() const;
    Q_INVOKABLE QVariantList listFolderTiles(const QString &storageKey, bool isFolder) const;

    // ── Чтение локального game_info.json ─────────────────────────────────────
    Q_INVOKABLE QVariantMap readGameInfo(const QString &installFolder) const;

    // ── Делегаты к InstallManager ─────────────────────────────────────────────

    Q_INVOKABLE void startInstallation(const QString &tileId,        const QString &title,
                                       const QString &sourceDirPath, const QString &exeRelativePath,
                                       const QString &iconPath,
                                       const QString &installDir = QString())
    { Q_ASSERT(m_installer); m_installer->startInstallation(tileId, title, sourceDirPath, exeRelativePath, iconPath, installDir); }

    Q_INVOKABLE void finalizeInstallation(const QString &tileId,       const QString &title,
                                          const QString &gameRootPath,
                                          const QString &exeRelativePath,
                                          const QString &iconRelativePath,
                                          const QString &kind = "app")
    { Q_ASSERT(m_installer); m_installer->finalizeInstallation(tileId, title, gameRootPath, exeRelativePath, iconRelativePath, kind); }

    Q_INVOKABLE void createExternalShortcut(const QString &tileId,  const QString &title,
                                            const QString &exePath, const QString &iconPath,
                                            const QString &installDir = QString())
    { Q_ASSERT(m_installer); m_installer->createExternalShortcut(tileId, title, exePath, iconPath, installDir); }

    Q_INVOKABLE QString prepareInstallDir(const QString &tileId, const QString &title,
                                          const QString &installDir = QString())
    { Q_ASSERT(m_installer); return m_installer->prepareInstallDir(tileId, title, installDir); }

    Q_INVOKABLE void  cancelInstallation(const QString &tileId)
    { Q_ASSERT(m_installer); m_installer->cancelInstallation(tileId); }

    Q_INVOKABLE bool  isInstalling(const QString &tileId) const
    { Q_ASSERT(m_installer); return m_installer->isInstalling(tileId); }

    Q_INVOKABLE qreal getInstallProgress(const QString &tileId) const
    { Q_ASSERT(m_installer); return m_installer->getInstallProgress(tileId); }

    Q_INVOKABLE void  deleteInstalledGame(const QString &folderPath)
    { Q_ASSERT(m_installer); m_installer->deleteInstalledGame(folderPath); }

    Q_INVOKABLE bool installedGameExists(const QString &installFolder,
                                         const QString &installBasePath) const
    { Q_ASSERT(m_installer); return m_installer->installedGameExists(installFolder, installBasePath); }

    Q_INVOKABLE QString findInstallFolderByTileId(const QString &tileId,
                                                  const QString &installBasePath) const
    { Q_ASSERT(m_installer); return m_installer->findInstallFolderByTileId(tileId, installBasePath); }

    Q_INVOKABLE void scanExeFiles(const QString &requestId, const QString &dirPath)
    { Q_ASSERT(m_installer); m_installer->scanExeFiles(requestId, dirPath); }

    Q_INVOKABLE void getExeDescription(const QString &requestId, const QString &exePath)
    { Q_ASSERT(m_installer); m_installer->getExeDescription(requestId, exePath); }

    Q_INVOKABLE void updateLaunchExe(const QString &tileId,
                                     const QString &installFolder,
                                     const QString &newExeRelative)
    { Q_ASSERT(m_installer); m_installer->updateLaunchExe(tileId, installFolder, newExeRelative); }

    Q_INVOKABLE void checkDependencies(const QString &requestId, const QString &exePath,
                                       const QString &toolPath = QString())
    { Q_ASSERT(m_installer); m_installer->checkDependencies(requestId, exePath, toolPath); }

signals:
    void tileStructureChanged(const QString &storageKey, bool isFolder);

    void installationProgress(const QString &tileId, qreal progress);
    void installationFinished(const QString &tileId, bool success,
                              const QString &ps1Path, const QString &iconPath,
                              const QString &installFolder);
    void installationCancelled(const QString &tileId);
    void exeScanReady(const QString &requestId, const QVariantList &exeFiles);
    void dirListReady(const QString &requestId, const QVariantList &entries);
    void exeDescriptionReady(const QString &requestId, const QString &description);
    void dependencyCheckReady(const QString &requestId, const QStringList &missingDeps);
    void launchExeUpdated(const QString &tileId, bool success,
                          const QString &newPs1Path, const QString &newExeRelative);

private:
    static QVariantList listDirImpl(const QString &path, const QString &filterExts);
    static QVariantList listDrivesImpl();

    QDir        dataDir;
    QString     m_structurePath;
    QJsonObject m_structure;

    // Не-owning указатель. Объектом владеет ModuleHost (через main.cpp).
    InstallManager *m_installer = nullptr;

    // Шина событий — для логирования и уведомлений.
    EventBus *m_bus = nullptr;

    void loadStructure();
    void saveStructure() const;

    // ── Уведомления пользователю — только из GUI-потока ─────────────────────
    void notify(NotificationLevel level,
                const QString     &msg,
                bool               autoDismiss = false,
                int                duration    = 4000) const;

    static bool         findNodeChildren(const QJsonArray  &arr, const QString &id, QJsonArray &out);
    static bool         setNodeChildren (QJsonArray        &arr, const QString &id, const QVariantList &tiles);
    static bool         removeNodeById  (QJsonArray &arr, const QString &id);
    static QJsonArray   mergeChildren   (const QJsonArray &existing, const QVariantList &tiles);
    static QVariantList stripChildren   (const QJsonArray &arr);

    void removeTileFromStructure(const QString &tileId);
    // Результат установки пишется в structure.json напрямую: раньше это делал
    // только открытый экран, и при уходе с экрана во время установки плитка
    // оставалась без пути (игра на диске есть, а запускать нечем).
    void persistInstallResult(const QString &tileId, bool success,
                              const QString &ps1Path, const QString &iconPath,
                              const QString &installFolder);
    // Применяет fields ко всем узлам с данным id; возвращает число изменённых
    static int  updateNodesById(QJsonArray &arr, const QString &id,
                                const QJsonObject &fields);
    static bool nodeHasEmptyPath(const QJsonArray &arr, const QString &id, bool &found);

    int countTilesById(const QString &tileId) const;
    static int countTilesByIdInArray(const QJsonArray &arr, const QString &tileId);

    static const QStringList s_knownScreens;
};
