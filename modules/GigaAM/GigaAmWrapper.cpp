// modules/KaraokeModule/GigaAmWrapper.cpp
#include "GigaAmWrapper.h"

#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QDebug>

#include <cmath>
#include <algorithm>
#include <vector>
#include <array>
#include <memory>

extern "C" {
#include "kiss_fftr.h"   // real FFT из kiss_fft (kiss_fftr_alloc, kiss_fftr, kiss_fftr_free)
}

// ── Конструктор / деструктор ──────────────────────────────────────────────────

GigaAmWrapper::GigaAmWrapper(QObject *parent)
    : QObject(parent)
    , m_env(ORT_LOGGING_LEVEL_WARNING, "GigaAM")
    , m_memoryInfo(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    , m_fftCfg(nullptr)   // теперь объявлен в .h
{
    m_sessionOptions.SetIntraOpNumThreads(4);
    m_sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    // CUDA execution provider — пробуем GPU, fallback на CPU
    OrtCUDAProviderOptions cudaOptions{};
    cudaOptions.device_id = 0;
    try {
        m_sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
        qDebug() << "[GigaAmWrapper] CUDA EP добавлен";
    } catch (...) {
        qDebug() << "[GigaAmWrapper] CUDA недоступен, используем CPU";
    }
}

GigaAmWrapper::~GigaAmWrapper()
{
    unload();
}

// ── load ──────────────────────────────────────────────────────────────────────

bool GigaAmWrapper::load(const QString &modelPath, const QString &vocabPath)
{
    if (m_session) {
        qWarning() << "[GigaAmWrapper] already loaded";
        return true;
    }

    if (!QFile::exists(modelPath)) {
        m_lastError = "Модель не найдена: " + modelPath;
        qCritical() << "[GigaAmWrapper]" << m_lastError;
        return false;
    }

    if (!loadVocab(vocabPath)) return false;

    try {
        const std::wstring wPath = modelPath.toStdWString();
        m_session = std::make_unique<Ort::Session>(m_env, wPath.c_str(), m_sessionOptions);
    } catch (const Ort::Exception &e) {
        m_lastError = QString("Ошибка загрузки модели: %1").arg(e.what());
        qCritical() << "[GigaAmWrapper]" << m_lastError;
        return false;
    }

    qDebug() << "[GigaAmWrapper] Модель загружена:" << modelPath;
    qDebug() << "[GigaAmWrapper] Словарь:" << m_vocab.size() << "токенов, blank id =" << m_blankId;
    return true;
}

// ── unload ────────────────────────────────────────────────────────────────────

void GigaAmWrapper::unload()
{
    m_session.reset();
    m_vocab.clear();
    m_blankId = -1;
    if (m_fftCfg) {
        kiss_fftr_free(m_fftCfg);
        m_fftCfg = nullptr;
    }
}

// ── loadVocab ────────────────────────────────────────────────────────────────

bool GigaAmWrapper::loadVocab(const QString &vocabPath)
{
    QFile f(vocabPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = "Словарь не найден: " + vocabPath;
        qCritical() << "[GigaAmWrapper]" << m_lastError;
        return false;
    }

    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);

    m_vocab.clear();
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        // Формат: "токен индекс"
        const int spaceIdx = line.lastIndexOf(' ');
        if (spaceIdx < 0) continue;

        const QString token = line.left(spaceIdx);
        const int     idx   = line.mid(spaceIdx + 1).toInt();

        // Расширяем вектор если нужно
        while (m_vocab.size() <= idx)
            m_vocab.append("");

        // ▁ (U+2581) — sentencepiece пробел, заменяем на обычный
        QString t = token;
        t.replace(QChar(0x2581), ' ');

        // <blk> — blank токен CTC
        if (token == "<blk>") {
            m_blankId = idx;
            t = "";
        }

        m_vocab[idx] = t;
    }

    if (m_blankId < 0) {
        // Если blank не найден явно — последний токен обычно blank
        m_blankId = m_vocab.size() - 1;
        qWarning() << "[GigaAmWrapper] blank не найден в словаре, используем idx =" << m_blankId;
    }

    return true;
}

// ── loadWav ───────────────────────────────────────────────────────────────────

