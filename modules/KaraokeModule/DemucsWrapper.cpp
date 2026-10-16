// modules/KaraokeModule/DemucsWrapper.cpp
#include "DemucsWrapper.h"

#include <QFile>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

DemucsWrapper::DemucsWrapper() = default;

DemucsWrapper::~DemucsWrapper()
{
    delete m_session;
    m_session = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// load
// ─────────────────────────────────────────────────────────────────────────────

bool DemucsWrapper::load(const QString &/*onnxDllDir*/, const QString &modelPath)
{
    if (m_session) { qWarning() << "[DemucsWrapper] уже загружен"; return true; }

    if (!QFile::exists(modelPath)) {
        m_lastError = "Модель не найдена: " + modelPath;
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }

    try {
        m_sessionOptions.SetIntraOpNumThreads(4);
        m_sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

        m_session = new Ort::Session(m_env,
            modelPath.toStdWString().c_str(),
            m_sessionOptions);

        // Имена всех выходов — запрашиваем их все (временная + частотная ветки)
        Ort::AllocatorWithDefaultOptions alloc;
        m_outputNames.clear();
        const size_t nOut = m_session->GetOutputCount();
        for (size_t i = 0; i < nOut; ++i)
            m_outputNames.emplace_back(m_session->GetOutputNameAllocated(i, alloc).get());

        qDebug() << "[DemucsWrapper] модель загружена:" << modelPath
                 << "| выходов:" << nOut;
        return true;

    } catch (const Ort::Exception &e) {
        m_lastError = QString("ONNX ошибка: %1").arg(e.what());
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// separate — как demucs.apply_model: нормализация трека, чанки с перекрытием,
// треугольные веса (transition_power=1), обратная денормализация.
// ─────────────────────────────────────────────────────────────────────────────

bool DemucsWrapper::separate(const QString          &inputWavPath,
                              const QString          &vocalsOutPath,
                              const QString          &instrOutPath,
                              DemucsProgressCb        progressCb,
                              const std::atomic<bool> *cancelled)
{
    if (!m_session) { m_lastError = "Модель не загружена"; return false; }

    QVector<float> leftIn, rightIn;
    int sampleRate = 0;
    if (!readWav(inputWavPath, leftIn, rightIn, sampleRate)) return false;

    if (sampleRate != 44100) {
        m_lastError = QString("Ожидается 44100 Hz, получено %1").arg(sampleRate);
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }

    const int total = leftIn.size();
    if (total <= 0) { m_lastError = "Пустой входной WAV"; return false; }

    // ── Нормализация по моно-миксу (как ref = wav.mean(0) в demucs) ──────────
    double sum = 0.0, sumSq = 0.0;
    for (int i = 0; i < total; ++i) {
        const double m = 0.5 * (leftIn[i] + rightIn[i]);
        sum += m; sumSq += m * m;
    }
    const double refMean = sum / total;
    double refStd = std::sqrt(std::max(0.0, sumSq / total - refMean * refMean));
    if (refStd < 1e-8) refStd = 1.0;
    for (int i = 0; i < total; ++i) {
        leftIn[i]  = static_cast<float>((leftIn[i]  - refMean) / refStd);
        rightIn[i] = static_cast<float>((rightIn[i] - refMean) / refStd);
    }
    qDebug() << "[DemucsWrapper] вход:" << total << "сэмплов, mean" << refMean << "std" << refStd;

    // ── Веса перекрытия ───────────────────────────────────────────────────────
    std::vector<float> weight(kChunkSamples);
    {
        const int half = kChunkSamples / 2;
        for (int i = 0; i < kChunkSamples; ++i)
            weight[i] = (i < half) ? float(i + 1) : float(kChunkSamples - i);
        const float mx = *std::max_element(weight.begin(), weight.end());
        for (float &w : weight) w /= mx;
    }

    std::vector<float> accVocL(total, 0.f),  accVocR(total, 0.f);
    std::vector<float> accInstL(total, 0.f), accInstR(total, 0.f);
    std::vector<float> sumWeight(total, 0.f);

    const int stride      = static_cast<int>((1.0f - kOverlap) * kChunkSamples);
    const int totalChunks = (total + stride - 1) / stride;
    int       chunkIdx    = 0;

    std::vector<float> chunkL(kChunkSamples), chunkR(kChunkSamples);
    std::vector<float> vocL, vocR, instL, instR;

    for (int chunkStart = 0; chunkStart < total; chunkStart += stride, ++chunkIdx) {
        if (cancelled && cancelled->load()) { m_lastError = "Отменено"; return false; }

        const int chunkLen = std::min(kChunkSamples, total - chunkStart);

        // Паддинг нулями до kChunkSamples
        std::fill(chunkL.begin(), chunkL.end(), 0.f);
        std::fill(chunkR.begin(), chunkR.end(), 0.f);
        std::copy(leftIn.constData()  + chunkStart, leftIn.constData()  + chunkStart + chunkLen, chunkL.begin());
        std::copy(rightIn.constData() + chunkStart, rightIn.constData() + chunkStart + chunkLen, chunkR.begin());

        if (!runChunk(chunkL.data(), chunkR.data(), vocL, vocR, instL, instR)) return false;

        for (int i = 0; i < chunkLen; ++i) {
            const float w = weight[i];
            const int   o = chunkStart + i;
            accVocL[o]  += vocL[i]  * w;  accVocR[o]  += vocR[i]  * w;
            accInstL[o] += instL[i] * w;  accInstR[o] += instR[i] * w;
            sumWeight[o] += w;
        }

        if (progressCb)
            progressCb(std::min(100, (chunkIdx + 1) * 100 / totalChunks));
    }

    // ── Нормировка весов + денормализация ────────────────────────────────────
    QVector<float> outVocL(total), outVocR(total), outInstL(total), outInstR(total);
    double rmsVoc = 0.0, rmsInst = 0.0;
    for (int i = 0; i < total; ++i) {
        const float sw = sumWeight[i] > 0.f ? sumWeight[i] : 1.f;
        outVocL[i]  = static_cast<float>(accVocL[i]  / sw * refStd + refMean);
        outVocR[i]  = static_cast<float>(accVocR[i]  / sw * refStd + refMean);
        outInstL[i] = static_cast<float>(accInstL[i] / sw * refStd + refMean);
        outInstR[i] = static_cast<float>(accInstR[i] / sw * refStd + refMean);
        rmsVoc  += double(outVocL[i])  * outVocL[i];
        rmsInst += double(outInstL[i]) * outInstL[i];
    }
    qDebug() << "[DemucsWrapper] RMS вокала:" << std::sqrt(rmsVoc / total)
             << "| RMS инструментала:" << std::sqrt(rmsInst / total);

    if (!writeWav(vocalsOutPath, outVocL,  outVocR,  44100)) return false;
    if (!writeWav(instrOutPath,  outInstL, outInstR, 44100)) return false;

    qDebug() << "[DemucsWrapper] готово:" << vocalsOutPath;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// runChunk
// ─────────────────────────────────────────────────────────────────────────────

bool DemucsWrapper::runChunk(const float *left, const float *right,
                             std::vector<float> &vocL,  std::vector<float> &vocR,
                             std::vector<float> &instL, std::vector<float> &instR)
{
    try {
        auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int N = kChunkSamples;

        // ── Вход 1: waveform "input" [1, 2, N] ───────────────────────────────
        std::vector<float> waveData(2 * static_cast<size_t>(N));
        std::copy(left,  left  + N, waveData.begin());
        std::copy(right, right + N, waveData.begin() + N);

        const int64_t waveShape[] = { 1, 2, N };
        Ort::Value waveTensor = Ort::Value::CreateTensor<float>(
            memInfo, waveData.data(), waveData.size(), waveShape, 3);

        // ── Вход 2: CaC-спектрограмма "x" [1, 4, 2048, frames] ───────────────
        const int frames = DemucsDsp::numFrames(N);          // 336
        std::vector<float> specData;
        m_dsp.computeCac(left, right, N, specData);

        const int64_t specShape[] = { 1, 4, DemucsDsp::kFreqs, frames };
        Ort::Value specTensor = Ort::Value::CreateTensor<float>(
            memInfo, specData.data(), specData.size(), specShape, 4);

        // ── Инференс (запрашиваем ВСЕ выходы) ────────────────────────────────
        const char *inputNames[] = { "input", "x" };
        Ort::Value inputTensors[] = { std::move(waveTensor), std::move(specTensor) };

        std::vector<const char *> outNames;
        for (const std::string &s : m_outputNames) outNames.push_back(s.c_str());

        auto outputs = m_session->Run(Ort::RunOptions{nullptr},
                                      inputNames, inputTensors, 2,
                                      outNames.data(), outNames.size());

        // ── Классификация выходов по числу элементов ─────────────────────────
        const size_t timeElems = size_t(kStems) * 2 * N;
        const size_t freqElems = size_t(kStems) * 4 * DemucsDsp::kFreqs * frames;

        const float *timeOut = nullptr;
        const float *freqOut = nullptr;
        for (size_t i = 0; i < outputs.size(); ++i) {
            const auto   info  = outputs[i].GetTensorTypeAndShapeInfo();
            const size_t count = info.GetElementCount();
            if (!m_loggedShapes) {
                QString shape;
                for (int64_t d : info.GetShape()) shape += QString::number(d) + " ";
                qDebug() << "[DemucsWrapper] выход" << QString::fromStdString(m_outputNames[i])
                         << "форма" << shape << "элементов" << count;
            }
            if (count == timeElems)      timeOut = outputs[i].GetTensorData<float>();
            else if (count == freqElems) freqOut = outputs[i].GetTensorData<float>();
        }
        if (!timeOut && !freqOut) {
            m_lastError = "Неожиданные выходы модели (ни временная, ни частотная ветка)";
            qCritical() << "[DemucsWrapper]" << m_lastError;
            return false;
        }
        if (!m_loggedShapes) {
            if (!freqOut)
                qWarning() << "[DemucsWrapper] частотная ветка не найдена — только временная,"
                              " качество будет низким";
            if (!timeOut)
                qWarning() << "[DemucsWrapper] временная ветка не найдена";
            m_loggedShapes = true;
        }

        // ── Сборка стемов: istft(freq) + time ────────────────────────────────
        const size_t freqStem = size_t(4) * DemucsDsp::kFreqs * frames;   // один стем, 4 CaC-канала
        std::vector<float> stemL(N), stemR(N);

        auto buildStem = [&](int s, std::vector<float> &dstL, std::vector<float> &dstR, bool add) {
            std::fill(stemL.begin(), stemL.end(), 0.f);
            std::fill(stemR.begin(), stemR.end(), 0.f);
            if (freqOut)
                m_dsp.inverseCac(freqOut + s * freqStem, N, stemL.data(), stemR.data());
            if (timeOut) {
                const float *tL = timeOut + size_t(s) * 2 * N;
                const float *tR = tL + N;
                for (int i = 0; i < N; ++i) { stemL[i] += tL[i]; stemR[i] += tR[i]; }
            }
            if (!add) { dstL = stemL; dstR = stemR; return; }
            for (int i = 0; i < N; ++i) { dstL[i] += stemL[i]; dstR[i] += stemR[i]; }
        };

        buildStem(kVocalsStem, vocL, vocR, false);

        instL.assign(N, 0.f);
        instR.assign(N, 0.f);
        for (int s = 0; s < kStems; ++s)
            if (s != kVocalsStem) buildStem(s, instL, instR, true);

        return true;

    } catch (const Ort::Exception &e) {
        m_lastError = QString("ONNX inference: %1").arg(e.what());
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WAV I/O
// ─────────────────────────────────────────────────────────────────────────────

bool DemucsWrapper::readWav(const QString &path,
                             QVector<float> &left, QVector<float> &right,
                             int &sampleRate)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_lastError = "Не удалось открыть WAV: " + path;
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();

    const uint8_t *p   = reinterpret_cast<const uint8_t*>(data.constData());
    const int      len = data.size();

    if (len < 44 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) {
        m_lastError = "Некорректный WAV: " + path;
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }

    int channels = 0, bitsPerSample = 0, dataOffset = -1, dataSize = -1;
    sampleRate = 0;
    int pos = 12;
    while (pos + 8 <= len) {
        const char    *id = reinterpret_cast<const char*>(p + pos);
        const uint32_t sz = *reinterpret_cast<const uint32_t*>(p + pos + 4);
        if (memcmp(id, "fmt ", 4) == 0) {
            sampleRate    = *reinterpret_cast<const int32_t*>(p + pos + 12);
            channels      = *reinterpret_cast<const int16_t*>(p + pos + 10);
            bitsPerSample = *reinterpret_cast<const int16_t*>(p + pos + 22);
        } else if (memcmp(id, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize   = static_cast<int>(sz);
            break;
        }
        pos += 8 + static_cast<int>(sz);
        if (sz & 1) ++pos;
    }

    if (dataOffset < 0 || channels != 2 || bitsPerSample != 16) {
        m_lastError = "WAV должен быть stereo 16-bit PCM: " + path;
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }

    // Размер data-чанка может быть больше фактического (недописанный файл или
    // 0xFFFFFFFF у потоковой записи) — не читаем за пределами буфера
    if (dataSize < 0 || dataOffset + dataSize > len)
        dataSize = len - dataOffset;
    const int nSamples = dataSize / (channels * 2);
    left.resize(nSamples);
    right.resize(nSamples);
    const int16_t *pcm = reinterpret_cast<const int16_t*>(p + dataOffset);
    for (int i = 0; i < nSamples; ++i) {
        left[i]  = pcm[i * 2]     / 32768.0f;
        right[i] = pcm[i * 2 + 1] / 32768.0f;
    }
    return true;
}

bool DemucsWrapper::writeWav(const QString &path,
                              const QVector<float> &left,
                              const QVector<float> &right,
                              int sampleRate)
{
    const int nSamples  = qMin(left.size(), right.size());
    const int dataSize  = nSamples * 4;
    const int totalSize = 44 + dataSize;

    QByteArray buf(totalSize, '\0');
    uint8_t *p = reinterpret_cast<uint8_t*>(buf.data());

    auto w16 = [](uint8_t *d, int16_t v) { memcpy(d, &v, 2); };
    auto w32 = [](uint8_t *d, int32_t v) { memcpy(d, &v, 4); };

    memcpy(p,      "RIFF", 4); w32(p + 4,  totalSize - 8);
    memcpy(p + 8,  "WAVE", 4);
    memcpy(p + 12, "fmt ", 4); w32(p + 16, 16);
    w16(p + 20, 1);
    w16(p + 22, 2);
    w32(p + 24, sampleRate);
    w32(p + 28, sampleRate * 4);
    w16(p + 32, 4);
    w16(p + 34, 16);
    memcpy(p + 36, "data", 4); w32(p + 40, dataSize);

    int16_t *out = reinterpret_cast<int16_t*>(p + 44);
    for (int i = 0; i < nSamples; ++i) {
        out[i * 2]     = static_cast<int16_t>(qBound(-32768.f, left[i]  * 32767.f, 32767.f));
        out[i * 2 + 1] = static_cast<int16_t>(qBound(-32768.f, right[i] * 32767.f, 32767.f));
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        m_lastError = "Не удалось записать WAV: " + path;
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }
    f.write(buf);
    return true;
}
