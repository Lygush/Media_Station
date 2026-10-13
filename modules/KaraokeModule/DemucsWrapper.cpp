// modules/KaraokeModule/DemucsWrapper.cpp
#include "DemucsWrapper.h"

#include <QFile>
#include <QDebug>
#include <cmath>
#include <cstring>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Константы модели (из inspect onnx):
//   input  "input": [1, 2, 343980]
//   input  "x":     [1, 4, 2048, 336]   ← magnitude spectrogram, complex-as-channels
//   output "add_67":[1, 4, 2, 343980]   ← 4 стема (drums/bass/other/vocals)
//
// nfft=4096, hop=1024 → nfreqs=2049, но модель ожидает 2048 (без DC+Nyquist)
// frames = ceil(343980 / 1024) = 336
// complex-as-channels: [re_L, im_L, re_R, im_R] → 4 канала × 2048 × 336
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

DemucsWrapper::DemucsWrapper()
{
    // Окно Хэннинга длиной kNfft
    m_window.resize(kNfft);
    for (int i = 0; i < kNfft; ++i)
        m_window[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (kNfft - 1)));

    m_fftCfg  = kiss_fftr_alloc(kNfft, 0, nullptr, nullptr);
    m_ifftCfg = kiss_fftr_alloc(kNfft, 1, nullptr, nullptr);
}

