// modules/KaraokeModule/WhisperWrapper.cpp
#include "WhisperWrapper.h"
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QThread>
#include <QElapsedTimer>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

struct TranscribeCallbackData {
    WhisperProgressCb cb;
    bool          cancelled    = false;
    int           lastProgress = 0;
    QElapsedTimer sinceCheck;
};

static void progressCallback(whisper_context *, whisper_state *, int progress, void *userdata)
{
    auto *d = static_cast<TranscribeCallbackData *>(userdata);
    if (!d) return;
    d->lastProgress = progress;
    if (d->cb && !d->cb(progress)) d->cancelled = true;
}

// Вызывается whisper.cpp часто во время вычислений. Раньше отмена проверялась
// только в progress_callback (раз в несколько процентов) и не прерывала
// whisper_full — «Отмена» срабатывала лишь после окончания распознавания.
static bool abortCallback(void *userdata)
{
    auto *d = static_cast<TranscribeCallbackData *>(userdata);
    if (!d) return false;
    if (d->cancelled) return true;
    if (d->cb && (!d->sinceCheck.isValid() || d->sinceCheck.elapsed() > 250)) {
        d->sinceCheck.restart();
        if (!d->cb(d->lastProgress)) d->cancelled = true;
    }
    return d->cancelled;
}

WhisperWrapper::WhisperWrapper(QObject *parent) : QObject(parent) {}
WhisperWrapper::~WhisperWrapper() { unload(); }

bool WhisperWrapper::load(const QString &dllDir, const QString &modelPath)
{
    if (m_ctx) { qWarning() << "[WhisperWrapper] already loaded"; return true; }

#ifdef Q_OS_WIN
    const std::wstring wDir = QDir::toNativeSeparators(dllDir).toStdWString();
    if (!AddDllDirectory(wDir.c_str()))
        qWarning() << "[WhisperWrapper] AddDllDirectory failed for:" << dllDir;
#endif

    const QString dllPath = QDir(dllDir).filePath("whisper.dll");
    if (!QFile::exists(dllPath)) {
        m_lastError = "whisper.dll не найден: " + dllPath;
        qCritical() << "[WhisperWrapper]" << m_lastError;
        return false;
    }

    m_lib.setFileName(dllPath);
    m_lib.setLoadHints(QLibrary::ResolveAllSymbolsHint);
    if (!m_lib.load()) {
        m_lastError = "Не удалось загрузить: " + m_lib.errorString();
        qCritical() << "[WhisperWrapper]" << m_lastError;
        return false;
    }
    qDebug() << "[WhisperWrapper] whisper.dll загружен из" << dllPath;

    if (!resolveSymbols()) { m_lib.unload(); return false; }

    if (!QFile::exists(modelPath)) {
        m_lastError = "Файл модели не найден: " + modelPath;
        qCritical() << "[WhisperWrapper]" << m_lastError;
        m_lib.unload();
        return false;
    }

    whisper_context_params cparams = m_fnDefCtxParams();
    qDebug() << "[WhisperWrapper] Загружаем модель:" << modelPath;
    m_ctx = m_fnInit(modelPath.toUtf8().constData(), cparams);
    if (!m_ctx) {
        qWarning() << "[WhisperWrapper] GPU failed, trying CPU";
        cparams.use_gpu = false;
        m_ctx = m_fnInit(modelPath.toUtf8().constData(), cparams);
    }
    if (!m_ctx) {
        m_lastError = "Не удалось инициализировать контекст";
        qCritical() << "[WhisperWrapper]" << m_lastError;
        m_lib.unload();
        return false;
    }
    qDebug() << "[WhisperWrapper] Контекст создан";
    return true;
}

void WhisperWrapper::unload()
{
    if (m_ctx) { if (m_fnFree) m_fnFree(m_ctx); m_ctx = nullptr; }
    if (m_lib.isLoaded()) m_lib.unload();
    m_fnInit = nullptr; m_fnDefCtxParams = nullptr; m_fnFree = nullptr;
    m_fnFullParams = nullptr; m_fnFull = nullptr; m_fnFullPar = nullptr;
    m_fnNSegments = nullptr; m_fnNTokens = nullptr;
    m_fnTokenData = nullptr; m_fnTokenText = nullptr;
    m_fnLangId = nullptr; m_fnLangStr = nullptr;
}