std::vector<float> GigaAmWrapper::loadWav(const QString &path, QString &error) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        error = "Не удалось открыть WAV: " + path;
        return {};
    }
    QByteArray data = f.readAll();
    f.close();

    // Ищем data chunk
    const uint8_t *p   = reinterpret_cast<const uint8_t *>(data.constData());
    const int      len = data.size();

    if (len < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) {
        error = "Некорректный WAV файл";
        return {};
    }

    int dataOffset = -1, dataSize = -1;
    int pos = 12;
    while (pos + 8 <= len) {
        const char    *id = reinterpret_cast<const char *>(p + pos);
        const uint32_t sz = *reinterpret_cast<const uint32_t *>(p + pos + 4);
        if (memcmp(id, "data", 4) == 0) { dataOffset = pos + 8; dataSize = (int)sz; break; }
        pos += 8 + (int)sz;
        if (sz & 1) ++pos;
    }

    if (dataOffset < 0 || dataOffset + dataSize > len) {
        error = "data chunk не найден";
        return {};
    }

    const int16_t *pcm16   = reinterpret_cast<const int16_t *>(p + dataOffset);
    const int      nSamples = dataSize / (int)sizeof(int16_t);

    std::vector<float> out(nSamples);
    for (int i = 0; i < nSamples; ++i)
        out[i] = pcm16[i] / 32768.0f;

    return out;
}

// ── Инициализация kiss_fft (real FFT) ────────────────────────────────────────

void GigaAmWrapper::ensureFftCfg() const
{
    if (m_fftCfg == nullptr) {
        // Создаём план real FFT размера kNFft (400)
        auto *cfg = kiss_fftr_alloc(kNFft, 0, nullptr, nullptr);
        if (!cfg) {
            qFatal("[GigaAmWrapper] Не удалось создать kiss_fftr план");
        }
        // Изменяем mutable член (FFT план создаётся один раз и шарится)
        m_fftCfg = cfg;
    }
}

// ── computeMelSpectrogram (с kiss_fft) ────────────────────────────────────────

std::vector<float> GigaAmWrapper::computeMelSpectrogram(const float *samples,
                                                        int nSamples,
                                                        int *outFrames) const
{
    ensureFftCfg();

    const int nFrames = nSamples / kHopLength + 1;
    *outFrames = nFrames;

    // Hann окно
    std::vector<float> window(kNFft);
    for (int i = 0; i < kNFft; ++i)
        window[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (kNFft - 1)));

    // Мел-фильтры
    const float fMin = 0.0f;
    const float fMax = kSampleRate / 2.0f;
    const int nBins = kNFft / 2 + 1;

    auto hzToMel = [](float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); };
    auto melToHz = [](float mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); };

    const float melMin = hzToMel(fMin);
    const float melMax = hzToMel(fMax);
    std::vector<float> melPoints(kNMels + 2);
    for (int i = 0; i < kNMels + 2; ++i)
        melPoints[i] = melToHz(melMin + (melMax - melMin) * i / (kNMels + 1));

    std::vector<int> binPoints(kNMels + 2);
    for (int i = 0; i < kNMels + 2; ++i)
        binPoints[i] = (int)std::floor(melPoints[i] * kNFft / kSampleRate);

    std::vector<float> filterBank(kNMels * nBins, 0.0f);
    for (int m = 0; m < kNMels; ++m) {
        const int left = binPoints[m], center = binPoints[m + 1], right = binPoints[m + 2];
        for (int k = left; k < center; ++k)
            if (k < nBins) filterBank[m * nBins + k] = (float)(k - left) / (center - left);
        for (int k = center; k < right; ++k)
            if (k < nBins) filterBank[m * nBins + k] = (float)(right - k) / (right - center);
    }

    std::vector<float> melSpec(kNMels * nFrames, 0.0f);
    std::vector<float> frame(kNFft, 0.0f);
    std::vector<kiss_fft_cpx> fftOut(nBins);

    for (int t = 0; t < nFrames; ++t) {
        const int center = t * kHopLength;
        for (int i = 0; i < kNFft; ++i) {
            const int idx = center - kNFft / 2 + i;
            frame[i] = (idx >= 0 && idx < nSamples) ? samples[idx] * window[i] : 0.0f;
        }

        kiss_fftr(m_fftCfg, frame.data(), fftOut.data());

        std::vector<float> mag(nBins);
        for (int k = 0; k < nBins; ++k) {
            const float re = fftOut[k].r, im = fftOut[k].i;
            mag[k] = re * re + im * im;
        }

        for (int m = 0; m < kNMels; ++m) {
            float val = 0.0f;
            for (int k = 0; k < nBins; ++k)
                val += filterBank[m * nBins + k] * mag[k];
            melSpec[m * nFrames + t] = std::log(std::max(val, 1e-6f));
        }
    }

    // ── Глобальная нормализация (как в оригинальном SpecScaler) ──
    size_t total = kNMels * nFrames;
    float sum = 0.0f, sum2 = 0.0f;
    for (size_t i = 0; i < total; ++i) {
        sum += melSpec[i];
        sum2 += melSpec[i] * melSpec[i];
    }
    float mean = sum / total;
    float var = sum2 / total - mean * mean;
    float stddev = std::sqrt(std::max(var, 1e-8f));
    if (stddev < 1e-8f) stddev = 1.0f;   // на случай абсолютной тишины
    for (size_t i = 0; i < total; ++i)
        melSpec[i] = (melSpec[i] - mean) / stddev;

    return melSpec;
}

