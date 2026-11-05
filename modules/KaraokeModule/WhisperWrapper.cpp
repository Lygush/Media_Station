// modules/KaraokeModule/WhisperWrapper.cpp
#include "WhisperWrapper.h"
#include "VoiceActivity.h"
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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

    // Время слов по DTW (выравнивание по вниманию модели) заметно точнее
    // обычных token_timestamps, которые на пении «плывут» на сотни мс.
    // Набор «голов выравнивания» свой у каждой модели — выбираем по имени
    // файла; при несовпадении DTW даёт мусор, поэтому для неизвестных
    // моделей его не включаем. С flash attention DTW в whisper.cpp не работает.
    const QString modelName = QFileInfo(modelPath).fileName().toLower();
    whisper_alignment_heads_preset preset = WHISPER_AHEADS_NONE;
    if (modelName.contains("large-v3-turbo") || modelName.contains("large-v3_turbo"))
        preset = WHISPER_AHEADS_LARGE_V3_TURBO;
    else if (modelName.contains("large-v3"))
        preset = WHISPER_AHEADS_LARGE_V3;
    else if (modelName.contains("large-v2"))
        preset = WHISPER_AHEADS_LARGE_V2;
    else if (modelName.contains("medium") && !modelName.contains(".en"))
        preset = WHISPER_AHEADS_MEDIUM;
    if (preset != WHISPER_AHEADS_NONE) {
        cparams.dtw_token_timestamps = true;
        cparams.dtw_aheads_preset    = preset;
        cparams.flash_attn           = false;
    }

    qDebug() << "[WhisperWrapper] Загружаем модель:" << modelPath
             << "| DTW:" << (preset != WHISPER_AHEADS_NONE);
    m_ctx = m_fnInit(modelPath.toUtf8().constData(), cparams);
    if (!m_ctx) {
        qWarning() << "[WhisperWrapper] GPU failed, trying CPU";
        cparams.use_gpu = false;
        m_ctx = m_fnInit(modelPath.toUtf8().constData(), cparams);
    }
    if (!m_ctx && cparams.dtw_token_timestamps) {
        qWarning() << "[WhisperWrapper] init with DTW failed, retry without DTW";
        cparams.dtw_token_timestamps = false;
        cparams.dtw_aheads_preset    = WHISPER_AHEADS_NONE;
        m_ctx = m_fnInit(modelPath.toUtf8().constData(), cparams);
    }
    m_dtw = m_ctx && cparams.dtw_token_timestamps;
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
                                           WhisperProgressCb progressCb,
                                           const QString &initialPrompt)
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

    // ── Участки с вокалом ─────────────────────────────────────────────────────
    // Whisper на длинной дорожке с паузами теряет синхронизацию и галлюцинирует
    // («Thank you» на тишине, равномерная сетка повторов). Режем на участки
    // с голосом и распознаём каждый отдельно.
    const VoiceActivity::Result vad = VoiceActivity::analyze(pcmf32.data(), nSamples, 16000);
    qDebug() << "[WhisperWrapper] участков с вокалом:" << int(vad.regions.size())
             << "| порог RMS:" << vad.threshold;

    result.language = "unknown";
    if (vad.regions.empty()) {
        // Инструментал / тишина — это не ошибка, просто текста нет
        qWarning() << "[WhisperWrapper] голос не найден";
        result.success = true;
        return result;
    }

    // Порядок обработки: сначала до трёх самых длинных участков — по ним
    // голосованием определяем язык всей песни. Раньше язык брался по одному
    // самому длинному участку, и если там был, например, английский припев
    // в русской песне — вся песня распознавалась как английская.
    const int nRegions = int(vad.regions.size());
    std::vector<int> order(nRegions);
    for (int i = 0; i < nRegions; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return vad.regions[a].end - vad.regions[a].start > vad.regions[b].end - vad.regions[b].start;
    });
    const int kVote = qMin(3, nRegions);
    std::sort(order.begin() + kVote, order.end());   // остальные — по времени

    const QByteArray promptBytes = initialPrompt.toUtf8();

    auto makeParams = [&](const QByteArray &lang) {
        whisper_full_params params = m_fnFullParams(WHISPER_SAMPLING_BEAM_SEARCH);
        params.n_threads             = qMin(4, QThread::idealThreadCount());
        params.translate             = false;
        // Без контекста предыдущего участка — иначе повторы («hello hello …»)
        // и ошибки тянутся дальше по песне
        params.no_context            = true;
        params.single_segment        = false;
        params.print_special         = false;
        params.print_progress        = false;
        params.print_realtime        = false;
        params.print_timestamps      = false;
        params.no_timestamps         = false;
        params.token_timestamps      = true;
        params.suppress_nst          = true;     // ♪ и прочие не-речевые токены
        // Словарь слов песни (из lrclib) — Whisper точнее на редких словах
        if (!promptBytes.isEmpty())
            params.initial_prompt    = promptBytes.constData();
        params.language              = lang.constData();   // "auto" — определить
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
        params.suppress_blank        = true;
        return params;
    };

    // Фильтр галлюцинаций: целые ФРАЗЫ из титров/субтитров, которые Whisper
    // выдумывает на тишине и проигрышах. Проверяется текст всего сегмента.
    // Раньше проверялось вхождение подстроки в каждое слово, и вместе с
    // мусором выкидывались настоящие слова песни («перевод», «создавал»).
    static const QStringList kHallucinationPhrases = {
        "продолжение следует", "субтитры сделал", "субтитры создавал",
        "субтитры подготовил", "редактор субтитров", "корректор а.",
        "а.семкин", "а.егорова", "dimatorzok", "amara.org",
        "thanks for watching", "thank you for watching",
        "спасибо за просмотр", "подписывайтесь на канал",
        "sous-titres", "sous-titrage"
    };

    struct RegionOut {
        QVector<WhisperWord> words;
        QByteArray           lang;
        bool                 ok = false;
    };

    TranscribeCallbackData cbData;

    // Распознаёт один участок с заданным языком ("auto" — определить)
    auto runRegion = [&](int k, int regionIdx, const QByteArray &lang) -> RegionOut {
        RegionOut out;
        const VoiceActivity::Region &rg = vad.regions[regionIdx];
        const qint64 offsetMs = qint64(rg.start) * 1000 / 16000;

        // Копия участка; whisper не принимает аудио короче ~1 с — добиваем тишиной
        std::vector<float> buf(pcmf32.begin() + rg.start, pcmf32.begin() + rg.end);
        const size_t minSamples = 16000 * 11 / 10;
        if (buf.size() < minSamples) buf.resize(minSamples, 0.f);

        cbData.cancelled    = false;
        cbData.lastProgress = 0;
        cbData.sinceCheck.invalidate();
        cbData.cb = nullptr;
        if (progressCb) {
            cbData.cb = [&, k](int p) -> bool {
                return progressCb(qMin(99, (k * 100 + p) / nRegions));
            };
        }

        whisper_full_params params = makeParams(lang);
        if (progressCb) {
            params.progress_callback           = progressCallback;
            params.progress_callback_user_data = &cbData;
            params.abort_callback              = abortCallback;
            params.abort_callback_user_data    = &cbData;
        }

        const int ret = m_fnFull(m_ctx, params, buf.data(), int(buf.size()));
        if (cbData.cancelled) return out;
        if (ret != 0) {
            qWarning() << "[WhisperWrapper] участок" << k << "whisper_full вернул" << ret;
            return out;                    // один плохой участок не должен убивать всю песню
        }
        out.ok = true;
        const int langId = m_fnLangId(m_ctx);
        if (langId >= 0 && m_fnLangStr)
            out.lang = QByteArray(m_fnLangStr(langId));

        // ── Сегменты → слова ─────────────────────────────────────────────────
        // Токен с ведущим пробелом = начало нового слова.
        const int nSeg = m_fnNSegments(m_ctx);
        for (int sIdx = 0; sIdx < nSeg; ++sIdx) {
            const int nTok = m_fnNTokens(m_ctx, sIdx);
            QVector<WhisperWord> segWords;

            // Слово собираем в БАЙТАХ: токен BPE может содержать половину UTF-8
            // символа (кириллица), и пословное декодирование давало «�».
            QByteArray curWord;
            qint64     curT0 = -1, curT1 = -1;

            auto flushWord = [&]() {
                if (curWord.isEmpty() || curT0 < 0) { curWord.clear(); return; }
                const QString w = QString::fromUtf8(curWord).trimmed();
                curWord.clear();
                if (w.isEmpty()) return;

                const qint64 t0 = offsetMs + curT0;
                const qint64 t1 = qMax(t0, offsetMs + curT1);
                // Слово там, где вокала нет (±150 мс) — галлюцинация
                if (!vad.isActive(t0 - 150, t1 + 150)) return;

                WhisperWord ww;
                ww.text  = w;
                ww.t0_ms = t0;
                ww.t1_ms = t1;
                ww.prob  = 1.0f;
                segWords.append(ww);
            };

            for (int t = 0; t < nTok; ++t) {
                const char *raw = m_fnTokenText(m_ctx, sIdx, t);
                if (!raw) continue;
                const QByteArray tok(raw);

                // Спецтокены пропускаем
                if ((tok.startsWith('<') && tok.endsWith('>')) ||
                    (tok.startsWith('[') && tok.endsWith(']')))
                    continue;

                const whisper_token_data td = m_fnTokenData(m_ctx, sIdx, t);
                if (tok.startsWith(' ') && !curWord.isEmpty())
                    flushWord();

                if (curWord.isEmpty()) {
                    // DTW даёт момент начала токена точнее, чем t0
                    curT0 = (m_dtw && td.t_dtw >= 0) ? qint64(td.t_dtw) * 10
                                                     : qint64(td.t0) * 10;
                }
                curT1 = qint64(td.t1) * 10;
                curWord += tok.trimmed();
            }
            flushWord();

            // DTW-начало следующего слова — естественный конец предыдущего
            if (m_dtw) {
                for (int i = 0; i + 1 < segWords.size(); ++i)
                    if (segWords[i + 1].t0_ms > segWords[i].t0_ms)
                        segWords[i].t1_ms = qMin(segWords[i].t1_ms, segWords[i + 1].t0_ms);
                for (auto &w : segWords)
                    w.t1_ms = qMax(w.t1_ms, w.t0_ms + 80);
            }

            // Фраза из титров — отбрасываем сегмент целиком
            QString segText;
            for (const auto &w : segWords) segText += w.text.toLower() + QLatin1Char(' ');
            bool junk = false;
            for (const QString &h : kHallucinationPhrases)
                if (segText.contains(h)) { junk = true; break; }
            if (junk) {
                qDebug() << "[WhisperWrapper] сегмент-галлюцинация отброшен:" << segText.trimmed();
                continue;
            }
            out.words += segWords;
        }

        // Очень короткий участок с 1–2 словами — почти всегда галлюцинация Whisper
        // на остатках звука («ПЕСНЯ», «субтитров», «Thank you» в конце трека)
        if (out.words.size() <= 2 && (rg.end - rg.start) < 16000 * 4) {
            qDebug() << "[WhisperWrapper] участок" << k + 1 << "отброшен как галлюцинация";
            out.words.clear();
        }
        return out;
    };

    // ── Голосование за язык ───────────────────────────────────────────────────
    QVector<RegionOut> voteOut(kVote);
    QHash<QByteArray, qint64> langWeight;        // язык → суммарная длительность
    for (int k = 0; k < kVote; ++k) {
        voteOut[k] = runRegion(k, order[k], "auto");
        if (cbData.cancelled) { result.error = "Отменено"; return result; }
        const auto &rg = vad.regions[order[k]];
        if (voteOut[k].ok && !voteOut[k].lang.isEmpty())
            langWeight[voteOut[k].lang] += rg.end - rg.start;
    }
    QByteArray langCode = "auto";
    for (auto it = langWeight.constBegin(); it != langWeight.constEnd(); ++it)
        if (langCode == "auto" || it.value() > langWeight.value(langCode))
            langCode = it.key();
    result.language = langCode == "auto" ? QStringLiteral("unknown") : QString::fromUtf8(langCode);
    qDebug() << "[WhisperWrapper] язык песни:" << result.language << "| голоса:" << langWeight;

    // Участки голосования, распознанные на другом языке, — переделываем
    for (int k = 0; k < kVote; ++k) {
        if (voteOut[k].ok && langCode != "auto" && voteOut[k].lang != langCode) {
            qDebug() << "[WhisperWrapper] участок" << k + 1 << "распознан как" << voteOut[k].lang
                     << "— повтор на" << langCode;
            voteOut[k] = runRegion(k, order[k], langCode);
            if (cbData.cancelled) { result.error = "Отменено"; return result; }
        }
        result.words += voteOut[k].words;
    }

    // ── Остальные участки — с найденным языком ───────────────────────────────
    for (int k = kVote; k < nRegions; ++k) {
        const RegionOut ro = runRegion(k, order[k], langCode);
        if (cbData.cancelled) { result.error = "Отменено"; return result; }
        result.words += ro.words;
        qDebug() << "[WhisperWrapper] участок" << k + 1 << "/" << nRegions
                 << "(" << qint64(vad.regions[order[k]].start) / 16000 << "с ) | слов всего:"
                 << result.words.size();
    }

    // Участки обрабатывались не по порядку времени
    std::stable_sort(result.words.begin(), result.words.end(),
                     [](const WhisperWord &a, const WhisperWord &b) { return a.t0_ms < b.t0_ms; });

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
