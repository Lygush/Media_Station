// GameLibraryModule.h
// ─────────────────────────────────────────────────────────────────────────────
// GameLibraryModule — модуль библиотеки игр.
//
// Отвечает за:
//   - Сканирование папки игр (из SettingsManager::libraryPath)
//   - Чтение/запись game_info.json для каждой игры
//   - Архивирование игры (7zr LZMA2, максимальное сжатие) с удалением плиток
//   - Разархивирование (с опциональным созданием плитки)
//   - Создание плитки из библиотеки (через TileManager / EventBus)
//   - Подсчёт размера папок (асинхронный)
//   - Регенерация метаданных для старых игр (legacy)
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QFuture>
#include <QFutureWatcher>
#include <QMap>
#include <atomic>
#include <memory>
#include "core/IModule.h"

class GameLibraryModule : public QObject, public IModule
{
    Q_OBJECT

    Q_PROPERTY(bool scanning    READ scanning    NOTIFY scanningChanged)
    Q_PROPERTY(bool archiving   READ archiving   NOTIFY archivingChanged)
    /// Сжатие средствами Windows (compact.exe) доступно
    Q_PROPERTY(bool compressAvailable READ compressAvailable CONSTANT)

public:
    explicit GameLibraryModule(QObject *parent = nullptr);
    ~GameLibraryModule();

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId() const override { return "gameLibrary"; }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown() override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return "GameLibraryModule"; }

    bool scanning()  const { return m_scanning; }
    bool archiving() const { return !m_archiveTasks.isEmpty(); }
    bool compressAvailable() const;

    // ── API для QML ───────────────────────────────────────────────────────────

    Q_INVOKABLE void scanLibrary();

    // Возвращает текущее состояние m_entries без похода на диск.
    // Используется при повторном открытии экрана библиотеки чтобы
    // восстановить UI без прерывания активных задач архивирования/извлечения.
    Q_INVOKABLE QVariantList currentEntries() const;
    Q_INVOKABLE void requestFolderSize(const QString &gameId, const QString &folderPath);
    Q_INVOKABLE void createTile(const QString &gameId,
                                const QString &storageKey,
                                bool           isFolder);
    Q_INVOKABLE void archiveGame(const QString &gameId);
    Q_INVOKABLE void cancelArchive(const QString &gameId);

    // extractGame — разархивирование.
    // storageKey / isFolder: если не пусты, после успешного извлечения
    // автоматически создаётся плитка в указанном месте.
    // keepArchive: оставить .7z файл после извлечения.
    Q_INVOKABLE void extractGame(const QString &gameId,
                                 bool           keepArchive   = false,
                                 const QString &storageKey    = QString(),
                                 bool           isFolder      = false);
    Q_INVOKABLE void cancelExtract(const QString &gameId);

    /// Сжать папку game/ средствами Windows (WOF, LZX) — игра остаётся рабочей,
    /// занимает меньше места. compress=false — вернуть обычный вид.
    Q_INVOKABLE void compressGame(const QString &gameId, bool compress);
    Q_INVOKABLE void cancelCompress(const QString &gameId);

    Q_INVOKABLE void removeFromLibrary(const QString &gameId);
    // Физически удаляет папку игры (для installed/archive) или только метаданные (external).
    // Также удаляет плитки через шину.
    Q_INVOKABLE void deleteGame(const QString &gameId);
    Q_INVOKABLE void openFolder(const QString &folderPath);
    Q_INVOKABLE void reassignPath(const QString &gameId, const QString &newExePath);

    // Возвращает актуальное количество плиток для игры.
    // Значение синхронизируется через шину (tileManager/tileCountChanged).
    // При первом вызове до синхронизации возвращает значение из game_info.json.
    Q_INVOKABLE int tileCount(const QString &gameId) const;

    // Принудительно генерирует game_info.json для игр с устаревшей структурой (tile_id.txt)
    Q_INVOKABLE void regenerateMetadata(const QString &gameId);

