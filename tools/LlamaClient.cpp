// tools/LlamaClient.cpp
#include "LlamaClient.h"
#include "llama.h"
#include "ggml.h"
#include "core/EventBus.h"
#include "LoggerModule.h"

#include <QDebug>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRecursiveMutex>
#include <QMutexLocker>
#include <QPointer>



// n_ctx=12288: влезает промпт ~9-10k токенов + 2k на ответ с запасом.
// KV cache Qwen2.5-7B при таком контексте ~410 MiB — терпимо даже для 8 GB VRAM.
static constexpr int kCtxSize   = 12288;
static constexpr int kMaxTokens = 2048;
static constexpr int kGpuLayers = 999;

// ── llama.cpp log callback ────────────────────────────────────────────────────
// userdata — указатель на LlamaClient. Колбэк вызывается из потока llama.cpp,
// поэтому используем invokeMethod(Qt::QueuedConnection) для безопасной передачи
// в поток Qt.
// ── Глобальное состояние llama.cpp ────────────────────────────────────────────
// llama_backend_init/free и llama_log_set — глобальные на весь процесс, а
// клиентов два (GameInfo и Karaoke). Раньше каждый клиент вызывал backend_free
// в деструкторе — даже когда второй ещё работал, — а обработчик журнала
// указывал на последний созданный клиент и после его удаления становился
// висячим указателем.
static QRecursiveMutex s_globalMutex;   // рекурсивный: backend_init/free пишут в журнал под этой блокировкой
static int           s_backendRefs = 0;
static LlamaClient  *s_logOwner    = nullptr;   // кому отдавать сообщения llama.cpp

static void llamaLogCallback(ggml_log_level level, const char *text, void * /*userdata*/)
{
    if (!text || text[0] == '\0') return;

    // Убираем trailing newline — LoggerModule добавляет свой
    QString msg = QString::fromUtf8(text).trimmed();
    if (msg.isEmpty()) return;

    QMutexLocker lock(&s_globalMutex);
    LlamaClient *client = s_logOwner;
    if (!client) {
        if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
            qWarning().noquote() << "[llama.cpp]" << msg;
        return;
    }

    switch (level) {
    case GGML_LOG_LEVEL_ERROR:
        client->log(Logger::kLevelError, "[llama.cpp] " + msg);
        break;
    case GGML_LOG_LEVEL_WARN:
        // Контекст задан меньше обучающего намеренно (экономия памяти) — это
        // штатная ситуация, не предупреждение
        client->log(msg.contains(QLatin1String("n_ctx_per_seq")) ? Logger::kLevelDebug
                                                                  : Logger::kLevelWarning,
                    "[llama.cpp] " + msg);
        break;
    default:
        // Пишем всё что касается инициализации железа и памяти.
        // "CUDA Graph reused" и прочий рантайм-шум не проходит ни по одному паттерну.
        if (msg.contains("llm_load",        Qt::CaseInsensitive) ||
            msg.contains("metal",           Qt::CaseInsensitive) ||
            msg.contains("vulkan",          Qt::CaseInsensitive) ||
            msg.contains("offload",         Qt::CaseInsensitive) ||
            msg.contains("kv cache",        Qt::CaseInsensitive) ||
            msg.contains("ggml_cuda_init",  Qt::CaseInsensitive) ||
            msg.contains("model buffer",    Qt::CaseInsensitive) ||
            msg.contains("using device",    Qt::CaseInsensitive) ||
            msg.contains("compute cap",     Qt::CaseInsensitive) ||
            msg.contains("driver version",  Qt::CaseInsensitive) ||
            msg.contains("cuda version",    Qt::CaseInsensitive) ||
            msg.contains("cudart",          Qt::CaseInsensitive) ||
            msg.contains("build info",      Qt::CaseInsensitive) ||
            msg.contains("n_ctx",           Qt::CaseInsensitive) ||
            msg.contains("n_batch",         Qt::CaseInsensitive) ||
            msg.contains("abort",           Qt::CaseInsensitive) ||
            msg.contains("error",           Qt::CaseInsensitive) ||
            msg.contains("fail",            Qt::CaseInsensitive))
        {
            client->log(Logger::kLevelDebug, "[llama.cpp] " + msg);
        }
        break;
    }
}

