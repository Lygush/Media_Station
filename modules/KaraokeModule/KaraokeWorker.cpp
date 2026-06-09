// modules/KaraokeModule/KaraokeWorker.cpp
#include "KaraokeWorker.h"

#include <QProcess>
#include <QFile>
#include <QDir>
#include <QDebug>

#include "DemucsWrapper.h"
#include "WhisperWrapper.h"
#include "LrcFetcher.h"
#include "LrcBuilder.h"
#include "../../tools/AtomicFile.h"

// ─────────────────────────────────────────────────────────────────────────────

KaraokeWorker::KaraokeWorker(const QString          &appDir,
                               const SongMeta         &meta,
                               const QString          &cacheDir,
                               const std::atomic<bool> *cancelled)
    : m_appDir(appDir)
    , m_meta(meta)
    , m_cacheDir(cacheDir)
    , m_cancelled(cancelled)
{
}

// ─────────────────────────────────────────────────────────────────────────────
// run
// ─────────────────────────────────────────────────────────────────────────────

void KaraokeWorker::run()
{
    qDebug() << "[KaraokeWorker] старт для:" << m_meta.filePath;

    const QString tempWav    = m_cacheDir + "/temp_input.wav";
    const QString vocalsWav  = m_cacheDir + "/vocals.wav";
    const QString instrWav   = m_cacheDir + "/instrumental.wav";
    const QString vocals16k  = m_cacheDir + "/vocals_16k.wav";
    const QString lrcOut     = m_cacheDir + "/song.lrc";

    // ── Шаг 1: ffmpeg → temp WAV 44100 stereo ────────────────────────────────
    reportProgress(0, "Подготовка аудио...");
    if (!stepFfmpegToWav(m_meta.filePath, tempWav)) return;
    if (isCancelled()) return;

    // ── Шаг 2: Demucs → vocals.wav + instrumental.wav ─────────────────────────
    reportProgress(10, "Разделение вокала...");
    if (!stepDemucs(tempWav, vocalsWav, instrWav)) return;
    if (isCancelled()) return;

    // Temp WAV больше не нужен
    QFile::remove(tempWav);

    // ── Шаг 3: LRC (lrclib.net или Whisper) ───────────────────────────────────
    reportProgress(65, "Получение текста...");
    if (!stepLrc(vocalsWav, lrcOut)) return;

    // vocals_16k.wav удаляем если остался
    QFile::remove(vocals16k);

    reportProgress(100, "Готово");
    qDebug() << "[KaraokeWorker] завершён успешно";
}

// ─────────────────────────────────────────────────────────────────────────────
// Шаг 1 — ffmpeg → WAV 44100 Hz stereo 16-bit
// ─────────────────────────────────────────────────────────────────────────────

