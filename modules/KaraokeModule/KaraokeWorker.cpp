// modules/KaraokeModule/KaraokeWorker.cpp
#include "KaraokeWorker.h"

#include <QProcess>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <vector>

#include "DemucsWrapper.h"
#include "WhisperWrapper.h"
#include "LrcFetcher.h"
#include "LrcBuilder.h"
#include "LyricsAligner.h"
#include "../../tools/AtomicFile.h"


// ffmpeg с возможностью отмены: раньше waitForFinished(120 с) не реагировал на
// «Отмена», и до двух минут обработка продолжалась.
static bool runCancellable(QProcess &proc, int timeoutMs, const std::atomic<bool> *cancelled,
                           const char *what)
{
    proc.start();
    if (!proc.waitForStarted(10'000)) {
        qCritical() << "[KaraokeWorker]" << what << "не запустился:" << proc.errorString();
        return false;
    }
    QElapsedTimer t;
    t.start();
    while (!proc.waitForFinished(200)) {
        if (cancelled && cancelled->load()) {
            proc.kill();
            proc.waitForFinished(3000);
            return false;
        }
        if (t.elapsed() > timeoutMs) {
            qCritical() << "[KaraokeWorker]" << what << "timeout";
            proc.kill();
            proc.waitForFinished(3000);
            return false;
        }
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        qCritical() << "[KaraokeWorker]" << what << "ошибка:" << proc.readAllStandardError().right(2000);
        return false;
    }
    return true;
}


// ─────────────────────────────────────────────────────────────────────────────
// WAV-помощники (16-бит PCM, как пишут ffmpeg и DemucsWrapper)
// ─────────────────────────────────────────────────────────────────────────────

struct WavInfo { int sampleRate = 0; int channels = 0; qint64 dataOffset = -1; qint64 dataBytes = 0; };

static WavInfo readWavInfo(QFile &f)
{
    WavInfo wi;
    const QByteArray hdr = f.read(12);
    if (hdr.size() < 12 || !hdr.startsWith("RIFF") || hdr.mid(8, 4) != "WAVE") return wi;
    while (!f.atEnd()) {
        const QByteArray ch = f.read(8);
        if (ch.size() < 8) break;
        const quint32 sz = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(ch.constData() + 4));
        if (ch.startsWith("fmt ")) {
            const QByteArray fmt = f.read(sz);
            if (fmt.size() >= 16) {
                wi.channels   = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(fmt.constData() + 2));
                wi.sampleRate = int(qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(fmt.constData() + 4)));
            }
            if (sz & 1) f.read(1);
        } else if (ch.startsWith("data")) {
            wi.dataOffset = f.pos();
            // ffmpeg при записи в поток может оставить размер 0xFFFFFFFF
            wi.dataBytes  = (sz == 0xFFFFFFFFu || sz == 0) ? f.size() - wi.dataOffset : qint64(sz);
            break;
        } else {
            f.seek(f.pos() + sz + (sz & 1));
        }
    }
    return wi;
}

static int wavDurationSec(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const WavInfo wi = readWavInfo(f);
    if (wi.dataOffset < 0 || wi.sampleRate <= 0 || wi.channels <= 0) return 0;
    return int(wi.dataBytes / (qint64(wi.sampleRate) * wi.channels * 2));
}

// RMS по кадрам 20 мс (каналы сводятся в моно)
static std::vector<float> wavFrameRms(const QString &path)
{
    std::vector<float> rms;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return rms;
    const WavInfo wi = readWavInfo(f);
    if (wi.dataOffset < 0 || wi.sampleRate <= 0 || wi.channels <= 0) return rms;
    const int frameSamples = wi.sampleRate / 50;
    const qint64 frameBytes = qint64(frameSamples) * wi.channels * 2;
    f.seek(wi.dataOffset);
    qint64 left = wi.dataBytes;
    while (left >= frameBytes) {
        const QByteArray buf = f.read(frameBytes);
        if (buf.size() < frameBytes) break;
        left -= frameBytes;
        const qint16 *p = reinterpret_cast<const qint16 *>(buf.constData());
        double acc = 0.0;
        for (int i = 0; i < frameSamples; ++i) {
            double v = 0.0;
            for (int c = 0; c < wi.channels; ++c) v += p[i * wi.channels + c];
            v /= wi.channels * 32768.0;
            acc += v * v;
        }
        rms.push_back(float(std::sqrt(acc / frameSamples)));
    }
    return rms;
}

