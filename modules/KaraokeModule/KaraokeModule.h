// modules/KaraokeModule/KaraokeModule.h
// ─────────────────────────────────────────────────────────────────────────────
// KaraokeModule — IModule + QML-интерфейс для всего пайплайна.
//
// Пайплайн (выполняется в фоновом потоке через KaraokeWorker):
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
#include <atomic>

#include "core/IModule.h"
#include "core/EventBus.h"
#include "SongMeta.h"

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
    Q_INVOKABLE void openSong(const QString &filePath);

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

signals:
    // ── Сигналы в QML ─────────────────────────────────────────────────────────
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

    /// Извлекает метаданные из ID3-тегов файла.
    /// Если тегов нет — использует имя файла.
    SongMeta readMeta(const QString &filePath) const;

    // ── Состояние ─────────────────────────────────────────────────────────────
    EventBus               *m_bus        = nullptr;
    QString                 m_currentSongPath;
    std::atomic<bool>       m_cancelled  { false };
    bool                    m_processing { false };

    QFutureWatcher<void>   *m_watcher    = nullptr;
};