bool KaraokeWorker::stepFfmpegToWav(const QString &inputPath, const QString &outputPath)
{
    const QString ffmpeg = ffmpegPath();
    if (!QFile::exists(ffmpeg)) {
        qCritical() << "[KaraokeWorker] ffmpeg не найден:" << ffmpeg;
        return false;
    }

    QProcess proc;
    proc.setProgram(ffmpeg);
    proc.setArguments({
        "-y",
        "-i", inputPath,
        "-ar", "44100",
        "-ac", "2",
        "-sample_fmt", "s16",
        outputPath
    });
    proc.start();
    if (!proc.waitForFinished(120'000)) {
        qCritical() << "[KaraokeWorker] ffmpeg timeout (44100)";
        proc.kill();
        return false;
    }
    if (proc.exitCode() != 0) {
        qCritical() << "[KaraokeWorker] ffmpeg ошибка:" << proc.readAllStandardError();
        return false;
    }
    qDebug() << "[KaraokeWorker] ffmpeg → WAV готов:" << outputPath;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Шаг 2 — Demucs
// ─────────────────────────────────────────────────────────────────────────────

bool KaraokeWorker::stepDemucs(const QString &inputWav,
                                 const QString &vocalsOut,
                                 const QString &instrOut)
{
    DemucsWrapper demucs;
    if (!demucs.load(onnxDllDir(), demucsModelPath())) {
        qCritical() << "[KaraokeWorker] Demucs load failed:" << demucs.lastError();
        return false;
    }

    const bool ok = demucs.separate(
        inputWav, vocalsOut, instrOut,
        [this](int pct) {
            // Demucs занимает 10–65% общего прогресса
            reportProgress(10 + pct * 55 / 100, "Разделение вокала...");
        },
        m_cancelled
    );

    if (!ok) {
        qCritical() << "[KaraokeWorker] Demucs separate failed:" << demucs.lastError();
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Шаг 3 — LRC: lrclib.net → Whisper fallback
// ─────────────────────────────────────────────────────────────────────────────

bool KaraokeWorker::stepLrc(const QString &vocalsWav, const QString &lrcOut)
{
    // ── Пробуем lrclib.net ────────────────────────────────────────────────────
    if (m_meta.hasEnoughMeta()) {
        reportProgress(66, "Поиск текста онлайн...");
        LrcFetcher fetcher;
        const LrcResult r = fetcher.fetch(m_meta.artist, m_meta.title,
                                           m_meta.album, m_meta.durationSec,
                                           m_cancelled);
        if (isCancelled()) return false;

        if (r.hasSynced) {
            qDebug() << "[KaraokeWorker] lrclib: syncedLyrics получен";
            AtomicFile::writeBytes(lrcOut, r.lrc.toUtf8());
            return true;
        }

        if (r.hasPlain) {
            // Есть текст но без синхронизации — запустим Whisper для тайм-кодов
            qDebug() << "[KaraokeWorker] lrclib: только plainLyrics, fallback на Whisper";
        } else if (r.networkError) {
            qDebug() << "[KaraokeWorker] lrclib: нет сети, fallback на Whisper";
        } else {
            qDebug() << "[KaraokeWorker] lrclib: трек не найден, fallback на Whisper";
        }
    } else {
        qDebug() << "[KaraokeWorker] нет метаданных, сразу Whisper";
    }

    // ── Fallback: Whisper ─────────────────────────────────────────────────────
    reportProgress(68, "Подготовка аудио для распознавания...");

    const QString vocals16k = m_cacheDir + "/vocals_16k.wav";
    if (!stepFfmpegTo16k(vocalsWav, vocals16k)) return false;
    if (isCancelled()) return false;

    reportProgress(70, "Распознавание текста...");

    WhisperWrapper whisper;
    if (!whisper.load(whisperDllDir(), whisperModelPath())) {
        qCritical() << "[KaraokeWorker] Whisper load failed:" << whisper.lastError();
        return false;
    }

    const TranscriptResult tr = whisper.transcribe(vocals16k,
        [this](int pct) -> bool {
            // Whisper занимает 70–97% общего прогресса
            reportProgress(70 + pct * 27 / 100, "Распознавание текста...");
            return !isCancelled();
        }
    );

    if (!tr.success) {
        qCritical() << "[KaraokeWorker] Whisper failed:" << tr.error;
        return false;
    }
    if (isCancelled()) return false;

    reportProgress(97, "Сборка субтитров...");
    const QString lrc = LrcBuilder::build(tr.words);
    AtomicFile::writeBytes(lrcOut, lrc.toUtf8());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Шаг 4 — ffmpeg → 16kHz mono для Whisper
// ─────────────────────────────────────────────────────────────────────────────

bool KaraokeWorker::stepFfmpegTo16k(const QString &inputWav, const QString &outputWav)
{
    QProcess proc;
    proc.setProgram(ffmpegPath());
    proc.setArguments({
        "-y",
        "-i", inputWav,
        "-ar", "16000",
        "-ac", "1",
        "-c:a", "pcm_s16le",
        outputWav
    });
    proc.start();
    if (!proc.waitForFinished(60'000)) {
        qCritical() << "[KaraokeWorker] ffmpeg timeout (16k)";
        proc.kill();
        return false;
    }
    if (proc.exitCode() != 0) {
        qCritical() << "[KaraokeWorker] ffmpeg 16k ошибка:" << proc.readAllStandardError();
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void KaraokeWorker::reportProgress(int percent, const QString &stage)
{
    if (m_progressCb)
        m_progressCb(percent, stage);
}

bool KaraokeWorker::isCancelled() const
{
    return m_cancelled && m_cancelled->load();
}

QString KaraokeWorker::ffmpegPath() const
{
    return m_appDir + "/tools/ffmpeg/ffmpeg.exe";
}

QString KaraokeWorker::whisperDllDir() const
{
    return m_appDir + "/tools/whisper";
}

QString KaraokeWorker::whisperModelPath() const
{
    return m_appDir + "/tools/whisper/models/ggml-large-v3-turbo.bin";
}

QString KaraokeWorker::onnxDllDir() const
{
    return m_appDir + "/tools/onnxruntime";
}

QString KaraokeWorker::demucsModelPath() const
{
    return m_appDir + "/models/htdemucs.onnx";
}