// ── runInference ──────────────────────────────────────────────────────────────

std::vector<float> GigaAmWrapper::runInference(const std::vector<float> &melSpec,
                                               int nFrames)
{
    if (!m_session) return {};

    // Вход: features [1, 64, seq_len]
    std::array<int64_t, 3> featShape = {1, kNMels, nFrames};
    Ort::Value featTensor = Ort::Value::CreateTensor<float>(
        m_memoryInfo,
        const_cast<float *>(melSpec.data()),
        melSpec.size(),
        featShape.data(), featShape.size()
        );

    // Вход: feature_lengths [1]
    int64_t featLen = nFrames;
    std::array<int64_t, 1> lenShape = {1};
    Ort::Value lenTensor = Ort::Value::CreateTensor<int64_t>(
        m_memoryInfo, &featLen, 1, lenShape.data(), lenShape.size()
        );

    const char *inputNames[]  = {"features", "feature_lengths"};
    const char *outputNames[] = {"log_probs"};

    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(featTensor));
    inputs.push_back(std::move(lenTensor));

    try {
        auto outputs = m_session->Run(
            Ort::RunOptions{nullptr},
            inputNames, inputs.data(), 2,
            outputNames, 1
            );

        // log_probs: [1, seq_len, vocab_size]
        const float *data = outputs[0].GetTensorData<float>();
        auto shape        = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const int seqLen  = (int)shape[1];
        const int vocabSz = (int)shape[2];

        return std::vector<float>(data, data + seqLen * vocabSz);

    } catch (const Ort::Exception &e) {
        qCritical() << "[GigaAmWrapper] Инференс ошибка:" << e.what();
        return {};
    }
}

// ── ctcDecode (с учётом субдискретизации) ─────────────────────────────────────
//
// Для CTC важны три вещи:
//  1. t0_ms — фрейм первого не-blank символа слова (начало звука)
//  2. t1_ms — фрейм последнего не-blank символа слова (конец звука, НЕ начало следующего)
//  3. В пении между словами могут быть длинные blank-серии; prevToken сбрасывается
//     на blank чтобы одинаковые токены на разных слогах не подавлялись.
//
// Sentencepiece токены: "▁слово" = пробел+буквы в одном токене.
// Чистый " " как отдельный токен тоже возможен.

QVector<GigaAmWord> GigaAmWrapper::ctcDecode(const std::vector<float> &logProbs,
                                             int nFrames,
                                             qint64 offsetMs,
                                             double subsampleFactor) const
{
    const int vocabSz = m_vocab.size();
    QVector<GigaAmWord> words;

    // Переводим выходной фрейм модели во время аудио.
    // subsampleFactor = (мел-фреймов) / (выходных фреймов) — обычно 4 для GigaAM v3.
    // kHopLength сэмплов на мел-фрейм, kSampleRate сэмплов/сек.
    auto frameToMs = [&](int frame) -> qint64 {
        return offsetMs + (qint64)std::round(frame * subsampleFactor * kHopLength * 1000.0 / kSampleRate);
    };

    QString currentWord;
    int wordStartFrame = -1;  // фрейм первого символа текущего слова
    int wordLastFrame  = -1;  // фрейм последнего символа текущего слова (для t1)
    int prevToken      = -1;  // для CTC collapse; -1 = сброшен (после blank)

    auto flushWord = [&](int endFrame) {
        if (currentWord.isEmpty() || wordStartFrame < 0) return;
        GigaAmWord w;
        w.text  = currentWord.trimmed();
        w.t0_ms = frameToMs(wordStartFrame);
        // t1 = конец последнего символа слова (середина следующего hop после него)
        w.t1_ms = frameToMs(wordLastFrame + 1);
        if (!w.text.isEmpty())
            words.append(w);
        currentWord.clear();
        wordStartFrame = -1;
        wordLastFrame  = -1;
    };

    for (int t = 0; t < nFrames; ++t) {
        const float *row = logProbs.data() + t * vocabSz;

        int bestIdx = 0;
        for (int i = 1; i < vocabSz; ++i)
            if (row[i] > row[bestIdx]) bestIdx = i;

        if (bestIdx == m_blankId) {
            // Blank сбрасывает prevToken — важно для пения: один и тот же слог
            // может повторяться после паузы и не должен подавляться.
            prevToken = -1;
            continue;
        }

        if (bestIdx == prevToken) {
            // CTC collapse: повторяющийся токен без blank между ними = один символ.
            // Обновляем wordLastFrame чтобы t1 охватывал всю длительность символа.
            if (wordLastFrame < t) wordLastFrame = t;
            continue;
        }

        prevToken = bestIdx;

        const QString &tok = m_vocab[bestIdx];

        // Определяем: токен начинает новое слово (содержит ведущий пробел)?
        const bool startsWord = tok.startsWith(' ');
        const QString chars   = startsWord ? tok.mid(1) : tok;  // без ведущего пробела

        if (startsWord && !currentWord.isEmpty()) {
            // Граница слова — сбрасываем предыдущее
            flushWord(t);
        }

        if (chars.isEmpty()) {
            // Чистый пробел-токен без букв — только граница, символов не добавляем
            continue;
        }

        if (wordStartFrame < 0)
            wordStartFrame = t;

        currentWord  += chars;
        wordLastFrame = t;
    }

    // Последнее слово в чанке
    flushWord(nFrames);

    return words;
}