signals:
    void scanningChanged();
    void scanFinished(const QVariantList &entries);
    void folderSizeReady(const QString &gameId, qint64 bytes);
    void tileCreated(const QString &gameId, bool success);
    void archivingChanged();
    void archiveProgress(const QString &gameId, qreal progress, const QString &currentFile);
    void archiveFinished(const QString &gameId, bool success);
    void extractProgress(const QString &gameId, qreal progress);
    void extractFinished(const QString &gameId, bool success);
    void compressProgress(const QString &gameId, qreal progress);
    /// compressed — состояние после операции (при отмене сжатие частичное → true)
    void compressFinished(const QString &gameId, bool success, bool compressed);
    void libraryEntryChanged(const QString &gameId, const QVariantMap &entry);

private:
    QVariantMap scanFolder(const QString &folderPath) const;
    QVariantMap readGameInfo(const QString &folderPath) const;
    bool        writeGameInfo(const QString &folderPath, const QVariantMap &info) const;

    static QString makeGameId(const QString &folderPath);
    static qint64  calcFolderSize(const QString &path);

    // Возвращает плитки, удалённые перед архивацией (отмена или ошибка)
    void restoreTilesAfterArchive(const QString &gameId);
    void clearArchivePending(const QString &installPath);
    void recoverInterruptedArchives();
    void removeTilesFromStructure(const QStringList &tileLocations, const QString &gameId);

    // Возвращает стандартный путь к архиву внутри папки игры:
    // <installPath>/archive/<gameName>.7z
    // Куда класть архив игры: папка архивов из настроек (archivePath) или,
    // если она не задана, <installPath>/archive/<имя>.7z, как раньше.
    QString archivePathForGame(const QString &installPath) const;
    // «Отпечаток» содержимого для кэша размера: время изменения папки и её
    // подпапок первого уровня (для архива — размер и время файла). Не совпал
    // с сохранённым — размер пересчитывается (раньше сохранённый размер не
    // обновлялся никогда, например после обновления игры).
    static QString sizeStamp(const QString &path);
    static QString sizeTargetFor(const QVariantMap &info);
    // Прежнее место архива (внутри папки игры) — для поиска старых архивов
    static QString legacyArchivePath(const QString &installPath);
    // Где лежит существующий архив: сначала по настройке, потом по-старому
    QString findExistingArchive(const QString &installPath) const;

    // Генерирует метаданные для legacy-игры (на основе tile_id.txt)
    QVariantMap generateMetadataForLegacy(const QString &folderPath) const;

    EventBus        *m_bus      = nullptr;
    SettingsManager *m_settings = nullptr;
    bool             m_scanning = false;
    QString          m_appDir;

    QMap<QString, QVariantMap> m_entries;

    struct ArchiveTask {
        QFutureWatcher<bool>               *watcher    = nullptr;
        std::shared_ptr<std::atomic<bool>>  cancelFlag;
        QFuture<bool>                       future;
    };
    QMap<QString, ArchiveTask> m_archiveTasks;

    struct ExtractTask {
        std::shared_ptr<std::atomic<bool>>  cancelFlag;
        QString                             archivePath;
        QString                             destFolder;
    };
    QMap<QString, ExtractTask> m_extractTasks;

    struct CompressTask {
        class QProcess *process  = nullptr;
        bool            compress = true;
        bool            cancelled = false;
        int             totalFiles = 0;     // 0 — ещё считаются
        int             doneFiles  = 0;
        qint64          sizeBefore = 0;
        qreal           progress   = 0.0;
    };
    QMap<QString, CompressTask> m_compressTasks;
    void finishCompress(const QString &gameId, bool ok);
    static QString compressTargetDir(const QString &installPath);

    // Прогресс активных извлечений — для восстановления UI.
    QMap<QString, qreal> m_extractProgress;
};
