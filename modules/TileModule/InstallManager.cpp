// InstallManager.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Реализация InstallManager — установка, сканирование, проверка зависимостей.
// Утилиты (createLaunchScript, writeGameInfoFile и др.) → InstallUtils.
// ─────────────────────────────────────────────────────────────────────────────

#include "InstallManager.h"
#include "InstallUtils.h"
#include "LoggerModule.h"
#include "NotificationModule.h"
#include "core/EventBus.h"
#include "tools/AtomicFile.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QMetaObject>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>
#include <QtConcurrent/QtConcurrentRun>
#include <QFuture>
#include <QFutureWatcher>
#include <QPointer>

// ═════════════════════════════════════════════════════════════════════════════
// Конструктор / деструктор
// ═════════════════════════════════════════════════════════════════════════════

InstallManager::InstallManager(QObject *parent)
    : QObject(parent)
    , m_appDir(QCoreApplication::applicationDirPath())
{}

InstallManager::~InstallManager()
{
    shutdown();
}

// ═════════════════════════════════════════════════════════════════════════════
// IModule
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::initialize(EventBus *bus, SettingsManager * /*settings*/)
{
    m_bus = bus;

    // NetworkListener эмитирует networkExtractFinished → финализируем установку.
    // Раньше эта подписка была в TileManager::initialize(), теперь здесь.
    connect(bus, &EventBus::networkExtractFinished,
            this, [this](const QString &tileId,
                         const QString &title,
                         const QString &gameRootPath,
                         const QString &exeRelPath,
                         const QString &iconRelPath,
                         const QString &kind)
    {
        finalizeInstallation(tileId, title, gameRootPath, exeRelPath, iconRelPath, kind);
    });

    qDebug() << "[InstallManager] Initialized";
    log(Logger::kLevelInfo, "Initialized");
}

void InstallManager::shutdown()
{
    if (m_shutdown) return;
    m_shutdown = true;

    cancelAllInstallations();
    m_bus = nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
// Логирование
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::log(const char *level, const QString &msg)
{
    if (!m_bus) return;
    const QByteArray bytes = msg.toUtf8();
    emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                            Logger::makeEvent(level, "installManager", bytes.constData()));
}

void InstallManager::logAsync(const char *level, const QString &msg)
{
    QMetaObject::invokeMethod(this, [this, level, msg]() {
        log(level, msg);
    }, Qt::QueuedConnection);
}

// ═════════════════════════════════════════════════════════════════════════════
// Уведомления
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::notify(NotificationLevel level,
                             const QString     &msg,
                             bool               autoDismiss,
                             int                duration)
{
    if (!m_bus) return;
    emit m_bus->moduleEvent(
        Notifications::kModule,
        Notifications::kEventShow,
        Notifications::makeEvent(msg, level, autoDismiss, duration));
}

// ═════════════════════════════════════════════════════════════════════════════
// Управление активными установками
// ═════════════════════════════════════════════════════════════════════════════

bool InstallManager::isInstalling(const QString &tileId) const
{
    return m_installTasks.contains(tileId);
}

qreal InstallManager::getInstallProgress(const QString &tileId) const
{
    auto it = m_installTasks.find(tileId);
    return it == m_installTasks.end() ? 0.0 : it->progress;
}

void InstallManager::cancelInstallation(const QString &tileId)
{
    auto it = m_installTasks.find(tileId);
    if (it == m_installTasks.end()) return;
    if (it->cancelFlag) it->cancelFlag->store(true);
}

void InstallManager::cancelAllInstallations()
{
    for (auto &task : m_installTasks)
        if (task.cancelFlag) task.cancelFlag->store(true);
    for (auto &task : m_installTasks) {
        if (task.watcher) {
            if (task.watcher->isRunning()) task.watcher->waitForFinished();
            // Обработчик finished уже не найдёт задачу — отключаем его и убираем
            // недокопированную папку здесь (раньше при выходе она оставалась).
            task.watcher->disconnect(this);
            task.watcher->deleteLater();
        }
        if (task.gameRootPtr && !task.gameRootPtr->isEmpty())
            QDir(*task.gameRootPtr).removeRecursively();
    }
    m_installTasks.clear();
}