// ─────────────────────────────────────────────────────────────────────────────
// Проверка синхронизации текста lrclib с вокалом.
//
// Тайм-коды lrclib сделаны людьми под конкретную запись. Если у нас другая
// версия (тишина в начале, другое издание), весь текст сдвинут на секунды.
// Ищем сдвиг, при котором начала строк лучше всего совпадают с началами
// пения на дорожке вокала (после Demucs): после метки голос есть, перед — нет.
// Возвращает сдвиг в мс (+ — текст надо показывать позже) или 0.
// ─────────────────────────────────────────────────────────────────────────────
static int estimateLrcOffsetMs(const QString &lrc, const std::vector<float> &frameRms)
{
    if (frameRms.size() < 500) return 0;                     // < 10 с звука

    static const QRegularExpression lineRe(QStringLiteral("^\\[(\\d+):(\\d+)\\.(\\d+)\\](.*)$"));
    std::vector<int> starts;   // мс
    for (const QString &raw : lrc.split(QLatin1Char('\n'))) {
        const auto m = lineRe.match(raw.trimmed());
        if (!m.hasMatch()) continue;
        if (m.captured(4).trimmed().isEmpty()) continue;      // пустая строка = проигрыш
        const QString cs = m.captured(3);
        int frac = cs.toInt();
        if (cs.length() == 2) frac *= 10; else if (cs.length() == 1) frac *= 100;
        starts.push_back(m.captured(1).toInt() * 60000 + m.captured(2).toInt() * 1000 + frac);
    }
    if (starts.size() < 6) return 0;

    std::vector<float> sorted = frameRms;
    std::sort(sorted.begin(), sorted.end());
    const float thr = std::max(0.004f, 0.10f * sorted[sorted.size() * 9 / 10]);
    const int nF = int(frameRms.size());
    // Префиксные суммы активных кадров — быстрые запросы «доля голоса в окне»
    std::vector<int> pre(nF + 1, 0);
    for (int i = 0; i < nF; ++i) pre[i + 1] = pre[i] + (frameRms[i] > thr ? 1 : 0);
    auto activeFrac = [&](int fromMs, int toMs) -> double {
        int a = std::clamp(fromMs / 20, 0, nF), b = std::clamp(toMs / 20, 0, nF);
        return b > a ? double(pre[b] - pre[a]) / (b - a) : 0.0;
    };
    auto score = [&](int off) {
        double s = 0.0;
        for (int t : starts) {
            const int at = t + off;
            s += activeFrac(at, at + 400) - activeFrac(at - 400, at - 40);
        }
        return s / double(starts.size());
    };

    const double base = score(0);
    int    bestOff   = 0;
    double bestScore = base;
    for (int off = -5000; off <= 5000; off += 40) {
        const double sc = score(off);
        if (sc > bestScore + 1e-9) { bestScore = sc; bestOff = off; }
    }
    qDebug() << "[KaraokeWorker] синхронизация lrclib: лучший сдвиг" << bestOff
             << "мс, оценка" << bestScore << "против" << base << "без сдвига";
    // Сдвигаем, только если улучшение заметное, а сдвиг не мелкий
    if (std::abs(bestOff) >= 250 && bestScore - base >= 0.12)
        return bestOff;
    return 0;
}

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
    if (!stepFfmpegToWav(m_meta.filePath, tempWav) || isCancelled()) {
        QFile::remove(tempWav);
        return;
    }

    // Длительность для выбора нужной версии текста на lrclib. Теги
    // (TagLib) в сборке выключены, и раньше она всегда была 0.
    if (m_meta.durationSec <= 0) {
        m_meta.durationSec = wavDurationSec(tempWav);
        qDebug() << "[KaraokeWorker] длительность трека:" << m_meta.durationSec << "с";
    }

    // ── Шаг 2: Demucs → vocals.wav + instrumental.wav ─────────────────────────
    reportProgress(10, "Разделение вокала...");
    if (!stepDemucs(tempWav, vocalsWav, instrWav) || isCancelled()) {
        // Недописанные дорожки не должны выглядеть как готовый кэш
        QFile::remove(tempWav);
        QFile::remove(vocalsWav);
        QFile::remove(instrWav);
        return;
    }

    // Temp WAV больше не нужен
    QFile::remove(tempWav);

    // ── Шаг 3: LRC (lrclib.net или Whisper) ───────────────────────────────────
    reportProgress(65, "Получение текста...");
    if (!stepLrc(vocalsWav, lrcOut)) {
        QFile::remove(vocals16k);   // временный файл распознавания не нужен
        return;
    }

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
    // Длинные треки на медленном диске не укладывались в 2 минуты
    if (!runCancellable(proc, 600'000, m_cancelled, "ffmpeg (44100)")) {
        QFile::remove(outputPath);
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
    QString plainLyrics;   // текст без времени (lrclib) — выровняем по Whisper

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
            QString lrc = r.lrc;
            reportProgress(67, "Проверка синхронизации...");
            const int offMs = estimateLrcOffsetMs(lrc, wavFrameRms(vocalsWav));
            if (offMs != 0) {
                // [offset:] по стандарту LRC: «+» — показывать раньше, поэтому
                // знак обратный найденному сдвигу
                lrc.prepend(QString("[offset:%1]\n").arg(-offMs));
                qDebug() << "[KaraokeWorker] текст сдвинут на" << offMs << "мс";
            }
            AtomicFile::writeBytes(lrcOut, lrc.toUtf8());
            return true;
        }

        if (r.hasPlain) {
            // Есть текст но без синхронизации: Whisper даст время слов,
            // а текст и строки останутся эталонными (LyricsAligner)
            plainLyrics = r.plainLyrics;
            qDebug() << "[KaraokeWorker] lrclib: только plainLyrics — Whisper + выравнивание";
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

    // Словарь песни как подсказка декодеру (если текст известен)
    const QString prompt = plainLyrics.isEmpty()
                               ? QString()
                               : LyricsAligner::promptVocabulary(plainLyrics);

    const TranscriptResult tr = whisper.transcribe(vocals16k,
        [this](int pct) -> bool {
            // Whisper занимает 70–97% общего прогресса
            reportProgress(70 + pct * 27 / 100, "Распознавание текста...");
            return !isCancelled();
        },
        prompt
    );

    if (!tr.success) {
        qCritical() << "[KaraokeWorker] Whisper failed:" << tr.error;
        return false;
    }
    if (isCancelled()) return false;

    reportProgress(97, "Сборка субтитров...");

    // Известный текст: правильные слова и строки + время от Whisper
    if (!plainLyrics.isEmpty()) {
        const LyricsAligner::Result al = LyricsAligner::align(plainLyrics, tr.words);
        qDebug() << "[KaraokeWorker] выравнивание: совпало" << al.matchedWords
                 << "из" << al.refWords << "слов (" << int(al.matchedRatio * 100) << "% )";
        if (al.matchedRatio >= 0.25) {
            AtomicFile::writeBytes(lrcOut, LrcBuilder::buildFromLines(al.lines).toUtf8());
            return true;
        }
        qWarning() << "[KaraokeWorker] совпадений слишком мало — используем распознанный текст";
    }

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
    if (!runCancellable(proc, 600'000, m_cancelled, "ffmpeg (16k)")) {
        QFile::remove(outputWav);
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
    // Полная large-v3 заметно точнее turbo на русском пении; если положить
    // ggml-large-v3.bin рядом — он будет использован автоматически.
    const QString full = m_appDir + "/tools/whisper/models/ggml-large-v3.bin";
    if (QFile::exists(full))
        return full;
    return m_appDir + "/tools/whisper/models/ggml-large-v3-turbo.bin";
}

QString KaraokeWorker::onnxDllDir() const
{
    return m_appDir + "/tools/onnxruntime";
}

QString KaraokeWorker::demucsModelPath() const
{
    // Основное место — внутри tools/ (как у Whisper и LLM): tools/ переносится
    // при обновлении. Старое место models/ — запасное, для существующих установок.
    const QString inTools = m_appDir + "/tools/onnxruntime/models/htdemucs.onnx";
    if (QFile::exists(inTools))
        return inTools;
    const QString legacy = m_appDir + "/models/htdemucs.onnx";
    if (QFile::exists(legacy))
        return legacy;
    return inTools;   // для сообщения об ошибке — путь, где модель должна лежать
}
