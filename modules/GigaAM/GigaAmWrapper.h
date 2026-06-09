// modules/KaraokeModule/GigaAmWrapper.h
#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
#include <vector>
#include <memory>

#include <onnxruntime_cxx_api.h>

// Forward declaration для kiss_fft – не тянем весь заголовок в интерфейс
struct kiss_fftr_state;

// Одно слово с временными метками
struct GigaAmWord {
    QString text;
    qint64  t0_ms;  // начало в миллисекундах
    qint64  t1_ms;  // конец в миллисекундах
};

// Результат транскрипции
struct GigaAmResult {
    bool              success = false;
    QVector<GigaAmWord> words;
    QString           fullText;
    QString           error;
};

using GigaAmProgressCb = std::function<bool(int percent)>;

class GigaAmWrapper : public QObject
{
    Q_OBJECT

public:
    explicit GigaAmWrapper(QObject *parent = nullptr);
    ~GigaAmWrapper();

    // modelPath  — путь к v3_e2e_ctc.onnx
    // vocabPath  — путь к v3_e2e_ctc_vocab.txt
    bool load(const QString &modelPath, const QString &vocabPath);

    bool    isLoaded()  const { return m_session != nullptr; }
    void    unload();
    QString lastError() const { return m_lastError; }

    // wav16kPath — WAV 16kHz mono PCM (после конвертации ffmpeg)
    // Длинные треки режутся на чанки по 25 сек с overlap автоматически
    GigaAmResult transcribe(const QString &wav16kPath,
                            GigaAmProgressCb progressCb = nullptr);

private:
    // Параметры мел-спектрограммы (из preprocess.py GigaAM)
    static constexpr int kSampleRate  = 16000;
    static constexpr int kHopLength   = 160;    // sample_rate // 100
    static constexpr int kNFft        = 400;    // sample_rate // 40
    static constexpr int kNMels       = 64;
    static constexpr int kChunkMs     = 25000;  // 25 сек — лимит модели
    static constexpr int kOverlapMs   = 3000;   // 3 сек overlap — нужно для точных границ слов

    bool loadVocab(const QString &vocabPath);

    // Вычисление мел-спектрограммы (теперь с kiss_fft)
    std::vector<float> computeMelSpectrogram(const float *samples,
                                             int nSamples,
                                             int *outFrames) const;

    // Инференс одного чанка — возвращает log_probs [seq_len, vocab_size]
    std::vector<float> runInference(const std::vector<float> &melSpec,
                                    int nFrames);

    // CTC greedy decode с word timestamps (учитывает субдискретизацию)
    QVector<GigaAmWord> ctcDecode(const std::vector<float> &logProbs,
                                  int nFrames,
                                  qint64 offsetMs,
                                  double subsampleFactor = 1.0) const;

    // Читаем WAV файл — возвращает float32 сэмплы
    std::vector<float> loadWav(const QString &path, QString &error) const;

    // Инициализация kiss_fft плана (ленивая, один раз)
    void ensureFftCfg() const;

    // ONNX Runtime
    Ort::Env                              m_env;
    std::unique_ptr<Ort::Session>         m_session;
    Ort::SessionOptions                   m_sessionOptions;
    Ort::MemoryInfo                       m_memoryInfo;

    // Словарь токенов
    QVector<QString>  m_vocab;    // индекс → символ
    int               m_blankId = -1;  // CTC blank токен

    QString m_lastError;

    // kiss_fft состояние (mutable, чтобы инициализировать в const-методах)
    mutable kiss_fftr_state* m_fftCfg = nullptr;
};
