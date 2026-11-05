// modules/KaraokeModule/SongNameResolver.cpp
#include "SongNameResolver.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

#include "tools/LlamaClient.h"

SongNameResolver::SongNameResolver(EventBus *bus) : m_bus(bus) {}

SongNameResolver::~SongNameResolver()
{
    // ~LlamaClient вызывает llama_backend_free — безопасно, пока модули,
    // держащие свои клиенты, завершаются раньше (см. порядок shutdown в main.cpp).
    if (LlamaClient *llama = m_llama.exchange(nullptr)) {
        llama->shutdown();
        delete llama;
    }
}

QString SongNameResolver::modelPath()
{
    // Общая настройка приложения; путь по умолчанию совпадает с остальными модулями
    const QString fromSettings = QSettings("MediaStation", "MediaStation")
                                     .value("llamaModelPath").toString();
    return fromSettings.isEmpty()
        ? QCoreApplication::applicationDirPath()
              + "/tools/llama/models/qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf"
        : fromSettings;
}

bool SongNameResolver::modelAvailable() { return QFile::exists(modelPath()); }

void SongNameResolver::abort()
{
    if (LlamaClient *llama = m_llama.load())
        llama->requestAbort();
}

// ─────────────────────────────────────────────────────────────────────────────
// Промпт
// ─────────────────────────────────────────────────────────────────────────────

QString SongNameResolver::systemPrompt()
{
    return QStringLiteral(
        "Ты — аккуратный редактор музыкальных тегов. По имени аудиофайла определи "
        "исполнителя и название песни.\n"
        "Ответ — ТОЛЬКО JSON в одну строку: {\"artist\":\"...\",\"title\":\"...\"}. "
        "Без пояснений и markdown.\n\n"
        "Правила:\n"
        "1. Название берётся ИЗ ИМЕНИ ФАЙЛА. Не придумывай и не добавляй слов, "
        "которых в имени нет.\n"
        "2. Убирай служебный мусор, не являющийся частью названия: номер трека, ID, "
        "адрес сайта, расширение, «Official Video», «Lyrics», «HQ», «скачать», битрейт.\n"
        "3. Год или дату убирай, если это пометка версии или загрузки: «2025», «(2021)», "
        "«Remastered 2011», «Live 1991». Но числа, которые входят в настоящее название "
        "песни, ОСТАВЛЯЙ: «1979», «99 Luftballons», «2 Become 1», «Blink-182».\n"
        "4. Подчёркивания замени пробелами, верни нормальный регистр, оригинальный "
        "язык и написание.\n"
        "5. Исполнитель — из имени файла. Если его там нет, а песню ты уверенно знаешь, "
        "укажи исполнителя; если не уверен — пустая строка.\n"
        "6. Если название определить нельзя — оба поля пустые.");
}

QString SongNameResolver::userPrompt(const QString &fileName)
{
    return QStringLiteral(
        "Примеры:\n"
        "Имя файла: Nirvana_-_Smells_Like_Teen_Spirit_47829249.mp3\n"
        "{\"artist\":\"Nirvana\",\"title\":\"Smells Like Teen Spirit\"}\n"
        "Имя файла: Кино - Апрель 2025.mp3\n"
        "{\"artist\":\"Кино\",\"title\":\"Апрель\"}\n"
        "Имя файла: The Smashing Pumpkins - 1979.mp3\n"
        "{\"artist\":\"The Smashing Pumpkins\",\"title\":\"1979\"}\n"
        "Имя файла: Nena - 99 Luftballons (Remastered 2013).mp3\n"
        "{\"artist\":\"Nena\",\"title\":\"99 Luftballons\"}\n"
        "Имя файла: zaycev.net_Кино - Группа крови.mp3\n"
        "{\"artist\":\"Кино\",\"title\":\"Группа крови\"}\n"
        "Имя файла: Dance Monkey.mp3\n"
        "{\"artist\":\"Tones and I\",\"title\":\"Dance Monkey\"}\n"
        "Имя файла: 01 track.mp3\n"
        "{\"artist\":\"\",\"title\":\"\"}\n\n"
        "Теперь определи:\n"
        "Имя файла: ") + fileName;
}