TranscriptResult WhisperWrapper::transcribe(const QString &wav16kPath,
                                             WhisperProgressCb progressCb)
{
    TranscriptResult result;
    if (!m_ctx) { result.error = "Не инициализирован"; return result; }
    if (!QFile::exists(wav16kPath)) { result.error = "WAV не найден"; return result; }

    // ── Читаем WAV ────────────────────────────────────────────────────────────
    QFile wav(wav16kPath);
    if (!wav.open(QIODevice::ReadOnly)) { result.error = "Не удалось открыть WAV"; return result; }
    QByteArray data = wav.readAll();
    wav.close();

    int dataOffset = -1, dataSize = -1;
    {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(data.constData());
        int len = data.size();
        if (len < 12 || memcmp(p,"RIFF",4)!=0 || memcmp(p+8,"WAVE",4)!=0) {
            result.error = "Некорректный WAV"; return result;
        }
        int pos = 12;
        while (pos + 8 <= len) {
            const char    *id = reinterpret_cast<const char*>(p + pos);
            const uint32_t sz = *reinterpret_cast<const uint32_t*>(p + pos + 4);
            if (memcmp(id,"data",4)==0) { dataOffset = pos+8; dataSize=(int)sz; break; }
            pos += 8 + (int)sz; if (sz&1) ++pos;
        }
    }
    if (dataOffset < 0 || dataOffset + dataSize > data.size()) {
        result.error = "data chunk не найден"; return result;
    }

    const int16_t *pcm16 = reinterpret_cast<const int16_t*>(data.constData() + dataOffset);
    const int nSamples   = dataSize / 2;
    std::vector<float> pcmf32(nSamples);
    for (int i = 0; i < nSamples; ++i) pcmf32[i] = pcm16[i] / 32768.0f;
    qDebug() << "[WhisperWrapper] Аудио:" << nSamples << "сэмплов (" << (nSamples/16000.0f) << "сек)";

    // ── Параметры ─────────────────────────────────────────────────────────────
    whisper_full_params params = m_fnFullParams(WHISPER_SAMPLING_BEAM_SEARCH);
    params.n_threads             = qMin(4, QThread::idealThreadCount());
    params.translate             = false;
    params.no_context            = false;
    params.single_segment        = false;
    params.print_special         = false;
    params.print_progress        = false;
    params.print_realtime        = false;
    params.print_timestamps      = false;
    params.no_timestamps         = false;
    params.token_timestamps      = true;
    params.language              = "ru";
    params.detect_language       = false;
    params.beam_search.beam_size = 5;
    params.greedy.best_of        = 5;
    params.no_speech_thold       = 0.6f;
    params.logprob_thold         = -1.0f;
    params.temperature_inc       = 0.2f;
    params.entropy_thold         = 2.4f;
    params.max_len               = 0;
    params.max_tokens            = 0;
    params.split_on_word         = false;
    params.tdrz_enable           = false;
    params.suppress_blank        = false;

    TranscribeCallbackData cbData;
    cbData.cb = progressCb;
    if (progressCb) {
        params.progress_callback           = progressCallback;
        params.progress_callback_user_data = &cbData;
        params.abort_callback              = abortCallback;
        params.abort_callback_user_data    = &cbData;
    }

    // ── whisper_full (не parallel!) ───────────────────────────────────────────
    qDebug() << "[WhisperWrapper] Запускаем whisper_full...";
    const int ret = m_fnFull(m_ctx, params, pcmf32.data(), nSamples);
    qDebug() << "[WhisperWrapper] ret =" << ret;
    if (cbData.cancelled) { result.error = "Отменено"; return result; }
    if (ret != 0) { result.error = QString("whisper_full вернул: %1").arg(ret); return result; }

    // ── Читаем сегменты и группируем токены в слова ───────────────────────────
    // Whisper токены — это части слова (BPE). Токен начинающийся с пробела
    // означает начало нового слова. Группируем токены пока не встретим пробел.
    const int nSeg = m_fnNSegments(m_ctx);
    qDebug() << "[WhisperWrapper] n_segments =" << nSeg;

    // Фильтр галлюцинаций — список типичных повторяющихся фраз
    static const QStringList hallucinations = {
        "продолжение следует", "субтитры", "titres", "перевод", "редактор",
        "создавал", "dimatorzok", "семкин", "егоров", "корректор"
    };

    for (int s = 0; s < nSeg; ++s) {
        const int nTok = m_fnNTokens(m_ctx, s);

        // Собираем все токены сегмента в слова
        // Слово собираем в БАЙТАХ: токен BPE может содержать половину UTF-8
        // символа (кириллица), и пословное декодирование давало «�» в тексте.
        QByteArray curWord;
        qint64   curT0 = -1, curT1 = -1;

        auto flushWord = [&]() {
            if (curWord.isEmpty() || curT0 < 0) { curWord.clear(); return; }
            const QString w = QString::fromUtf8(curWord).trimmed();
            if (w.isEmpty()) { curWord.clear(); return; }
            // Фильтруем галлюцинации
            const QString wLow = w.toLower();
            for (const QString &h : hallucinations)
                if (wLow.contains(h)) { curWord.clear(); return; }
            WhisperWord ww;
            ww.text  = w;
            ww.t0_ms = curT0;
            ww.t1_ms = curT1;
            ww.prob  = 1.0f;
            result.words.append(ww);
            curWord.clear();
        };

        for (int t = 0; t < nTok; ++t) {
            const char *raw = m_fnTokenText(m_ctx, s, t);
            if (!raw) continue;
            const QByteArray tok(raw);

            // Спецтокены пропускаем
            if ((tok.startsWith('<') && tok.endsWith('>')) ||
                (tok.startsWith('[') && tok.endsWith(']')))
                continue;

            whisper_token_data td = m_fnTokenData(m_ctx, s, t);
            const qint64 t0 = td.t0 * 10;
            const qint64 t1 = td.t1 * 10;

            // Токен с ведущим пробелом = начало нового слова
            if (tok.startsWith(' ') && !curWord.isEmpty()) {
                flushWord();
            }

            if (curWord.isEmpty()) curT0 = t0;
            curT1 = t1;
            curWord += tok.trimmed();
        }
        flushWord();
    }

    const int langId = m_fnLangId(m_ctx);
    result.language  = (langId >= 0 && m_fnLangStr)
                       ? QString::fromUtf8(m_fnLangStr(langId)) : "unknown";
    qDebug() << "[WhisperWrapper] Язык:" << result.language << "| Слов:" << result.words.size();
    result.success = true;
    return result;
}

