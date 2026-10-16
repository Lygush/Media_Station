// modules/KaraokeModule/WhisperWrapper.h
#pragma once

#include <QObject>
#include <QString>
#include <QLibrary>
#include <QVector>
#include <QThread>
#include <functional>
#include <vector>

#include "whisper.h"

struct WhisperWord {
    QString text;
    qint64  t0_ms;
    qint64  t1_ms;
    float   prob;
};

struct TranscriptResult {
    bool                 success = false;
    QVector<WhisperWord> words;
    QString              language;
    QString              error;
};

using WhisperProgressCb = std::function<bool(int percent)>;

class WhisperWrapper : public QObject
{
    Q_OBJECT

public:
    explicit WhisperWrapper(QObject *parent = nullptr);
    ~WhisperWrapper();

    bool    load(const QString &dllDir, const QString &modelPath);
    bool    isLoaded()  const { return m_ctx != nullptr; }
    void    unload();
    QString lastError() const { return m_lastError; }

    /// initialPrompt — подсказка декодеру (например словарь слов песни из lrclib):
    /// повышает точность на редких словах и именах. Может быть пустой.
    TranscriptResult transcribe(const QString &wav16kPath,
                                WhisperProgressCb progressCb = nullptr,
                                const QString &initialPrompt = QString());

private:
    bool resolveSymbols();

    // whisper_full_parallel — точно как whisper-cli, пишет результаты в ctx
    using fn_init_t        = whisper_context*(*)(const char*, whisper_context_params);
    using fn_ctx_params_t  = whisper_context_params(*)();
    using fn_free_t        = void(*)(whisper_context*);
    using fn_full_params_t = whisper_full_params(*)(whisper_sampling_strategy);
    using fn_full_t        = int(*)(whisper_context*, whisper_full_params,
                                    const float*, int);          // whisper_full — без n_processors
    using fn_full_par_t    = int(*)(whisper_context*, whisper_full_params,
                                    const float*, int, int);     // whisper_full_parallel
    using fn_n_segments_t  = int(*)(whisper_context*);
    using fn_n_tokens_t    = int(*)(whisper_context*, int);
    using fn_token_data_t  = whisper_token_data(*)(whisper_context*, int, int);
    using fn_token_text_t  = const char*(*)(whisper_context*, int, int);
    using fn_lang_id_t     = int(*)(whisper_context*);
    using fn_lang_str_t    = const char*(*)(int);

    QLibrary         m_lib;
    whisper_context *m_ctx = nullptr;
    QString          m_lastError;

    fn_init_t        m_fnInit         = nullptr;
    fn_ctx_params_t  m_fnDefCtxParams = nullptr;
    fn_free_t        m_fnFree         = nullptr;
    fn_full_params_t m_fnFullParams   = nullptr;
    fn_full_t        m_fnFull         = nullptr;   // whisper_full (не parallel)
    fn_full_par_t    m_fnFullPar      = nullptr;   // whisper_full_parallel (не используем)
    fn_n_segments_t  m_fnNSegments    = nullptr;
    fn_n_tokens_t    m_fnNTokens      = nullptr;
    fn_token_data_t  m_fnTokenData    = nullptr;
    fn_token_text_t  m_fnTokenText    = nullptr;
    fn_lang_id_t     m_fnLangId       = nullptr;
    fn_lang_str_t    m_fnLangStr      = nullptr;
};
