// modules/KaraokeModule/KaraokeModule.h
// ─────────────────────────────────────────────────────────────────────────────
// KaraokeModule — IModule + QML-интерфейс для всего пайплайна.
//
// Этапы:
//   0. Анализ имени (быстро): теги → имя файла → при необходимости нейросеть
//      (SongNameResolver → локальная LLM). Результат — metaProposed(); пользователь проверяет /
//      правит название и вызывает confirmSong(artist, title).
//
// Пайплайн (после подтверждения, в фоновом потоке через KaraokeWorker):
//   1. Проверка кэша  (data/songs/Artist - Title/)
//   2. ffmpeg         → temp WAV 44100 stereo
//   3. DemucsWrapper  → vocals.wav + instrumental.wav
//   4. LrcFetcher     → lrclib.net
//      └─ fallback    → ffmpeg 16kHz + WhisperWrapper + LrcBuilder
//   5. AtomicFile     → song.lrc
//
// QML-использование:
//   KaraokeModule.openSong("/path/to/track.mp3")
//   KaraokeModule.onProgress: (percent, stage) => { ... }
//   KaraokeModule.onReady:    (lrcPath, vocalsPath, instrumentalPath) => { ... }
//   KaraokeModule.onError:    (message) => { ... }
//
// EventBus (namespace "karaoke"):
//   moduleEvent("karaoke", "ready",    {songPath, lrcPath, vocalsPath, instrumentalPath})
//   moduleEvent("karaoke", "error",    {songPath, message})
//   moduleEvent("karaoke", "progress", {songPath, percent, stage})
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QString>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QPair>
#include <atomic>

#include "core/IModule.h"
#include "core/EventBus.h"
#include "SongMeta.h"

class SongNameResolver;

class KaraokeModule : public QObject, public IModule
{
    Q_OBJECT

public:
    explicit KaraokeModule(QObject *parent = nullptr);
    ~KaraokeModule() override;

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId()  const override { return "karaoke"; }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown()  override;
    QObject *qmlObject() override      { return this; }
    QString  qmlName()   const override { return "KaraokeModule"; }

    // ── Q_INVOKABLE — вызываются из QML ──────────────────────────────────────

    /// Запускает пайплайн для указанного аудиофайла.
    /// Если кэш уже есть — сразу эмитирует ready().
    /// Если уже идёт обработка — игнорирует вызов.
    /// Шаг 1: анализирует имя/теги файла и предлагает исполнителя и название
    /// (сигнал metaProposed). Тяжёлая обработка НЕ стартует до confirmSong().
    Q_INVOKABLE void openSong(const QString &filePath);

    /// Шаг 2: пользователь подтвердил (или поправил) исполнителя и название —
    /// запускает пайплайн (или сразу отдаёт кэш). artist может быть пустым.
    Q_INVOKABLE void confirmSong(const QString &artist, const QString &title);

    /// Отменяет текущую обработку (если идёт).
    Q_INVOKABLE void cancel();

    /// Возвращает true если для данного файла уже есть готовый кэш.
    Q_INVOKABLE bool isCached(const QString &filePath) const;

    /// Пути к файлам кэша (пустая строка если кэша нет).
    Q_INVOKABLE QString cachedLrcPath(const QString &filePath)            const;
    Q_INVOKABLE QString cachedVocalsPath(const QString &filePath)         const;
    Q_INVOKABLE QString cachedInstrumentalPath(const QString &filePath)   const;

    /// true если пайплайн сейчас выполняется.
    Q_INVOKABLE bool isProcessing() const { return m_processing; }

    // Читает текстовый файл (UTF-8) — QML XMLHttpRequest не умеет читать
    // локальные файлы без QML_XHR_ALLOW_FILE_READ=1.
    Q_INVOKABLE QString readTextFile(const QString &pathOrUrl) const;

signals:
    // ── Сигналы в QML ─────────────────────────────────────────────────────────
    // Анализ имени файла (до подтверждения)
    void analysisStatus(const QString &message);
    void analysisToken(const QString &token);          // потоковый вывод нейросети
    /// source: "tags" | "ai" | "filename" | "manual" (не удалось — ввести вручную)
    void metaProposed(const QString &artist, const QString &title,
                      const QString &source, const QString &fileName);

    void progress(int percent, const QString &stage);
    void ready(const QString &lrcPath,
               const QString &vocalsPath,
               const QString &instrumentalPath);
    void error(const QString &message);

private slots:
    void onWorkerFinished();

private:
    // ── Пути ──────────────────────────────────────────────────────────────────

    /// Корневая папка кэша для данного трека.
    /// Формат: <appDir>/data/songs/<Artist> - <Title>/
    /// Спецсимволы \ / : * ? " < > | заменяются на _
    QString songCacheDir(const QString &filePath) const;

    /// Метаданные с учётом подтверждённых пользователем названий (overrides).
    SongMeta readMeta(const QString &filePath) const;

    /// Теги → имя файла, БЕЗ overrides. source: "tags" | "filename".
    SongMeta readMetaRaw(const QString &filePath, QString *source) const;

    /// Анализ имени (выполняется в фоновом потоке).
    void analyzeSong(const QString &filePath);

    void startPipeline(const SongMeta &meta);
    void emitReady(const QString &filePath);

    QString overridesFile() const;
    void    loadOverrides();
    void    saveOverrides() const;

    // ── Состояние ─────────────────────────────────────────────────────────────
    EventBus               *m_bus        = nullptr;
    QString                 m_currentSongPath;
    std::atomic<bool>       m_cancelled  { false };
    bool                    m_processing { false };

    QFutureWatcher<void>   *m_watcher    = nullptr;

    // ── Анализ имени + нейросеть ──────────────────────────────────────────────
    bool                    m_analyzing  { false };
    QFuture<void>           m_analysisFuture;
    SongNameResolver       *m_resolver   = nullptr;   // нейросеть для имён (логика модуля Karaoke)

    // Подтверждённые пользователем исполнитель/название: путь файла → {artist, title}
    QHash<QString, QPair<QString, QString>> m_overrides;
};