bool WhisperWrapper::resolveSymbols()
{
    bool ok = true;
    auto resolve = [&](const char *name, auto &fn) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(m_lib.resolve(name));
        if (!fn) { qCritical() << "[WhisperWrapper] FAILED:" << name; ok = false; }
        else      { qDebug()   << "[WhisperWrapper] OK:"     << name; }
    };

    resolve("whisper_init_from_file_with_params", m_fnInit);
    resolve("whisper_context_default_params",     m_fnDefCtxParams);
    resolve("whisper_free",                       m_fnFree);
    resolve("whisper_full_default_params",        m_fnFullParams);
    resolve("whisper_full",                       m_fnFull);      // ← не parallel!
    resolve("whisper_full_parallel",              m_fnFullPar);   // резолвим, не используем
    resolve("whisper_full_n_segments",            m_fnNSegments);
    resolve("whisper_full_n_tokens",              m_fnNTokens);
    resolve("whisper_full_get_token_data",        m_fnTokenData);
    resolve("whisper_full_get_token_text",        m_fnTokenText);
    resolve("whisper_full_lang_id",               m_fnLangId);
    resolve("whisper_lang_str",                   m_fnLangStr);

    if (!ok) m_lastError = "Не удалось загрузить символы из whisper.dll";
    return ok;
}