void InstallManager::deleteInstalledGame(const QString &folderPath)
{
    if (folderPath.isEmpty()) return;
    QDir dir(folderPath);
    if (dir.exists()) {
        dir.removeRecursively();
        qDebug() << "[InstallManager] Deleted game folder:" << folderPath;
        log(Logger::kLevelInfo, QString("Deleted game folder: %1").arg(folderPath));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Локальная установка — асинхронное копирование файлов
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::startInstallation(const QString &tileId,        const QString &title,
                                       const QString &sourceDirPath, const QString &exeRelativePath,
                                       const QString &iconPath,
                                       const QString &installDir)
{
    if (m_installTasks.contains(tileId)) {
        qWarning() << "[InstallManager] Installation already in progress for tile" << tileId;
        return;
    }

    const QString gamesInstallDir = installDir.isEmpty()
        ? m_appDir + "/data/games"
        : installDir;

    auto cancelFlag      = std::make_shared<std::atomic<bool>>(false);
    auto gameRootPathPtr = std::make_shared<QString>();
    auto *watcher        = new QFutureWatcher<QVariantMap>(this);

    InstallTask task;
    task.watcher     = watcher;
    task.cancelFlag  = cancelFlag;
    task.gameRootPtr = gameRootPathPtr;
    task.progress    = 0.0;
    m_installTasks.insert(tileId, task);

    qDebug() << "[InstallManager] Starting local installation:" << tileId << title;
    log(Logger::kLevelInfo, QString("Starting installation: %1 (%2)").arg(tileId, title));

    connect(watcher, &QFutureWatcher<QVariantMap>::finished, this,
            [this, watcher, tileId, title, cancelFlag, gameRootPathPtr]()
    {
        const bool wasCancelled = cancelFlag->load();
        const QVariantMap result = watcher->result();
        const bool success = !wasCancelled && result.contains("ps1Path");

        if (wasCancelled) {
            if (!gameRootPathPtr->isEmpty())
                QDir(*gameRootPathPtr).removeRecursively();
            m_installTasks.remove(tileId);

            qDebug() << "[InstallManager] Installation cancelled:" << tileId;
            log(Logger::kLevelInfo, QString("Installation cancelled: %1").arg(tileId));
            // Пользователь сам нажал отмену — уведомление не нужно.
            emit installationCancelled(tileId);

        } else if (!success) {
            // Ошибка копирования — не оставляем полупустую папку игры
            if (!gameRootPathPtr->isEmpty())
                QDir(*gameRootPathPtr).removeRecursively();
            m_installTasks.remove(tileId);

            qWarning() << "[InstallManager] Installation failed:" << tileId;
            log(Logger::kLevelWarning, QString("Installation failed: %1").arg(tileId));
            notify(NotificationLevel::Error,
                   QString("Ошибка установки: %1").arg(title));
            emit installationFinished(tileId, false, {}, {}, {});

        } else {
            const QString gameFolder = result["gameFolder"].toString();
            m_installTasks.remove(tileId);

            qDebug() << "[InstallManager] Installation finished:" << tileId << "->" << gameFolder;
            log(Logger::kLevelInfo,
                QString("Installation finished: %1 → %2").arg(tileId, gameFolder));
            notify(NotificationLevel::Info,
                   QString("Установка завершена: %1").arg(title),
                   /*autoDismiss=*/true);
            emit installationFinished(tileId, true,
                result["ps1Path"].toString(),
                result["iconPath"].toString(),
                gameFolder);

            // Dep-check незаметно после успешной установки.
            const QString exeRel = result["exeRel"].toString();
            if (!gameFolder.isEmpty() && !exeRel.isEmpty()) {
                QString exeRelFwd = exeRel;
                exeRelFwd.replace('\\', '/');
                const QString exeAbsPath = QDir::fromNativeSeparators(gameFolder)
                                           + "/game/" + exeRelFwd;
                checkDependencies("install_" + tileId, exeAbsPath);
            }
        }
        watcher->deleteLater();
    });

    const QPointer<InstallManager> self(this);
    QFuture<QVariantMap> future = QtConcurrent::run(
        [this, self, tileId, title, sourceDirPath, exeRelativePath, iconPath,
         gamesInstallDir, cancelFlag, gameRootPathPtr]() -> QVariantMap
    {
        const QString sourceDir = QDir::fromNativeSeparators(
            QDir(sourceDirPath).absolutePath());

        if (!QDir(sourceDir).exists()) {
            qWarning() << "[InstallManager] Source dir not found:" << sourceDir;
            logAsync(Logger::kLevelWarning,
                     QString("startInstallation: source dir not found: %1").arg(sourceDir));
            return {};
        }

        QString exeRel = exeRelativePath;
        exeRel.replace('\\', '/');
        const QString exeAbsPath = QDir(sourceDir).absoluteFilePath(exeRel);
        if (!QFile::exists(exeAbsPath)) {
            qWarning() << "[InstallManager] Selected exe not found:" << exeAbsPath;
            logAsync(Logger::kLevelWarning,
                     QString("startInstallation: exe not found: %1").arg(exeAbsPath));
            return {};
        }

        QStringList allFiles;
        QDirIterator it(sourceDir,
                        QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) allFiles.append(it.next());

        if (allFiles.isEmpty()) return {};
        if (cancelFlag->load()) return {};

        QString safeTitle = title;
        static const QRegularExpression kInvalidCharsRe("[<>:\"/\\\\|?*]");
        safeTitle.replace(kInvalidCharsRe, "_");
        const QString gameFolderName = InstallUtils::generateUniqueFolderName(safeTitle, gamesInstallDir);

        const QDir gamesDir(gamesInstallDir);
        if (!gamesDir.mkpath(".")) return {};

        const QString gameRootPath    = gamesDir.filePath(gameFolderName);
        const QString gameSubPath     = gameRootPath + "/game";
        const QString launcherSubPath = gameRootPath + "/launcher";

        if (!QDir(gameRootPath).mkpath(".")  ||
            !QDir(gameSubPath).mkpath(".")    ||
            !QDir(launcherSubPath).mkpath("."))
            return {};

        InstallUtils::writeTileIdFile(gameRootPath, tileId);
        *gameRootPathPtr = gameRootPath;

        QVariantMap result;
        result["gameFolder"] = QDir::toNativeSeparators(gameRootPath);

        // ── Копирование файлов ────────────────────────────────────────────────
        const int totalFiles = allFiles.size();
        int copied = 0, lastPercent = -1;

        for (const QString &srcFile : allFiles) {
            if (cancelFlag->load()) return {};

            const QString relativePath = QDir::fromNativeSeparators(
                QFileInfo(srcFile).absoluteFilePath()).mid(sourceDir.length() + 1);
            const QString destFile = QDir(gameSubPath).filePath(relativePath);

            if (!QFileInfo(destFile).absoluteDir().mkpath(".")) {
                qWarning() << "[InstallManager] mkdir failed for" << destFile;
                return {};
            }
            if (!InstallUtils::copyFileCancelable(srcFile, destFile, cancelFlag)) {
                if (!cancelFlag->load())
                    qWarning() << "[InstallManager] Copy failed:" << srcFile;
                return {};
            }

            ++copied;
            const int percent = int(100.0 * copied / totalFiles);
            if (percent != lastPercent) {
                lastPercent = percent;
                const qreal progress = qreal(copied) / totalFiles;
                if (self) QMetaObject::invokeMethod(self.data(), [this, tileId, progress]() {
                    if (m_installTasks.contains(tileId))
                        m_installTasks[tileId].progress = progress;
                    emit installationProgress(tileId, progress);
                }, Qt::QueuedConnection);
            }
        }

        if (cancelFlag->load()) return {};

        // ── Копирование иконки ────────────────────────────────────────────────
        QString newIconPath;
        if (!iconPath.isEmpty()) {
            const QFileInfo iconInfo(iconPath);
            if (iconInfo.exists()) {
                const QString destIcon = launcherSubPath + "/icon." + iconInfo.suffix();
                if (QFile::copy(iconPath, destIcon))
                    newIconPath = QDir::toNativeSeparators(destIcon);
            }
        }

        if (cancelFlag->load()) return {};

        // ── Создание скрипта запуска ──────────────────────────────────────────
        QString exeRelForScript = exeRel;
        exeRelForScript.replace('/', '\\');
        const QString ps1Path = InstallUtils::createLaunchScript(gameRootPath, exeRelForScript, "app");
        if (ps1Path.isEmpty()) return {};

        if (cancelFlag->load()) return {};

        // ── Запись game_info.json ─────────────────────────────────────────────
        InstallUtils::writeGameInfoFile(gameRootPath, tileId, title, "installed",
                                        exeRelForScript, newIconPath);

        result["ps1Path"]  = QDir::toNativeSeparators(ps1Path);
        result["iconPath"] = newIconPath;
        result["exeRel"]   = exeRelForScript;
        return result;
    });

    m_installTasks[tileId].future = future;
    watcher->setFuture(future);
}

// ═════════════════════════════════════════════════════════════════════════════
// finalizeInstallation — финализация после распаковки архива (сетевая)
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::finalizeInstallation(const QString &tileId,   const QString &title,
                                          const QString &gameRootPath,
                                          const QString &exeRelativePath,
                                          const QString &iconRelativePath,
                                          const QString &kind)
{
    Q_UNUSED(title)

    const QString gameSubPath     = gameRootPath + "/game";
    const QString launcherSubPath = gameRootPath + "/launcher";
    // Отображаемое имя для уведомлений — берём из имени папки если title пуст.
    const QString displayTitle = title.isEmpty() ? QDir(gameRootPath).dirName() : title;

    QString exeFullPath;
    if (kind == "app") {
        if (!exeRelativePath.isEmpty()) {
            QString normalizedRel = exeRelativePath;
            normalizedRel.replace('\\', '/');
            const QString directCandidate = QDir(gameSubPath).absoluteFilePath(normalizedRel);
            if (QFile::exists(directCandidate)) {
                exeFullPath = directCandidate;
            } else {
                const QString exeFileName = QFileInfo(normalizedRel).fileName();
                QDirIterator it(gameSubPath, QStringList() << exeFileName,
                                QDir::Files, QDirIterator::Subdirectories);
                if (it.hasNext()) exeFullPath = it.next();
            }
        }

        if (exeFullPath.isEmpty()) {
            QDirIterator it(gameSubPath, QStringList() << "*.exe",
                            QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString candidate = it.next();
                const QString baseName  = QFileInfo(candidate).baseName().toLower();
                bool skip = false;
                for (const QString &pat : InstallUtils::kSkipExePatterns)
                    if (baseName.contains(pat)) { skip = true; break; }
                if (!skip) { exeFullPath = candidate; break; }
            }
        }
        if (exeFullPath.isEmpty()) {
            QDirIterator it(gameSubPath, QStringList() << "*.exe",
                            QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext()) exeFullPath = it.next();
        }

        if (exeFullPath.isEmpty()) {
            qWarning() << "[InstallManager] finalizeInstallation: no exe in" << gameSubPath;
            log(Logger::kLevelWarning,
                QString("finalizeInstallation: no exe found for %1").arg(tileId));
            notify(NotificationLevel::Error,
                   QString("Ошибка установки: не найден исполняемый файл (%1)").arg(displayTitle));
            emit installationFinished(tileId, false, {}, {}, {});
            return;
        }
    }

    // ── Копирование иконки ─────────────────────────────────────────────────────
    QString newIconPath;
    if (!iconRelativePath.isEmpty()) {
        const QString srcIcon = QDir(gameSubPath).absoluteFilePath(iconRelativePath);
        if (QFile::exists(srcIcon)) {
            const QString destIcon = launcherSubPath + "/icon." + QFileInfo(srcIcon).suffix();
            if (QFile::copy(srcIcon, destIcon))
                newIconPath = QDir::toNativeSeparators(destIcon);
        }
    }

    // ── Генерация скрипта запуска ──────────────────────────────────────────────
    QString exeRelativeInGame;
    if (kind == "app") {
        exeRelativeInGame = QDir(gameSubPath).relativeFilePath(exeFullPath);
        exeRelativeInGame.replace('/', '\\');
    }

    const QString ps1Path = InstallUtils::createLaunchScript(gameRootPath, exeRelativeInGame, kind);
    if (ps1Path.isEmpty()) {
        qWarning() << "[InstallManager] finalizeInstallation: failed to create launch.ps1 for" << tileId;
        log(Logger::kLevelWarning,
            QString("finalizeInstallation: failed to create launch.ps1 for %1").arg(tileId));
        notify(NotificationLevel::Error,
               QString("Ошибка установки: не удалось создать скрипт запуска (%1)").arg(displayTitle));
        emit installationFinished(tileId, false, {}, {}, {});
        return;
    }

    // ── Запись game_info.json ──────────────────────────────────────────────────
    const QString gameTitle = QDir(gameRootPath).dirName();
    const QString exeForInfo = (kind == "app") ? exeRelativeInGame : QString();
    InstallUtils::writeGameInfoFile(gameRootPath, tileId, gameTitle, "installed",
                                    exeForInfo, newIconPath);

    qDebug() << "[InstallManager] finalizeInstallation: done" << tileId;
    log(Logger::kLevelInfo,
        QString("Network installation finalized: %1 → %2")
        .arg(tileId, QDir::toNativeSeparators(gameRootPath)));
    notify(NotificationLevel::Info,
           QString("Установка завершена: %1").arg(displayTitle),
           /*autoDismiss=*/true);

    emit installationFinished(tileId, true,
                              ps1Path,
                              newIconPath,
                              QDir::toNativeSeparators(gameRootPath));

    if (!exeFullPath.isEmpty())
        checkDependencies("install_" + tileId, QDir::fromNativeSeparators(exeFullPath));
}

// ═════════════════════════════════════════════════════════════════════════════
// createExternalShortcut — ярлык на внешнее приложение
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::createExternalShortcut(const QString &tileId,  const QString &title,
                                            const QString &exePath, const QString &iconPath,
                                            const QString &installDir)
{
    const QString gamesInstallDir = installDir.isEmpty()
        ? m_appDir + "/data/games"
        : installDir;

    QString safeTitle = title;
    static const QRegularExpression kInvalidCharsRe("[<>:\"/\\\\|?*]");
    safeTitle.replace(kInvalidCharsRe, "_");
    const QString gameFolderName = InstallUtils::generateUniqueFolderName(safeTitle, gamesInstallDir);

    const QDir gamesDir(gamesInstallDir);
    if (!gamesDir.mkpath(".")) {
        log(Logger::kLevelWarning,
            QString("createExternalShortcut: cannot create games dir for %1").arg(tileId));
        notify(NotificationLevel::Error,
               QString("Ошибка создания ярлыка: %1").arg(title));
        emit installationFinished(tileId, false, {}, {}, {});
        return;
    }

    const QString gameRootPath    = gamesDir.filePath(gameFolderName);
    const QString launcherSubPath = gameRootPath + "/launcher";

    if (!QDir(gameRootPath).mkpath(".")          ||
        !QDir(gameRootPath + "/game").mkpath(".")  ||
        !QDir(launcherSubPath).mkpath(".")) {
        log(Logger::kLevelWarning,
            QString("createExternalShortcut: cannot create subdirs for %1").arg(tileId));
        notify(NotificationLevel::Error,
               QString("Ошибка создания ярлыка: %1").arg(title));
        emit installationFinished(tileId, false, {}, {}, {});
        return;
    }

    InstallUtils::writeTileIdFile(gameRootPath, tileId);

    QString newIconPath;
    if (!iconPath.isEmpty()) {
        const QFileInfo iconInfo(iconPath);
        if (iconInfo.exists()) {
            const QString destIcon = launcherSubPath + "/icon." + iconInfo.suffix();
            if (QFile::copy(iconPath, destIcon))
                newIconPath = QDir::toNativeSeparators(destIcon);
        }
    }

    const QString ps1Path = InstallUtils::createLaunchScript(gameRootPath, exePath, "external");
    if (ps1Path.isEmpty()) {
        QDir(gameRootPath).removeRecursively();
        log(Logger::kLevelWarning,
            QString("createExternalShortcut: failed to create launch.ps1 for %1").arg(tileId));
        notify(NotificationLevel::Error,
               QString("Ошибка создания ярлыка: %1").arg(title));
        emit installationFinished(tileId, false, {}, {}, {});
        return;
    }

    InstallUtils::writeGameInfoFile(gameRootPath, tileId, title, "external", exePath, newIconPath);

    qDebug() << "[InstallManager] External shortcut created:" << tileId;
    log(Logger::kLevelInfo,
        QString("External shortcut created: %1 (%2)").arg(tileId, title));
    notify(NotificationLevel::Info,
           QString("Ярлык создан: %1").arg(title),
           /*autoDismiss=*/true);

    emit installationFinished(tileId, true,
                              ps1Path,
                              newIconPath,
                              QDir::toNativeSeparators(gameRootPath));
}

// ═════════════════════════════════════════════════════════════════════════════
// prepareInstallDir — создаёт структуру каталогов для сетевой установки
// ═════════════════════════════════════════════════════════════════════════════

QString InstallManager::prepareInstallDir(const QString &tileId, const QString &title,
                                          const QString &installDir)
{
    const QString gamesInstallDir = installDir.isEmpty()
        ? m_appDir + "/data/games"
        : installDir;

    QString safeTitle = title;
    static const QRegularExpression kInvalidCharsRe("[<>:\"/\\\\|?*]");
    safeTitle.replace(kInvalidCharsRe, "_");
    const QString gameFolderName = InstallUtils::generateUniqueFolderName(safeTitle, gamesInstallDir);

    const QDir gamesDir(gamesInstallDir);
    if (!gamesDir.mkpath(".")) {
        qWarning() << "[InstallManager] Cannot create games dir:" << gamesInstallDir;
        log(Logger::kLevelWarning,
            QString("prepareInstallDir: cannot create games dir for %1").arg(tileId));
        return {};
    }

    const QString gameRootPath = gamesDir.filePath(gameFolderName);
    if (!QDir(gameRootPath).mkpath(".")           ||
        !QDir(gameRootPath + "/game").mkpath(".")  ||
        !QDir(gameRootPath + "/launcher").mkpath(".")) {
        qWarning() << "[InstallManager] Cannot create subdirs for:" << gameRootPath;
        log(Logger::kLevelWarning,
            QString("prepareInstallDir: cannot create subdirs for %1").arg(tileId));
        return {};
    }

    InstallUtils::writeTileIdFile(gameRootPath, tileId);
    return gameRootPath;
}

// ═════════════════════════════════════════════════════════════════════════════
// Поиск установленных игр
// ═════════════════════════════════════════════════════════════════════════════

QString InstallManager::resolveInstallFolder(const QString &installFolder,
                                             const QString &installBasePath) const
{
    if (!installFolder.isEmpty()) {
        const QDir dir(installFolder);
        if (dir.exists()) return QDir::toNativeSeparators(dir.absolutePath());
    }
    if (!installBasePath.isEmpty()) {
        const QDir base(installBasePath);
        const QFileInfoList entries = base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &e : entries) {
            if (QFile::exists(e.absoluteFilePath() + "/launcher/launch.ps1"))
                return QDir::toNativeSeparators(e.absoluteFilePath());
        }
    }
    return {};
}

bool InstallManager::installedGameExists(const QString &installFolder,
                                         const QString &installBasePath) const
{
    Q_UNUSED(installBasePath)
    return !installFolder.isEmpty() && QDir(installFolder).exists();
}

QString InstallManager::findInstallFolderByTileId(const QString &tileId,
                                                  const QString &installBasePath) const
{
    if (tileId.isEmpty() || installBasePath.isEmpty()) return {};
    const QDir base(installBasePath);
    if (!base.exists()) return {};

    const QFileInfoList entries = base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        QFile f(entry.absoluteFilePath() + "/tile_id.txt");
        if (f.open(QIODevice::ReadOnly)) {
            const QString storedId = QString::fromUtf8(f.readAll()).trimmed();
            f.close();
            if (storedId == tileId)
                return QDir::toNativeSeparators(entry.absoluteFilePath());
        }
    }
    return {};
}