// ── LlamaClient ───────────────────────────────────────────────────────────────

LlamaClient::LlamaClient(QObject *parent)
    : QObject(parent)
{
    // CUDA Graphs (USE_GRAPHS=1 в этом билде) вызывают hard crash при warmup
    // на некоторых конфигурациях (RTX 3050 + определённые версии драйвера).
    // Отключаем через env до llama_backend_init — llama.cpp читает эту переменную
    // при инициализации CUDA бэкенда.
    qputenv("GGML_CUDA_NO_GRAPHS", "1");

    QMutexLocker lock(&s_globalMutex);
    if (s_backendRefs++ == 0) {
        llama_backend_init();
        llama_log_set(llamaLogCallback, nullptr);
    }
}

LlamaClient::~LlamaClient()
{
    shutdown();
    QMutexLocker lock(&s_globalMutex);
    if (s_logOwner == this)
        s_logOwner = nullptr;
    if (--s_backendRefs == 0)
        llama_backend_free();
}

void LlamaClient::log(const char *level, const QString &message)
{
    // Потокобезопасно; один путь записи (раньше — дважды: qDebug и шина)
    Logger::write(level, "LlamaClient", message);
}

bool LlamaClient::initialize(const QString &modelPath)
{
    if (m_ready) return true;

    log(Logger::kLevelInfo, "initialize() start, model path: " + modelPath);

    // Сообщения llama.cpp идут в журнал клиента, который сейчас загружает модель
    {
        QMutexLocker lock(&s_globalMutex);
        s_logOwner = this;
    }

    // Версия llama.cpp и системная информация — критично для диагностики на клиенте
    log(Logger::kLevelInfo,
        QString("llama.cpp build: %1").arg(llama_print_system_info()));

    if (m_abort.load()) return false;

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = kGpuLayers;
    // Загрузка модели (несколько секунд) прерывается по requestAbort()
    mparams.progress_callback = [](float, void *ud) -> bool {
        return !static_cast<LlamaClient *>(ud)->isAborted();
    };
    mparams.progress_callback_user_data = this;

    log(Logger::kLevelInfo,
        QString("Loading model with n_gpu_layers=%1").arg(kGpuLayers));

    m_model = llama_model_load_from_file(modelPath.toUtf8().constData(), mparams);
    if (!m_model && m_abort.load()) {
        log(Logger::kLevelInfo, "Model loading aborted");
        return false;
    }
    if (!m_model) {
        log(Logger::kLevelWarning,
            "GPU load failed (or not available), retrying CPU-only (n_gpu_layers=0)");
        mparams.n_gpu_layers = 0;
        m_model = llama_model_load_from_file(modelPath.toUtf8().constData(), mparams);
    }
    if (!m_model) {
        log(Logger::kLevelError,
            "Failed to load model from: " + modelPath +
            " — check that the file exists and is a valid GGUF");
        return false;
    }

    log(Logger::kLevelInfo, "Model loaded successfully");

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx       = kCtxSize;
    cparams.n_batch     = 512;
    cparams.offload_kqv = true; // KV cache на GPU — всегда, llama.cpp сам упадёт с ошибкой если не хватит VRAM

    log(Logger::kLevelInfo,
        QString("Creating context: n_ctx=%1, n_batch=%2, offload_kqv=true")
            .arg(kCtxSize).arg(cparams.n_batch));

    m_ctx = llama_init_from_model(m_model, cparams);
    if (!m_ctx) {
        log(Logger::kLevelError,
            "Failed to create llama context — not enough VRAM/RAM");
        llama_model_free(m_model);
        m_model = nullptr;
        return false;
    }

    m_ready = true;
    log(Logger::kLevelInfo, "LlamaClient ready");
    return true;
}

void LlamaClient::shutdown()
{
    if (!m_ready && !m_ctx && !m_model) return;

    log(Logger::kLevelInfo, "shutdown()");
    if (m_ctx)   { llama_free(m_ctx);        m_ctx   = nullptr; }
    if (m_model) { llama_model_free(m_model); m_model = nullptr; }
    m_ready = false;
}