// ── transcribe ────────────────────────────────────────────────────────────────

GigaAmResult GigaAmWrapper::transcribe(const QString &wav16kPath,
                                       GigaAmProgressCb progressCb)
{
    GigaAmResult result;

    if (!m_session) {
        result.error = "GigaAmWrapper не инициализирован";
        return result;
    }

    QString loadError;
    std::vector<float> samples = loadWav(wav16kPath, loadError);
    if (samples.empty()) {
        result.error = loadError;
        return result;
    }

    qDebug() << "[GigaAmWrapper] Аудио:" << samples.size() << "сэмплов ("
             << (samples.size() / 16000.0f) << "сек)";

    const int chunkSamples   = kChunkMs   * kSampleRate / 1000;
    const int overlapSamples = kOverlapMs * kSampleRate / 1000;
    const int stepSamples    = chunkSamples - overlapSamples;

    const int totalSamples = (int)samples.size();
    int       processed    = 0;

    qDebug() << "[GigaAmWrapper] chunkSamples=" << chunkSamples << "total=" << totalSamples;

    while (processed < totalSamples) {
        const int start = processed;
        const int end   = std::min(start + chunkSamples, totalSamples);
        const int count = end - start;

        qDebug() << "[GigaAmWrapper] Чанк start=" << start << "end=" << end << "count=" << count;

        if (progressCb) {
            const int pct = (int)(100LL * processed / totalSamples);
            if (!progressCb(pct)) {
                result.error = "Отменено";
                return result;
            }
        }

        int nFrames = 0;
        qDebug() << "[GigaAmWrapper] Вычисляем мел-спектрограмму...";
        std::vector<float> mel = computeMelSpectrogram(
            samples.data() + start, count, &nFrames);
        qDebug() << "[GigaAmWrapper] Мел готов, nFrames=" << nFrames;

        if (mel.empty() || nFrames == 0) {
            processed += stepSamples;
            continue;
        }

        // ⚠️ Per-feature нормализация убрана

        qDebug() << "[GigaAmWrapper] Запускаем инференс...";
        std::vector<float> logProbs = runInference(mel, nFrames);
        qDebug() << "[GigaAmWrapper] Инференс завершён, logProbs size=" << logProbs.size();
        if (logProbs.empty()) {
            result.error = "Ошибка инференса";
            return result;
        }

        const int outFrames = static_cast<int>(logProbs.size()) / m_vocab.size();
        const double subsampleFactor = (nFrames > 0) ? static_cast<double>(nFrames) / outFrames : 1.0;

        const qint64 offsetMs = (qint64)start * 1000 / kSampleRate;
        QVector<GigaAmWord> chunkWords = ctcDecode(logProbs, outFrames, offsetMs, subsampleFactor);
        qDebug() << "[GigaAmWrapper] Декодировано слов в чанке:" << chunkWords.size()
                 << "overlapStartMs=" << (qint64)(end - overlapSamples) * 1000 / kSampleRate;

        if (end < totalSamples) {
            const qint64 overlapStartMs = (qint64)(end - overlapSamples) * 1000 / kSampleRate;
            for (const auto &w : chunkWords)
                if (w.t1_ms <= overlapStartMs)
                    result.words.append(w);
        } else {
            // Последний чанк: убираем дубли с предыдущим чанком по времени начала
            const qint64 lastWordMs = result.words.isEmpty()
                                          ? -1                          // <-- теперь 0 будет проходить
                                          : result.words.last().t0_ms;
            for (const auto &w : chunkWords)
                if (w.t0_ms > lastWordMs)            // при lastWordMs=-1 пройдут все слова
                    result.words.append(w);
        }

        processed += stepSamples;
    }

    if (progressCb) progressCb(100);

    QStringList parts;
    for (const auto &w : result.words)
        parts << w.text;
    result.fullText = parts.join(' ');

    qDebug() << "[GigaAmWrapper] Извлечено слов:" << result.words.size();
    qDebug() << "[GigaAmWrapper] Текст:" << result.fullText.left(100);

    result.success = true;
    return result;
}