// ═════════════════════════════════════════════════════════════════════════════
// scanExeFiles — асинхронный рекурсивный поиск .exe в папке игры
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::scanExeFiles(const QString &requestId, const QString &dirPath)
{
    const QPointer<InstallManager> self(this);
    QFuture<void> scanFuture = QtConcurrent::run([this, self, requestId, dirPath]() {
        const QString rootDir = QDir::fromNativeSeparators(
            QDir(dirPath).absolutePath());

        QVariantList result;
        QDirIterator it(rootDir, QStringList() << "*.exe",
                        QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);

        while (it.hasNext()) {
            const QString absPath  = QDir::fromNativeSeparators(it.next());
            const QString baseName = QFileInfo(absPath).baseName().toLower();

            bool skip = false;
            for (const QString &pat : InstallUtils::kSkipExePatterns)
                if (baseName.contains(pat)) { skip = true; break; }
            if (skip) continue;

            QString rel = absPath;
            if (rel.startsWith(rootDir))
                rel = rel.mid(rootDir.length());
            if (!rel.isEmpty() && (rel[0] == '/' || rel[0] == '\\'))
                rel = rel.mid(1);

            QVariantMap entry;
            entry["name"]     = QFileInfo(absPath).fileName();
            entry["relative"] = rel;
            result.append(entry);
        }

        if (self) QMetaObject::invokeMethod(self.data(), [this, requestId, result]() {
            emit exeScanReady(requestId, result);
        }, Qt::QueuedConnection);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// getExeDescription — асинхронно читает ProductName / FileDescription из exe
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::getExeDescription(const QString &requestId, const QString &exePath)
{
    const QPointer<InstallManager> self(this);
    QFuture<void> descFuture = QtConcurrent::run([this, self, requestId, exePath]() {
        QString description;
#ifdef Q_OS_WIN
        const QString nativePath = QDir::toNativeSeparators(exePath);
        const QString safePath   = QString(nativePath).replace("'", "''");
        // OutputEncoding=UTF8 — иначе PowerShell пишет в OEM-кодировке и
        // кириллические названия приходили «кракозябрами».
        const QString script = QString(
            "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; "
            "$v = (Get-Item -LiteralPath '%1').VersionInfo; "
            "if ($v.ProductName -and $v.ProductName.Trim() -ne '') { $v.ProductName.Trim() } "
            "elseif ($v.FileDescription -and $v.FileDescription.Trim() -ne '') { $v.FileDescription.Trim() } "
            "else { '' }"
        ).arg(safePath);

        QProcess process;
        process.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command", script});
        if (process.waitForFinished(5000))
            description = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
#else
        Q_UNUSED(exePath)
#endif
        if (self) QMetaObject::invokeMethod(self.data(), [this, requestId, description]() {
            emit exeDescriptionReady(requestId, description);
        }, Qt::QueuedConnection);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// updateLaunchExe — пересоздаёт launch.ps1 при смене exe в установленной игре
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::updateLaunchExe(const QString &tileId,
                                     const QString &installFolder,
                                     const QString &newExeRelative)
{
    if (installFolder.isEmpty() || newExeRelative.isEmpty()) {
        qWarning() << "[InstallManager] updateLaunchExe: empty arguments";
        emit launchExeUpdated(tileId, false, {}, {});
        return;
    }

    const QPointer<InstallManager> self(this);
    QFuture<void> updateFuture = QtConcurrent::run([this, self, tileId, installFolder, newExeRelative]() {
        const QString gameRoot = QDir::fromNativeSeparators(installFolder);
        const QString gameDir  = gameRoot + "/game";

        QString exeRel = newExeRelative;
        exeRel.replace('\\', '/');
        const QString exeAbs = QDir(gameDir).absoluteFilePath(exeRel);

        if (!QFile::exists(exeAbs)) {
            qWarning() << "[InstallManager] updateLaunchExe: exe not found:" << exeAbs;
            if (self) QMetaObject::invokeMethod(self.data(), [this, tileId]() {
                log(Logger::kLevelWarning,
                    QString("updateLaunchExe: exe not found for %1").arg(tileId));
                notify(NotificationLevel::Error,
                       QString("Ошибка смены exe: файл не найден"));
                emit launchExeUpdated(tileId, false, {}, {});
            }, Qt::QueuedConnection);
            return;
        }

        QString exeRelNative = exeRel;
        exeRelNative.replace('/', '\\');
        const QString ps1Path = InstallUtils::createLaunchScript(gameRoot, exeRelNative, "app");
        const bool ok = !ps1Path.isEmpty();

        if (ok) {
            qDebug() << "[InstallManager] updateLaunchExe: OK" << exeRelNative;

            const QString infoPath = gameRoot + "/game_info.json";
            if (QFile::exists(infoPath)) {
                QFile f(infoPath);
                if (f.open(QIODevice::ReadOnly)) {
                    QJsonParseError err;
                    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
                    f.close();
                    if (err.error == QJsonParseError::NoError && doc.isObject()) {
                        QJsonObject obj = doc.object();
                        obj["executablePath"] = exeRelNative;
                        AtomicFile::writeJson(infoPath, QJsonDocument(obj));
                    }
                }
            }
            checkDependencies("update_" + tileId, exeAbs);
        } else {
            qWarning() << "[InstallManager] updateLaunchExe: failed to write ps1 for" << tileId;
        }

        const QString ps1Native = ok ? QDir::toNativeSeparators(ps1Path) : QString();
        const QString exeFinal  = ok ? exeRelNative : QString();

        if (self) QMetaObject::invokeMethod(self.data(), [this, tileId, ok, ps1Native, exeFinal]() {
            if (ok) {
                log(Logger::kLevelInfo,
                    QString("Launch exe updated: %1 → %2").arg(tileId, exeFinal));
            } else {
                log(Logger::kLevelWarning,
                    QString("updateLaunchExe: failed to write ps1 for %1").arg(tileId));
                notify(NotificationLevel::Error,
                       QString("Ошибка смены exe: не удалось создать скрипт запуска"));
            }
            emit launchExeUpdated(tileId, ok, ps1Native, exeFinal);
        }, Qt::QueuedConnection);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// checkDependencies — проверка зависимостей через PEDependencyAnalyzer.exe
//
// Парсинг вывода вынесен в InstallUtils::parseDependencyAnalyzerOutput() —
// чистую функцию без побочных эффектов, которую можно тестировать отдельно.
// ═════════════════════════════════════════════════════════════════════════════

void InstallManager::checkDependencies(const QString &requestId,
                                       const QString &exePath,
                                       const QString &toolPath)
{
    qDebug() << "[InstallManager] checkDependencies requestId:" << requestId
             << "exe:" << QFileInfo(exePath).fileName();

    const QPointer<InstallManager> self(this);
    QFuture<void> depFuture = QtConcurrent::run([this, self, requestId, exePath, toolPath]() {

#ifdef Q_OS_WIN
        // ── Определяем путь к анализатору ────────────────────────────────────
        const QString analyzerPath = toolPath.isEmpty()
            ? m_appDir + "/tools/PEDependencyAnalyzer.exe"
            : toolPath;

        if (!QFile::exists(analyzerPath)) {
            qWarning() << "[InstallManager] PEDependencyAnalyzer not found:" << analyzerPath;
            if (self) QMetaObject::invokeMethod(self.data(), [this, requestId]() {
                emit dependencyCheckReady(requestId, {});
            }, Qt::QueuedConnection);
            return;
        }

        // ── Запускаем процесс ─────────────────────────────────────────────────
        QProcess process;
        process.start(analyzerPath, { QDir::toNativeSeparators(exePath) });

        if (!process.waitForStarted(5000)) {
            qWarning() << "[InstallManager] Failed to start PEDependencyAnalyzer:"
                       << process.errorString();
            if (self) QMetaObject::invokeMethod(self.data(), [this, requestId]() {
                emit dependencyCheckReady(requestId, {});
            }, Qt::QueuedConnection);
            return;
        }
        if (!process.waitForFinished(30000)) {
            qWarning() << "[InstallManager] PEDependencyAnalyzer timed out for"
                       << QFileInfo(exePath).fileName();
            process.kill();
            if (self) QMetaObject::invokeMethod(self.data(), [this, requestId]() {
                emit dependencyCheckReady(requestId, {});
            }, Qt::QueuedConnection);
            return;
        }

        // ── Парсим вывод ──────────────────────────────────────────────────────
        const QString rawOutput = QString::fromUtf8(process.readAllStandardOutput());
        const InstallUtils::DepsResult deps = InstallUtils::parseDependencyAnalyzerOutput(rawOutput);

        // ── Обновляем inputTypes в game_info.json ─────────────────────────────
        if (!deps.inputTypes.isEmpty()) {
            QString gameInfoPath;
            QDir searchDir = QFileInfo(exePath).absoluteDir();
            for (int i = 0; i < 5; ++i) {
                const QString candidate = searchDir.filePath("game_info.json");
                if (QFile::exists(candidate)) { gameInfoPath = candidate; break; }
                if (!searchDir.cdUp()) break;
            }
            if (!gameInfoPath.isEmpty()) {
                QFile f(gameInfoPath);
                if (f.open(QIODevice::ReadOnly)) {
                    QJsonParseError err;
                    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
                    f.close();
                    if (err.error == QJsonParseError::NoError && doc.isObject()) {
                        QJsonObject obj = doc.object();
                        QJsonArray arr;
                        for (const QString &t : deps.inputTypes) arr.append(t);
                        obj["inputTypes"] = arr;
                        AtomicFile::writeJson(gameInfoPath, QJsonDocument(obj));
                    }
                }
            }
        }

        // ── Публикуем результат в GUI-поток ───────────────────────────────────
        const QString exeName = QFileInfo(exePath).fileName();
        if (self) QMetaObject::invokeMethod(self.data(), [this, requestId, deps, exeName]() {
            if (deps.missing.isEmpty()) {
                const QString msg = QString("Dependencies OK for %1 inputTypes=[%2]")
                    .arg(exeName, deps.inputTypes.join(", "));
                qDebug() << "[InstallManager]" << msg;
                log(Logger::kLevelInfo, msg);
            } else {
                const QString msg = QString("Missing dependencies for %1: %2")
                    .arg(exeName, deps.missing.join(", "));
                qWarning() << "[InstallManager]" << msg;
                log(Logger::kLevelWarning, msg);
                notify(NotificationLevel::Warning,
                       QString("Внимание: не найдены библиотеки для %1:\n%2")
                           .arg(exeName, deps.missing.join(", ")));
            }
            emit dependencyCheckReady(requestId, deps.missing);
        }, Qt::QueuedConnection);

#else
        Q_UNUSED(exePath)
        Q_UNUSED(toolPath)
        if (self) QMetaObject::invokeMethod(self.data(), [this, requestId]() {
            emit dependencyCheckReady(requestId, {});
        }, Qt::QueuedConnection);
#endif
    });
}