QString LlamaClient::runInference(const QString &systemPrompt, const QString &userPrompt,
                                   const TokenCallback &onToken)
{
    if (!m_ready) {
        log(Logger::kLevelError, "runInference() called but client is not ready");
        return {};
    }
    if (m_abort.load()) {
        log(Logger::kLevelInfo, "runInference(): aborted before start");
        return {};
    }
    // KV-кэш мог остаться заполненным после прерванного/неудачного вызова
    llama_kv_self_clear(m_ctx);

    // Универсальный chat template из метаданных GGUF.
    // QByteArray держим живыми до вызова llama_chat_apply_template —
    // llama_chat_message хранит const char*, поэтому временные объекты недопустимы.
    const QByteArray systemUtf8 = systemPrompt.toUtf8();
    const QByteArray userUtf8   = userPrompt.toUtf8();

    const char *tmpl = llama_model_chat_template(m_model, nullptr);

    std::vector<llama_chat_message> messages = {
        {"system", systemUtf8.constData()},
        {"user",   userUtf8.constData()}
    };

    std::vector<char> tmplBuf(65536);
    int tmplLen = llama_chat_apply_template(
        tmpl,
        messages.data(), messages.size(),
        true,   // add_ass — добавить начало assistant turn
        tmplBuf.data(), static_cast<int>(tmplBuf.size())
    );
    if (tmplLen > static_cast<int>(tmplBuf.size())) {
        tmplBuf.resize(tmplLen);
        llama_chat_apply_template(
            tmpl,
            messages.data(), messages.size(),
            true,
            tmplBuf.data(), static_cast<int>(tmplBuf.size())
        );
    }
    const QString chatPrompt = QString::fromUtf8(tmplBuf.data(), tmplLen);

    const QByteArray promptBytes = chatPrompt.toUtf8();
    const llama_vocab *vocab = llama_model_get_vocab(m_model);

    // Токенизация
    std::vector<llama_token> tokens(kCtxSize);
    int nTokens = llama_tokenize(
        vocab,
        promptBytes.constData(), promptBytes.size(),
        tokens.data(), static_cast<int>(tokens.size()),
        true, true);

    if (nTokens < 0) {
        tokens.resize(-nTokens);
        nTokens = llama_tokenize(
            vocab,
            promptBytes.constData(), promptBytes.size(),
            tokens.data(), static_cast<int>(tokens.size()),
            true, true);
        if (nTokens < 0) {
            log(Logger::kLevelError,
                QString("Tokenize failed even after resize (returned %1)").arg(nTokens));
            return {};
        }
    }

    log(Logger::kLevelInfo,
        QString("Prompt tokenized: %1 tokens (ctx limit: %2, max output: %3)")
            .arg(nTokens).arg(kCtxSize).arg(kMaxTokens));

    if (nTokens > kCtxSize - kMaxTokens) {
        // Раньше отрезался КОНЕЦ промпта — вместе с заголовком реплики ассистента
        // (<|im_start|>assistant), и модель получала «битый» диалог. Теперь
        // вырезаем середину (хвост источников), сохраняя начало и конец.
        const int limit = kCtxSize - kMaxTokens;
        const int keepTail = 256;
        const int keepHead = limit - keepTail;
        log(Logger::kLevelWarning,
            QString("Prompt truncated: %1 → %2 tokens (would overflow context)")
                .arg(nTokens).arg(limit));
        std::copy(tokens.begin() + (nTokens - keepTail), tokens.begin() + nTokens,
                  tokens.begin() + keepHead);
        nTokens = limit;
    }
    tokens.resize(nTokens);

    // Decode prompt батчами
    const int n_batch    = 512;
    int       n_processed = 0;
    while (n_processed < nTokens) {
        if (m_abort.load()) {
            log(Logger::kLevelInfo, "runInference(): aborted during prompt decode");
            llama_kv_self_clear(m_ctx);
            return {};
        }
        const int n_this = std::min(n_batch, nTokens - n_processed);
        llama_batch batch = llama_batch_get_one(tokens.data() + n_processed, n_this);
        if (llama_decode(m_ctx, batch) != 0) {
            log(Logger::kLevelError,
                QString("Prompt decode failed at offset %1/%2")
                    .arg(n_processed).arg(nTokens));
            return {};
        }
        n_processed += n_this;
    }

    log(Logger::kLevelInfo, "Prompt decoded, starting token generation...");

    // Сэмплер
    const llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler *sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.4f));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));

    QString result;
    int generatedTokens = 0;

    // Токен BPE может содержать только ЧАСТЬ UTF-8 символа (кириллица — 2 байта).
    // Раньше каждый токен декодировался отдельно → в русских описаниях
    // появлялись символы «�». Копим байты и отдаём только завершённые символы.
    QByteArray pendingBytes;
    auto completeUtf8Prefix = [](const QByteArray &b) -> int {
        int i = b.size();
        // Ищем начало последнего символа (не continuation-байт 10xxxxxx)
        int back = 0;
        while (back < 4 && i - back - 1 >= 0 && (uchar(b[i - back - 1]) & 0xC0) == 0x80) ++back;
        if (i - back - 1 < 0) return i;           // одни continuation-байты — отдаём как есть
        const uchar lead = uchar(b[i - back - 1]);
        int need = 1;
        if      ((lead & 0xE0) == 0xC0) need = 2;
        else if ((lead & 0xF0) == 0xE0) need = 3;
        else if ((lead & 0xF8) == 0xF0) need = 4;
        return (back + 1 >= need) ? i : i - back - 1;   // последний символ неполный — оставляем
    };

    for (int i = 0; i < kMaxTokens; ++i) {
        if (m_abort.load()) {
            log(Logger::kLevelInfo, "runInference(): aborted during generation");
            break;
        }
        const llama_token newToken = llama_sampler_sample(sampler, m_ctx, -1);

        // EOG покрывает и eos, и <|im_end|>/<|eot_id|> разных шаблонов
        if (llama_vocab_is_eog(vocab, newToken)) {
            log(Logger::kLevelInfo,
                QString("End-of-generation token reached after %1 generated tokens").arg(generatedTokens));
            break;
        }

        char buf[256];
        const int n = llama_token_to_piece(vocab, newToken, buf, sizeof(buf), 0, false);
        if (n > 0) pendingBytes.append(buf, n);

        const int ready = completeUtf8Prefix(pendingBytes);
        const QString piece = QString::fromUtf8(pendingBytes.constData(), ready);
        pendingBytes.remove(0, ready);

        if (piece.contains("<|im_end|>")) {
            log(Logger::kLevelInfo,
                QString("im_end stop token reached after %1 generated tokens").arg(generatedTokens));
            break;
        }

        if (!piece.isEmpty()) {
            result += piece;
            if (onToken) onToken(piece);
        }
        ++generatedTokens;

        llama_token tok = newToken;
        llama_batch nb  = llama_batch_get_one(&tok, 1);
        if (llama_decode(m_ctx, nb) != 0) {
            log(Logger::kLevelError,
                QString("Decode failed during generation at token %1").arg(generatedTokens));
            break;
        }
    }

    if (generatedTokens == kMaxTokens) {
        log(Logger::kLevelWarning,
            QString("Generation hit kMaxTokens limit (%1) without stop token — "
                    "response may be truncated").arg(kMaxTokens));
    }

    // Остаток байт (если генерация оборвалась посреди символа)
    if (!pendingBytes.isEmpty()) {
        const QString tail = QString::fromUtf8(pendingBytes);
        result += tail;
        if (onToken) onToken(tail);
    }

    llama_sampler_free(sampler);
    llama_kv_self_clear(m_ctx);

    const QString trimmed = result.trimmed();

    log(Logger::kLevelInfo,
        QString("Inference complete: %1 tokens generated, response length: %2 chars")
            .arg(generatedTokens).arg(trimmed.length()));

    if (trimmed.isEmpty()) {
        log(Logger::kLevelWarning, "Model returned an empty response");
    }

    return trimmed;
}

