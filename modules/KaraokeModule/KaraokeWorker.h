// modules/KaraokeModule/KaraokeWorker.h
// ─────────────────────────────────────────────────────────────────────────────
// KaraokeWorker — весь тяжёлый пайплайн в фоновом потоке.
//
// Запускается через QtConcurrent::run из KaraokeModule::openSong().
// Результат сигнализируется через KaraokeModule::onWorkerFinished().
//
// Пайплайн:
//   1. ffmpeg:         inputFile → temp_input.wav  (44100 Hz stereo 16-bit)
//   2. DemucsWrapper:  temp_input.wav → vocals.wav + instrumental.wav
//   3. LrcFetcher:     lrclib.net → LrcResult
//      hasSynced  → сохраняем song.lrc напрямую
//      иначе:
//   4. ffmpeg:         vocals.wav → vocals_16k.wav  (16kHz mono)
//   5. WhisperWrapper: vocals_16k.wav → QVector<WhisperWord>
//   6. LrcBuilder:     QVector<WhisperWord> → Enhanced LRC строка
//   7. AtomicFile:     сохраняем song.lrc
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>
#include <functional>
#include <atomic>

#include "SongMeta.h"

using KaraokeProgressCb = std::function<void(int percent, const QString &stage)>;

class KaraokeWorker
{
public:
    /// @param appDir     QCoreApplication::applicationDirPath()
    /// @param meta       Метаданные трека (artist, title, album, duration, filePath)
    /// @param cacheDir   Папка для результатов (data/songs/Artist - Title/)
    /// @param cancelled  Флаг отмены, пробрасывается во все подкомпоненты
    KaraokeWorker(const QString          &appDir,
                  const SongMeta         &meta,
                  const QString          &cacheDir,
                  const std::atomic<bool> *cancelled);

    void setProgressCallback(KaraokeProgressCb cb) { m_progressCb = cb; }

    /// Запускает пайплайн синхронно (вызывается из фонового потока).
    void run();

private:
    // ── Шаги пайплайна ────────────────────────────────────────────────────────
    bool stepFfmpegToWav(const QString &inputPath, const QString &outputPath);
    bool stepDemucs(const QString &inputWav,
                    const QString &vocalsOut,
                    const QString &instrOut);
    bool stepLrc(const QString &vocalsWav,
                 const QString &lrcOut);
    bool stepFfmpegTo16k(const QString &inputWav, const QString &outputWav);

    // ── Helpers ───────────────────────────────────────────────────────────────
    void reportProgress(int percent, const QString &stage);
    bool isCancelled() const;
    QString ffmpegPath() const;   // appDir/external_tools/ffmpeg.exe
    QString whisperDllDir() const;
    QString whisperModelPath() const;
    QString onnxDllDir() const;
    QString demucsModelPath() const;

    // ── Данные ────────────────────────────────────────────────────────────────
    QString                  m_appDir;
    SongMeta                 m_meta;
    QString                  m_cacheDir;
    const std::atomic<bool> *m_cancelled;
    KaraokeProgressCb        m_progressCb;
};
