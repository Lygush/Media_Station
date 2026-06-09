// modules/KaraokeModule/DemucsWrapper.h
#pragma once

#include <QString>
#include <QVector>
#include <functional>
#include <atomic>
#include <vector>

#include "onnxruntime_cxx_api.h"
#include "kiss/kiss_fftr.h"

using DemucsProgressCb = std::function<void(int percent)>;

class DemucsWrapper
{
public:
    DemucsWrapper();
    ~DemucsWrapper();

    bool load(const QString &onnxDllDir, const QString &modelPath);

    bool    isLoaded()  const { return m_session != nullptr; }
    QString lastError() const { return m_lastError; }

    bool separate(const QString          &inputWavPath,
                  const QString          &vocalsOutPath,
                  const QString          &instrOutPath,
                  DemucsProgressCb        progressCb = nullptr,
                  const std::atomic<bool> *cancelled = nullptr);

private:
    // ── WAV I/O ───────────────────────────────────────────────────────────────
    bool readWav(const QString &path,
                 QVector<float> &left, QVector<float> &right,
                 int &sampleRate);

    bool writeWav(const QString &path,
                  const QVector<float> &left,
                  const QVector<float> &right,
                  int sampleRate);

    // ── STFT / iSTFT ──────────────────────────────────────────────────────────
    // nfft=4096, hop=1024 совпадают с тем что использует HTDemucs
    static constexpr int kNfft   = 4096;
    static constexpr int kHop    = 1024;
    static constexpr int kNfreqs = kNfft / 2 + 1;  // 2049 — для stft/istft

    void stft (const float *pcm,    float *outReal, float *outImag);
    void istft(const float *inReal, const float *inImag, float *outPcm);

    // ── Spectrogram для ONNX-входа "x" ───────────────────────────────────────
    // Модель ожидает [1, 4, 2048, 336]:
    //   4 канала = re_L, im_L, re_R, im_R (complex-as-channels)
    //   2048 частотных бинов (без DC и Nyquist, т.е. бины 1..2048)
    //   336 фреймов = ceil(343980 / 1024)
    static constexpr int kSpecFreqs  = 2048;
    static constexpr int kSpecFrames = 336;

    void computeMagnitudeSpectrogram(const QVector<float> &leftIn,
                                     const QVector<float> &rightIn,
                                     std::vector<float>   &specOut);

    // ── Чанковая обработка ────────────────────────────────────────────────────
    // Длина чанка фиксирована моделью: вход "input" = [1, 2, 343980]
    static constexpr int kChunkSamples   = 343980;
    static constexpr int kOverlapSamples = 44100 / 4;  // 0.25 сек crossfade

    bool runOnnx(const QVector<float> &leftIn,  const QVector<float> &rightIn,
                 QVector<float>       &leftVoc,  QVector<float>       &rightVoc,
                 QVector<float>       &leftInst, QVector<float>       &rightInst);

    static void applyCrossfade(QVector<float> &prev, const QVector<float> &curr,
                               int overlapSamples);

    // ── ONNX ──────────────────────────────────────────────────────────────────
    Ort::Env            m_env { ORT_LOGGING_LEVEL_WARNING, "DemucsWrapper" };
    Ort::Session       *m_session = nullptr;
    Ort::SessionOptions m_sessionOptions;

    // ── kiss_fft ──────────────────────────────────────────────────────────────
    kiss_fftr_cfg  m_fftCfg  = nullptr;
    kiss_fftr_cfg  m_ifftCfg = nullptr;
    QVector<float> m_window;

    QString m_lastError;
};
