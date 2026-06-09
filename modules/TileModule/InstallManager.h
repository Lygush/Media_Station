// InstallManager.h
// ─────────────────────────────────────────────────────────────────────────────
// InstallManager — вся логика установки и обслуживания игр.
//
// Теперь является IModule: регистрируется в main.cpp, получает EventBus
// через initialize() и сам подписывается на networkExtractFinished.
//
// TileManager остаётся фасадом для QML — он держит указатель на InstallManager
// и делегирует вызовы. Прямой доступ из QML к InstallManager не нужен.
//
// Отвечает за:
//   - Локальную установку (асинхронное копирование с поддержкой отмены)
//   - Финализацию сетевой установки после распаковки архива
//   - Создание ярлыков на внешние приложения
//   - Подготовку каталога для сетевой установки
//   - Генерацию PowerShell-скриптов запуска (launch.ps1)  [→ InstallUtils]
//   - Запись game_info.json при каждой установке          [→ InstallUtils]
//   - Удаление установленных игр
//   - Поиск установленных игр по tileId / пути
//   - Асинхронный поиск exe-файлов в папке игры
//   - Получение описания exe через VersionInfo (PowerShell)
//   - Проверку зависимостей через PEDependencyAnalyzer
//   - Логирование ключевых событий через LoggerModule (EventBus)
//   - Пользовательские уведомления через NotificationModule (EventBus)
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QDir>
#include <QVariantList>
#include <QVariantMap>
#include <QFutureWatcher>
#include <QFuture>
#include <QMap>
#include <memory>
#include <atomic>

#include "core/IModule.h"
#include "NotificationModule.h"   // для NotificationLevel и Notifications::

class EventBus;
class SettingsManager;

class InstallManager : public QObject, public IModule
{
    Q_OBJECT

public:
    explicit InstallManager(QObject *parent = nullptr);
    ~InstallManager() override;

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId()  const override { return QStringLiteral("installManager"); }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown()  override;
    QObject *qmlObject() override       { return this; }
    QString  qmlName()   const override { return QStringLiteral("InstallManager"); }

    // ── Локальная установка ───────────────────────────────────────────────────
    Q_INVOKABLE void startInstallation(const QString &tileId,        const QString &title,
                                       const QString &sourceDirPath, const QString &exeRelativePath,
                                       const QString &iconPath,
                                       const QString &installDir = QString());

    // ── Сетевая установка (финализация после распаковки архива 7z) ───────────
    Q_INVOKABLE void finalizeInstallation(const QString &tileId,       const QString &title,
                                          const QString &gameRootPath,
                                          const QString &exeRelativePath,
                                          const QString &iconRelativePath,
                                          const QString &kind = "app");

    // ── Ярлык на внешнее приложение ───────────────────────────────────────────
    Q_INVOKABLE void createExternalShortcut(const QString &tileId,  const QString &title,
                                            const QString &exePath, const QString &iconPath,
                                            const QString &installDir = QString());

    // ── Подготовка каталога для сетевой установки (синхронный) ───────────────
    Q_INVOKABLE QString prepareInstallDir(const QString &tileId,  const QString &title,
                                          const QString &installDir = QString());

    // ── Управление активной установкой ────────────────────────────────────────
    Q_INVOKABLE void  cancelInstallation(const QString &tileId);
    Q_INVOKABLE bool  isInstalling(const QString &tileId) const;
    Q_INVOKABLE qreal getInstallProgress(const QString &tileId) const;

    void cancelAllInstallations();

    // ── Удаление установленной игры ───────────────────────────────────────────
    Q_INVOKABLE void deleteInstalledGame(const QString &folderPath);

    // ── Поиск установленных игр ───────────────────────────────────────────────
    Q_INVOKABLE QString resolveInstallFolder(const QString &installFolder,
                                             const QString &installBasePath) const;
    Q_INVOKABLE bool    installedGameExists(const QString &installFolder,
                                            const QString &installBasePath) const;
    Q_INVOKABLE QString findInstallFolderByTileId(const QString &tileId,
                                                  const QString &installBasePath) const;

    // ── Сканирование exe (асинхронное) ────────────────────────────────────────
    Q_INVOKABLE void scanExeFiles(const QString &requestId, const QString &dirPath);

    // ── Получение описания exe через VersionInfo (асинхронное) ───────────────
    Q_INVOKABLE void getExeDescription(const QString &requestId, const QString &exePath);

    // ── Смена exe в установленной игре (пересоздаёт launch.ps1) ─────────────
    Q_INVOKABLE void updateLaunchExe(const QString &tileId,
                                     const QString &installFolder,
                                     const QString &newExeRelative);

    // ── Проверка зависимостей через PEDependencyAnalyzer (асинхронная) ────────
    Q_INVOKABLE void checkDependencies(const QString &requestId,
                                       const QString &exePath,
                                       const QString &toolPath = QString());

signals:
    void installationProgress(const QString &tileId, qreal progress);
    void installationFinished(const QString &tileId, bool success,
                              const QString &ps1Path, const QString &iconPath,
                              const QString &installFolder);
    void installationCancelled(const QString &tileId);

    void exeScanReady(const QString &requestId, const QVariantList &exeFiles);
    void exeDescriptionReady(const QString &requestId, const QString &description);
    void dependencyCheckReady(const QString &requestId, const QStringList &missingDeps);
    void launchExeUpdated(const QString &tileId, bool success,
                          const QString &newPs1Path, const QString &newExeRelative);

private:
    // ── Логирование — только из GUI-потока ────────────────────────────────────
    void log(const char *level, const QString &msg);

    // ── Логирование — thread-safe (из фоновых потоков) ───────────────────────
    void logAsync(const char *level, const QString &msg);

    // ── Уведомления пользователю — только из GUI-потока ─────────────────────
    // autoDismiss=true  → исчезает само (для успешных операций)
    // autoDismiss=false → пользователь закрывает вручную (для ошибок/предупреждений)
    void notify(NotificationLevel level,
                const QString     &msg,
                bool               autoDismiss = false,
                int                duration    = 4000);

    // ── Активные задачи установки ─────────────────────────────────────────────
    struct InstallTask {
        QFutureWatcher<QVariantMap> *watcher    = nullptr;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        std::shared_ptr<QString>    gameRootPtr;
        qreal                       progress    = 0.0;
        QFuture<QVariantMap>        future;
    };

    QMap<QString, InstallTask> m_installTasks;

    EventBus *m_bus      = nullptr;
    bool      m_shutdown = false;

    QString m_appDir;
};