// ─────────────────────────────────────────────────────────────────────────────
// Разбор и проверка ответа
// ─────────────────────────────────────────────────────────────────────────────

SongNameResolver::Result SongNameResolver::parseAnswer(const QString &answer)
{
    Result r;
    const int a = answer.indexOf(QLatin1Char('{'));
    const int b = answer.lastIndexOf(QLatin1Char('}'));
    if (a < 0 || b <= a) return r;

    const QJsonObject o = QJsonDocument::fromJson(answer.mid(a, b - a + 1).toUtf8()).object();
    r.artist = o.value("artist").toString().simplified().left(120);
    r.title  = o.value("title").toString().simplified().left(120);
    return r;
}

QString SongNameResolver::stripTrailingYear(const QString &title)
{
    static const QRegularExpression kYearRe(
        QStringLiteral("^(.*\\p{L}.*?)[\\s]*[\\(\\[]?\\s*(?:19|20)\\d{2}\\s*[\\)\\]]?$"));
    const QRegularExpressionMatch m = kYearRe.match(title.trimmed());
    if (!m.hasMatch()) return title;
    const QString rest = m.captured(1).trimmed();
    return rest.isEmpty() ? title : rest;
}

static QSet<QString> tokenSet(const QString &text)
{
    QSet<QString> out;
    static const QRegularExpression kSplit(QStringLiteral("[^\\p{L}\\p{N}]+"));
    for (const QString &t : text.toLower().replace(QChar(0x0451), QChar(0x0435))
                                .split(kSplit, Qt::SkipEmptyParts))
        out.insert(t);
    return out;
}

bool SongNameResolver::titleMatchesFile(const QString &title, const QString &fileStem)
{
    const QSet<QString> have = tokenSet(fileStem);
    const QSet<QString> want = tokenSet(title);
    if (want.isEmpty()) return false;
    for (const QString &w : want)
        if (!have.contains(w)) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolve
// ─────────────────────────────────────────────────────────────────────────────

SongNameResolver::Result SongNameResolver::resolve(const QString &fileName,
                                                   const TokenCb &onToken,
                                                   const StatusCb &onStatus)
{
    Result r;
    auto status = [&](const QString &m) { if (onStatus) onStatus(m); };

    if (!modelAvailable()) {
        r.message = QStringLiteral("Модель нейросети не найдена — использую имя файла");
        return r;
    }

    status(QStringLiteral("Загружаю нейросеть…"));
    LlamaClient *llama = m_llama.load();
    if (!llama) {
        llama = new LlamaClient();
        llama->setBus(m_bus);
        m_llama.store(llama);
    }
    llama->resetAbort();   // новая задача; abort() после этой точки сработает
    if (!llama->initialize(modelPath())) {
        r.message = QStringLiteral("Не удалось загрузить нейросеть — использую имя файла");
        return r;
    }

    status(QStringLiteral("Нейросеть определяет исполнителя и название…"));
    const QString answer = llama->runInference(systemPrompt(), userPrompt(fileName), onToken);

    // Освобождаем VRAM сразу — дальше Demucs и Whisper
    llama->shutdown();
    qDebug() << "[SongNameResolver] ответ:" << answer.left(200);

    r = parseAnswer(answer);

    // Название — только из слов имени файла (нейросеть не должна ничего дописывать)
    const QString stem = QFileInfo(fileName).completeBaseName().replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!r.title.isEmpty() && !titleMatchesFile(r.title, stem)) {
        qWarning() << "[SongNameResolver] название не из имени файла, отброшено:" << r.title;
        r.title.clear();
        r.message = QStringLiteral("Нейросеть предложила название, которого нет в имени файла — проверьте вручную");
    }
    return r;
}
