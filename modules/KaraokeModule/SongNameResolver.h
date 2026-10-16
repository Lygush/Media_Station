// modules/KaraokeModule/SongNameResolver.h
// ─────────────────────────────────────────────────────────────────────────────
// SongNameResolver — определяет «Исполнитель» и «Название» по имени аудиофайла
// с помощью локальной LLM (общий инструмент tools/LlamaClient).
//
// Логика промпта, разбора ответа и проверки результата принадлежит модулю
// Karaoke. Чужие модули (GameInfoModule и др.) не используются: у каждого
// модуля свой экземпляр LlamaClient и своя логика поверх него.
//
// Модель загружается на время запроса и выгружается сразу после ответа —
// видеопамять освобождается для Demucs и Whisper.
//
// resolve() блокирующий — вызывать из фонового потока.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <QString>
#include <atomic>
#include <functional>

class EventBus;
class LlamaClient;

class SongNameResolver
{
public:
    struct Result {
        QString artist;
        QString title;
        QString message;      ///< пояснение для пользователя, если нейросеть не помогла
    };

    using TokenCb  = std::function<void(const QString &)>;
    using StatusCb = std::function<void(const QString &)>;

    explicit SongNameResolver(EventBus *bus);
    ~SongNameResolver();

    static QString modelPath();
    static bool    modelAvailable();

    /// fileName — имя файла (с расширением). Пустой title в результате означает
    /// «нейросети не доверяем» (нет модели, ошибка, ответ не прошёл проверку).
    Result resolve(const QString &fileName, const TokenCb &onToken, const StatusCb &onStatus);

    /// Прервать идущий запрос (из другого потока).
    void abort();

    // ── Чистые функции (без модели) — вынесены для тестирования ──────────────
    static QString systemPrompt();
    static QString userPrompt(const QString &fileName);
    static Result  parseAnswer(const QString &answer);

    /// Убирает ТОЛЬКО завершающий год (1900–2099, в скобках или без) после других слов:
    /// «Апрель 2025» → «Апрель», «Song (2021)» → «Song». Название из одного числа
    /// («1979») и числа внутри названия («99 Luftballons») не трогает.
    static QString stripTrailingYear(const QString &title);

    /// Название должно состоять из слов, которые есть в имени файла: нейросеть
    /// может убрать мусор, но не имеет права добавлять слова (например год).
    static bool titleMatchesFile(const QString &title, const QString &fileStem);

private:
    EventBus                  *m_bus;
    std::atomic<LlamaClient *> m_llama { nullptr };   // создаётся лениво
};