DemucsWrapper::~DemucsWrapper()
{
    delete m_session;
    m_session = nullptr;
    if (m_fftCfg)  { kiss_fft_free(m_fftCfg);  m_fftCfg  = nullptr; }
    if (m_ifftCfg) { kiss_fft_free(m_ifftCfg); m_ifftCfg = nullptr; }
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

        qDebug() << "[DemucsWrapper] модель загружена:" << modelPath;
        return true;

    } catch (const Ort::Exception &e) {
        m_lastError = QString("ONNX ошибка: %1").arg(e.what());
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// separate
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

    const int totalSamples = leftIn.size();
    QVector<float> outLeftVoc (totalSamples, 0.f), outRightVoc (totalSamples, 0.f);
    QVector<float> outLeftInst(totalSamples, 0.f), outRightInst(totalSamples, 0.f);

    // Модель работает с фиксированными чанками kChunkSamples=343980
    // Overlap для crossfade на стыках
    const int stride      = kChunkSamples - kOverlapSamples;
    int       chunkStart  = 0;
    int       chunkIdx    = 0;
    const int totalChunks = (totalSamples + stride - 1) / stride;

    QVector<float> prevVocL, prevVocR, prevInstL, prevInstR;

    while (chunkStart < totalSamples) {
        if (cancelled && cancelled->load()) { m_lastError = "Отменено"; return false; }

        const int chunkEnd = qMin(chunkStart + kChunkSamples, totalSamples);
        const int chunkLen = chunkEnd - chunkStart;

        // Паддинг нулями до kChunkSamples
        QVector<float> chunkL(kChunkSamples, 0.f), chunkR(kChunkSamples, 0.f);
        std::copy(leftIn.constData()  + chunkStart,
                  leftIn.constData()  + chunkEnd, chunkL.data());
        std::copy(rightIn.constData() + chunkStart,
                  rightIn.constData() + chunkEnd, chunkR.data());

        QVector<float> vocL, vocR, instL, instR;
        if (!runOnnx(chunkL, chunkR, vocL, vocR, instL, instR)) return false;

        // Crossfade с предыдущим чанком
        if (!prevVocL.isEmpty()) {
            applyCrossfade(prevVocL,  vocL,  kOverlapSamples);
            applyCrossfade(prevVocR,  vocR,  kOverlapSamples);
            applyCrossfade(prevInstL, instL, kOverlapSamples);
            applyCrossfade(prevInstR, instR, kOverlapSamples);
        }

        const int writeLen = qMin(chunkLen, (int)vocL.size());
        std::copy(vocL.constData(),  vocL.constData()  + writeLen, outLeftVoc.data()   + chunkStart);
        std::copy(vocR.constData(),  vocR.constData()  + writeLen, outRightVoc.data()  + chunkStart);
        std::copy(instL.constData(), instL.constData() + writeLen, outLeftInst.data()  + chunkStart);
        std::copy(instR.constData(), instR.constData() + writeLen, outRightInst.data() + chunkStart);

        prevVocL  = vocL;  prevVocR  = vocR;
        prevInstL = instL; prevInstR = instR;

        chunkStart += stride;
        ++chunkIdx;

        if (progressCb)
            progressCb(chunkIdx * 100 / totalChunks);
    }

    if (!writeWav(vocalsOutPath, outLeftVoc,  outRightVoc,  44100)) return false;
    if (!writeWav(instrOutPath,  outLeftInst, outRightInst, 44100)) return false;

    qDebug() << "[DemucsWrapper] готово:" << vocalsOutPath;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// computeMagnitudeSpectrogram
//
// Вычисляет magnitude spectrogram в формате complex-as-channels:
// [re_L, im_L, re_R, im_R] × kSpecFreqs × kSpecFrames
// Размер выхода: 4 * kSpecFreqs * kSpecFrames float'ов
// ─────────────────────────────────────────────────────────────────────────────

void DemucsWrapper::computeMagnitudeSpectrogram(
    const QVector<float> &leftIn,
    const QVector<float> &rightIn,
    std::vector<float>   &specOut)
{
    // frames = ceil(kChunkSamples / kHop)
    specOut.assign(4 * kSpecFreqs * kSpecFrames, 0.f);

    // Указатели на 4 плоскости: re_L, im_L, re_R, im_R
    float *reL = specOut.data();
    float *imL = reL + kSpecFreqs * kSpecFrames;
    float *reR = imL + kSpecFreqs * kSpecFrames;
    float *imR = reR + kSpecFreqs * kSpecFrames;

    QVector<float> frameBuf(kNfft, 0.f);
    QVector<kiss_fft_cpx> spectrum(kNfft / 2 + 1);

    for (int frame = 0; frame < kSpecFrames; ++frame) {
        const int offset = frame * kHop;

        // Левый канал
        for (int i = 0; i < kNfft; ++i) {
            int idx = offset + i;
            frameBuf[i] = (idx < leftIn.size()) ? leftIn[idx] * m_window[i] : 0.f;
        }
        kiss_fftr(m_fftCfg, frameBuf.constData(), spectrum.data());
        // Берём бины 1..kSpecFreqs (пропускаем DC=0 и Nyquist)
        for (int f = 0; f < kSpecFreqs; ++f) {
            reL[frame + f * kSpecFrames] = spectrum[f + 1].r;
            imL[frame + f * kSpecFrames] = spectrum[f + 1].i;
        }

        // Правый канал
        for (int i = 0; i < kNfft; ++i) {
            int idx = offset + i;
            frameBuf[i] = (idx < rightIn.size()) ? rightIn[idx] * m_window[i] : 0.f;
        }
        kiss_fftr(m_fftCfg, frameBuf.constData(), spectrum.data());
        for (int f = 0; f < kSpecFreqs; ++f) {
            reR[frame + f * kSpecFrames] = spectrum[f + 1].r;
            imR[frame + f * kSpecFrames] = spectrum[f + 1].i;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runOnnx
// ─────────────────────────────────────────────────────────────────────────────

bool DemucsWrapper::runOnnx(const QVector<float> &leftIn,  const QVector<float> &rightIn,
                             QVector<float>       &leftVoc,  QVector<float>       &rightVoc,
                             QVector<float>       &leftInst, QVector<float>       &rightInst)
{
    try {
        auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // ── Тензор 1: waveform "input" [1, 2, kChunkSamples] ─────────────────
        // Формат: сначала все левые сэмплы, затем все правые
        std::vector<float> waveData(2 * kChunkSamples);
        std::copy(leftIn.constData(),  leftIn.constData()  + kChunkSamples, waveData.data());
        std::copy(rightIn.constData(), rightIn.constData() + kChunkSamples, waveData.data() + kChunkSamples);

        const int64_t waveShape[] = { 1, 2, kChunkSamples };
        Ort::Value waveTensor = Ort::Value::CreateTensor<float>(
            memInfo, waveData.data(), waveData.size(), waveShape, 3);

        // ── Тензор 2: spectrogram "x" [1, 4, kSpecFreqs, kSpecFrames] ────────
        // complex-as-channels: [re_L, im_L, re_R, im_R]
        std::vector<float> specData;
        computeMagnitudeSpectrogram(leftIn, rightIn, specData);

        const int64_t specShape[] = { 1, 4, kSpecFreqs, kSpecFrames };
        Ort::Value specTensor = Ort::Value::CreateTensor<float>(
            memInfo, specData.data(), specData.size(), specShape, 4);

        // ── Инференс ──────────────────────────────────────────────────────────
        const char *inputNames[]  = { "input", "x" };
        const char *outputNames[] = { "add_67" };   // waveform выход [1,4,2,343980]

        Ort::Value inputTensors[] = { std::move(waveTensor), std::move(specTensor) };

        auto outputs = m_session->Run(
            Ort::RunOptions{nullptr},
            inputNames,  inputTensors, 2,
            outputNames, 1);

        // ── Читаем выход [1, 4, 2, kChunkSamples] ────────────────────────────
        // stems: 0=drums, 1=bass, 2=other, 3=vocals
        const float *out   = outputs[0].GetTensorData<float>();
        const int    nSamp = kChunkSamples;
        const int    stemStride = 2 * nSamp;  // left+right за один стем

        leftVoc.resize(nSamp);  rightVoc.resize(nSamp);
        leftInst.resize(nSamp); rightInst.resize(nSamp);

        // Вокал = стем 3
        const float *vocL = out + 3 * stemStride;
        const float *vocR = out + 3 * stemStride + nSamp;
        std::copy(vocL, vocL + nSamp, leftVoc.data());
        std::copy(vocR, vocR + nSamp, rightVoc.data());

        // Инструментал = drums(0) + bass(1) + other(2)
        std::fill(leftInst.data(),  leftInst.data()  + nSamp, 0.f);
        std::fill(rightInst.data(), rightInst.data() + nSamp, 0.f);
        for (int s = 0; s < 3; ++s) {
            const float *sL = out + s * stemStride;
            const float *sR = out + s * stemStride + nSamp;
            for (int i = 0; i < nSamp; ++i) {
                leftInst[i]  += sL[i];
                rightInst[i] += sR[i];
            }
        }

        return true;

    } catch (const Ort::Exception &e) {
        m_lastError = QString("ONNX inference: %1").arg(e.what());
        qCritical() << "[DemucsWrapper]" << m_lastError;
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// STFT / iSTFT  (используются для LrcBuilder, не для Demucs)
// ─────────────────────────────────────────────────────────────────────────────

void DemucsWrapper::stft(const float *pcm, float *outReal, float *outImag)
{
    QVector<float> windowed(kNfft);
    for (int i = 0; i < kNfft; ++i)
        windowed[i] = pcm[i] * m_window[i];

    QVector<kiss_fft_cpx> spectrum(kNfft / 2 + 1);
    kiss_fftr(m_fftCfg, windowed.constData(), spectrum.data());

    for (int i = 0; i < kNfreqs; ++i) {
        outReal[i] = spectrum[i].r;
        outImag[i] = spectrum[i].i;
    }
}

void DemucsWrapper::istft(const float *inReal, const float *inImag, float *outPcm)
{
    QVector<kiss_fft_cpx> spectrum(kNfreqs);
    for (int i = 0; i < kNfreqs; ++i) {
        spectrum[i].r = inReal[i];
        spectrum[i].i = inImag[i];
    }

    QVector<float> raw(kNfft);
    kiss_fftri(m_ifftCfg, spectrum.constData(), raw.data());

    const float norm = 1.0f / kNfft;
    for (int i = 0; i < kNfft; ++i)
        outPcm[i] = raw[i] * norm * m_window[i];
}

// ─────────────────────────────────────────────────────────────────────────────
// Crossfade
// ─────────────────────────────────────────────────────────────────────────────

void DemucsWrapper::applyCrossfade(QVector<float> &prev, const QVector<float> &curr,
                                    int overlapSamples)
{
    const int prevSize = prev.size();
    for (int i = 0; i < overlapSamples && i < prevSize && i < curr.size(); ++i) {
        const float t = static_cast<float>(i) / overlapSamples;
        prev[prevSize - overlapSamples + i] =
            prev[prevSize - overlapSamples + i] * (1.f - t) + curr[i] * t;
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
