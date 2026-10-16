// modules/KaraokeModule/DemucsWrapper.h
#pragma once

#include <QString>
#include <QVector>
#include <functional>
#include <atomic>
#include <string>
#include <vector>

#include "onnxruntime_cxx_api.h"
#include "DemucsDsp.h"

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

    // ── Модель ────────────────────────────────────────────────────────────────
    // Вход  "input": [1, 2, 343980]        — waveform
    // Вход  "x":     [1, 4, 2048, 336]     — CaC-спектрограмма (см. DemucsDsp.h)
    // Выход "add_67":[1, 4, 2, 343980]     — ВРЕМЕННАЯ ветка (xt)
    // Выход частотной ветки [1, 4, 4, 2048, 336] — её нужно пропустить через iSTFT
    //   и сложить с временной. Определяется по числу элементов (имя зависит от экспорта).
    // Стемы: 0=drums 1=bass 2=other 3=vocals
    static constexpr int kChunkSamples = 343980;
    static constexpr int kStems        = 4;
    static constexpr int kVocalsStem   = 3;
    static constexpr float kOverlap    = 0.25f;   // как в demucs apply_model

    // Один чанк ровно kChunkSamples: результат — вокал и инструментал (L/R).
    bool runChunk(const float *left, const float *right,
                  std::vector<float> &vocL,  std::vector<float> &vocR,
                  std::vector<float> &instL, std::vector<float> &instR);

    // ── ONNX ──────────────────────────────────────────────────────────────────
    Ort::Env            m_env { ORT_LOGGING_LEVEL_WARNING, "DemucsWrapper" };
    Ort::Session       *m_session = nullptr;
    Ort::SessionOptions m_sessionOptions;

    std::vector<std::string> m_outputNames;
    bool                     m_loggedShapes = false;

    DemucsDsp::Stft m_dsp;

    QString m_lastError;
};
