// modules/GameInfoModule/GameInfoModule.cpp
#include "tools/LocalModelPaths.h"
#include "GameInfoModule.h"
#include "GameInfoFetcher.h"
#include "tools/LlamaClient.h"
#include "SteamAppList.h"
#include "core/EventBus.h"
#include "modules/SettingsModule/SettingsManager.h"
#include "modules/NotificationModule/NotificationModule.h"
#include "tools/AtomicFile.h"
#include "tools/GameInfoStore.h"
#include "LoggerModule.h"

#include <QtConcurrent>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QDeadlineTimer>

// ── вспомогательный макрос ────────────────────────────────────────────────────
// Отправляет сообщение в LoggerModule через EventBus.
// Безопасно из любого потока (QueuedConnection внутри log()).
#define GIM_LOG(level, msg) logMsg(level, msg)

GameInfoModule::GameInfoModule(QObject *parent)
    : QObject(parent)
{}

GameInfoModule::~GameInfoModule()
{
    shutdown();
}

// ── private helper ────────────────────────────────────────────────────────────

void GameInfoModule::logMsg(const char *level, const QString &message)
{
    // Один путь записи (консоль + файл); раньше каждое сообщение уходило
    // дважды — в qDebug/qWarning и через шину.
    Logger::write(level, "GameInfoModule", message);
}

// ── IModule ───────────────────────────────────────────────────────────────────

void GameInfoModule::initialize(EventBus *bus, SettingsManager *settings)
{
    m_bus      = bus;
    m_settings = settings;

    connect(bus, &EventBus::moduleEvent, this,
        [this](const QString &mod, const QString &ev, const QVariantMap &data) {
            if (mod == "gameinfo" && ev == "fetch")
                onFetchRequested(data);
        });

    GIM_LOG(Logger::kLevelInfo, "initialized");
}

void GameInfoModule::shutdown()
{
    // Разбудить фоновый поток если он заблокирован на ожидании выбора кандидата
    {
        QMutexLocker locker(&m_candidateMutex);
        m_aborted = true;
        m_candidateCond.wakeAll();
        // m_llama создаётся фоновым потоком под этим же мьютексом —
        // читаем его здесь же, без гонки.
        if (m_llama) m_llama->requestAbort();
    }

    // ДОЖИДАЕМСЯ фонового потока: раньше m_llama удалялся, пока doFetch
    // ещё работал с ним (падение при выходе во время генерации).
    if (m_fetchFuture.isRunning()) {
        GIM_LOG(Logger::kLevelInfo, "shutdown(): waiting for fetch thread");
        m_fetchFuture.waitForFinished();
    }

    if (m_llama) {
        GIM_LOG(Logger::kLevelInfo, "shutdown(): releasing LlamaClient");
        m_llama->shutdown();
        delete m_llama;
        m_llama = nullptr;
    }
}

void GameInfoModule::setStage(const QString &stage)
{
    if (m_stage == stage) return;
    m_stage = stage;
    emit stageChanged();
}

// ── public slots ──────────────────────────────────────────────────────────────

void GameInfoModule::requestFetch(const QString &tileId,
                                   const QString &title,
                                   const QString &installFolder)
{
    onFetchRequested({
        {"tileId",        tileId},
        {"title",         title},
        {"installFolder", installFolder}
    });
}

void GameInfoModule::onFetchRequested(const QVariantMap &data)
{
    if (m_fetching) {
        GIM_LOG(Logger::kLevelWarning, "already fetching, ignoring new request");
        // Раньше запрос молча пропадал: нажатие на другой игре «ничего не делало»
        if (m_bus)
            emit m_bus->moduleEvent(
                QLatin1String(Notifications::kModule), QLatin1String(Notifications::kEventShow),
                Notifications::makeEvent(
                    QStringLiteral("Нейросеть занята описанием другой игры — повторите позже"),
                    NotificationLevel::Warning, true, 5000));
        return;
    }

    const QString tileId        = data["tileId"].toString();
    const QString title         = data["title"].toString();
    const QString installFolder = data["installFolder"].toString();

    if (title.isEmpty() || installFolder.isEmpty()) {
        GIM_LOG(Logger::kLevelError,
                QString("fetch requested with empty title or installFolder "
                        "(tileId=%1)").arg(tileId));
        return;
    }

    GIM_LOG(Logger::kLevelInfo,
            QString("fetch requested: tileId=%1, title=\"%2\", folder=%3")
                .arg(tileId, title, installFolder));

    m_fetching = true;
    emit fetchingChanged();

    {
        QMutexLocker locker(&m_candidateMutex);
        m_pendingTileId = tileId;   // fieldsReady и т.п. адресуются этой плитке
        m_selectedAppId.clear();
    }

    m_fetchFuture = QtConcurrent::run([this, tileId, title, installFolder]() {
        doFetch(tileId, title, installFolder);
    });
}

// ── doFetch (background thread) ───────────────────────────────────────────────

void GameInfoModule::doFetch(const QString &tileId,
                              const QString &title,
                              const QString &installFolder)
{
    GIM_LOG(Logger::kLevelInfo,
            QString("doFetch start: \"%1\" (tileId=%2)").arg(title, tileId));

    // 0. Загружаем SteamAppList ------------------------------------------------
    if (!SteamAppList::instance().isLoaded()) {
        GIM_LOG(Logger::kLevelInfo, "SteamAppList not loaded yet, loading...");
        QMetaObject::invokeMethod(this, [this]() { setStage("db"); }, Qt::QueuedConnection);

        SteamAppList::instance().loadIfNeeded();
        SteamAppList::instance().waitUntilLoaded();

        if (SteamAppList::instance().isLoaded())
            GIM_LOG(Logger::kLevelInfo, "SteamAppList loaded successfully");
        else
            GIM_LOG(Logger::kLevelWarning, "SteamAppList failed to load or was cancelled");
    }

    // 1. Поиск appId -----------------------------------------------------------
    QString chosenAppId;
    if (SteamAppList::instance().isLoaded()) {
        const auto candidates = SteamAppList::instance().findTopCandidates(title, 8);

        if (candidates.isEmpty()) {
            GIM_LOG(Logger::kLevelInfo,
                    QString("SteamAppList: no candidates found for \"%1\"").arg(title));
        } else {
            const double topScore = candidates.first().score;
            GIM_LOG(Logger::kLevelInfo,
                    QString("SteamAppList: top candidate \"%1\" (appId=%2, score=%3)")
                        .arg(candidates.first().name)
                        .arg(candidates.first().appid)
                        .arg(topScore, 0, 'f', 3));

            if (topScore >= 0.99 && candidates.size() == 1) {
                chosenAppId = QString::number(candidates.first().appid);
                GIM_LOG(Logger::kLevelInfo,
                        QString("auto-selected appId=%1 (exact match)").arg(chosenAppId));
            } else {
                GIM_LOG(Logger::kLevelInfo,
                        QString("ambiguous match (%1 candidates), waiting for user selection")
                            .arg(candidates.size()));

                QVariantList qmlCandidates;
                for (const auto &c : candidates) {
                    QVariantMap m;
                    m["appId"] = QString::number(c.appid);
                    m["name"]  = c.name;
                    m["score"] = c.score;
                    qmlCandidates.append(m);
                }

                {
                    QMutexLocker locker(&m_candidateMutex);
                    m_selectedAppId.clear();
                    m_pendingTileId = tileId;
                }

                QMetaObject::invokeMethod(this, [this, tileId, qmlCandidates]() {
                    setStage("db_select");
                    emit candidatesReady(tileId, qmlCandidates);
                }, Qt::QueuedConnection);

                {
                    // Не ждём вечно: если пользователь ушёл со страницы игры, раньше
                    // поток висел навсегда и все следующие запросы отклонялись
                    // («already fetching»). Через 5 минут — как «Пропустить».
                    QMutexLocker locker(&m_candidateMutex);
                    QDeadlineTimer deadline(5 * 60 * 1000);
                    while (m_selectedAppId.isEmpty() && m_pendingTileId == tileId && !m_aborted) {
                        if (!m_candidateCond.wait(&m_candidateMutex, deadline)) {
                            m_selectedAppId = QStringLiteral("__skip__");
                            break;
                        }
                    }
                    if (m_aborted) {
                        GIM_LOG(Logger::kLevelInfo, "doFetch: aborted while waiting for candidate selection");
                        return;
                    }
                    // __skip__ означает что пользователь нажал "Пропустить"
                    chosenAppId = (m_selectedAppId == QLatin1String("__skip__"))
                                  ? QString() : m_selectedAppId;
                }

                if (chosenAppId.isEmpty())
                    GIM_LOG(Logger::kLevelWarning,
                            "user did not select an appId (cancelled or skipped)");
                else
                    GIM_LOG(Logger::kLevelInfo,
                            QString("user selected appId=%1").arg(chosenAppId));

                QMetaObject::invokeMethod(this, [this]() {
                    setStage("fetching");
                }, Qt::QueuedConnection);
            }
        }
    }

    // 2. Сбор текста из сети ---------------------------------------------------
    GIM_LOG(Logger::kLevelInfo, "starting web fetch...");
    QMetaObject::invokeMethod(this, [this]() { setStage("fetching"); }, Qt::QueuedConnection);

    GameInfoFetcher fetcher;
    const auto    fetchResult = fetcher.fetch(title, installFolder, chosenAppId);
    const QString sourceText  = fetchResult.sourceText;
    const QString steamAppId  = chosenAppId.isEmpty() ? fetchResult.steamAppId : chosenAppId;

    GIM_LOG(Logger::kLevelInfo,
            QString("web fetch done: sourceText=%1 chars, steamAppId=\"%2\"")
                .arg(sourceText.length()).arg(steamAppId));

    if (sourceText.isEmpty())
        GIM_LOG(Logger::kLevelWarning,
                "no source text from fetcher — LLM will use its own knowledge");

    // 3. Инициализация LlamaClient ---------------------------------------------
    GIM_LOG(Logger::kLevelInfo, "preparing LlamaClient...");
    QMetaObject::invokeMethod(this, [this]() { setStage("generating"); }, Qt::QueuedConnection);

    // Создание и сброс флага — под мьютексом, которым shutdown() выставляет
    // m_aborted и вызывает requestAbort(): после этой точки выход из программы
    // гарантированно прервёт загрузку модели и генерацию.
    {
        QMutexLocker locker(&m_candidateMutex);
        if (m_aborted) return;
        if (!m_llama) {
            m_llama = new LlamaClient();
            m_llama->setBus(m_bus); // пробрасываем шину ДО initialize()
        }
        m_llama->resetAbort();
    }

    // Модель выгружается после каждой генерации (см. ниже), поэтому
    // загружаем её заново, если объект есть, но модель не в памяти.
    if (!m_llama->isReady()) {

        const QString resolvedPath = LocalModels::llamaModelPath();

        GIM_LOG(Logger::kLevelInfo,
                QString("LlamaClient: resolved path: \"%1\"").arg(resolvedPath));

        // Проверяем существование файла ДО инициализации — частая причина проблем
        if (!QFile::exists(resolvedPath)) {
            GIM_LOG(Logger::kLevelError,
                    "LlamaClient: model file NOT FOUND at: " + resolvedPath);
        }

        if (!m_llama->initialize(resolvedPath)) {
            GIM_LOG(Logger::kLevelError,
                    "LlamaClient: initialize() failed — see above for details");
            // Объект не удаляем: он может читаться в shutdown(); следующий
            // запрос просто попробует загрузить модель заново.

            QMetaObject::invokeMethod(this, [this, tileId]() {
                m_fetching = false;
                emit fetchingChanged();
                setStage("");
                notifyUser("Не удалось загрузить модель LLM", true);
                emit fetchError(tileId, "model_load_failed");
            }, Qt::QueuedConnection);
            return;
        }
    }

    // 4. Генерация метаданных --------------------------------------------------
    GIM_LOG(Logger::kLevelInfo,
            QString("calling LlamaClient::generate() for \"%1\"").arg(title));

    // Колбэк вызывается из потока генерации на каждый токен описания.
    // QueuedConnection передаёт токен в GUI поток.
    const LlamaClient::TokenCallback onToken = [this](const QString &token) {
        QMetaObject::invokeMethod(this, [this, token]() {
            emit tokenGenerated(token);
        }, Qt::QueuedConnection);
    };

    // Колбэк для первого запроса (JSON) — для отображения в попапе.
    const LlamaClient::TokenCallback onJsonToken = [this](const QString &token) {
        QMetaObject::invokeMethod(this, [this, token]() {
            emit jsonTokenGenerated(token);
        }, Qt::QueuedConnection);
    };

    const QVariantMap meta = generateGameInfo(title, sourceText, onToken, onJsonToken);

    // Освобождаем видеопамять сразу после генерации. Раньше модель (~4.5 ГБ)
    // оставалась в VRAM до выхода из программы, и запущенные после этого игры
    // получали заметно меньше видеопамяти. Описание игры запрашивается редко,
    // повторная загрузка модели занимает несколько секунд.
    m_llama->shutdown();

    {
        QMutexLocker locker(&m_candidateMutex);
        if (m_aborted) return;   // приложение закрывается
    }

    if (meta.isEmpty()) {
        GIM_LOG(Logger::kLevelError,
                QString("LLM returned no data for \"%1\"").arg(title));
        QMetaObject::invokeMethod(this, [this, tileId]() {
            m_fetching = false;
            emit fetchingChanged();
            setStage("");
            notifyUser("LLM не вернула данные об игре", true);
            emit fetchError(tileId, "llm_empty_response");
        }, Qt::QueuedConnection);
        return;
    }

    GIM_LOG(Logger::kLevelInfo,
            QString("LLM generated metadata for \"%1\" (%2 fields)")
                .arg(title).arg(meta.size()));

    // 5. Сохранение ------------------------------------------------------------
    saveToGameInfo(installFolder, meta, steamAppId);
    GIM_LOG(Logger::kLevelInfo,
            QString("metadata saved to %1/game_info.json").arg(installFolder));

    // 6. Уведомляем GUI --------------------------------------------------------
    QVariantMap metaWithId = meta;
    if (!steamAppId.isEmpty())
        metaWithId["steamAppId"] = steamAppId;

    QMetaObject::invokeMethod(this, [this, tileId, metaWithId]() {
        m_fetching = false;
        emit fetchingChanged();
        setStage("");
        emit infoFetched(tileId, metaWithId);
        notifyUser("Информация об игре получена");

        if (m_bus)
            emit m_bus->moduleEvent("gameinfo", "fetched",
                {{"tileId", tileId}, {"success", true}});
    }, Qt::QueuedConnection);

    GIM_LOG(Logger::kLevelInfo,
            QString("doFetch complete for \"%1\" (tileId=%2)").arg(title, tileId));
}

// ── selectCandidate ───────────────────────────────────────────────────────────

void GameInfoModule::selectCandidate(const QString &tileId, const QString &appId)
{
    QMutexLocker locker(&m_candidateMutex);
    if (m_pendingTileId != tileId) {
        GIM_LOG(Logger::kLevelWarning,
                QString("selectCandidate: tileId mismatch "
                        "(expected=%1, got=%2), ignoring")
                    .arg(m_pendingTileId, tileId));
        return;
    }
    GIM_LOG(Logger::kLevelInfo,
            QString("selectCandidate: appId=%1 for tileId=%2").arg(appId, tileId));
    // Если пользователь пропустил (appId пустой) — ставим sentinel чтобы выйти из wait.
    // Пустая строка не выходит из цикла (условие isEmpty() остаётся true).
    m_selectedAppId = appId.isEmpty() ? QStringLiteral("__skip__") : appId;
    m_candidateCond.wakeAll();
}

// ── saveToGameInfo ────────────────────────────────────────────────────────────

void GameInfoModule::saveToGameInfo(const QString &installFolder, const QVariantMap &meta,
                                    const QString &steamAppId)
{
    // Меняем только свои поля под общей блокировкой (GameInfoStore): обложки,
    // трейлер, плитки и тип ввода, записанные другими модулями, не затираются.
    QJsonObject obj;
    obj["description"] = meta.value("description").toString();
    obj["genre"]       = meta.value("genre").toString();
    obj["releaseYear"] = meta.value("releaseYear").toString();
    obj["developer"]   = meta.value("developer").toString();
    obj["publisher"]   = meta.value("publisher").toString();
    obj["rating"]      = meta.value("rating").toString();
    obj["sysreqMin"]   = meta.value("sysreqMin").toString();
    obj["sysreqRec"]   = meta.value("sysreqRec").toString();
    obj["infoFetched"] = true;
    if (!steamAppId.isEmpty())
        obj["steamAppId"] = steamAppId;

    if (!GameInfoStore::update(installFolder, obj))
        GIM_LOG(Logger::kLevelError,
                "failed to save game_info.json for: " + installFolder);
}

// ── notifyUser ────────────────────────────────────────────────────────────────

void GameInfoModule::notifyUser(const QString &msg, bool isError)
{
    if (!m_bus) return;
    emit m_bus->moduleEvent(
        Notifications::kModule,
        Notifications::kEventShow,
        Notifications::makeEvent(
            msg,
            isError ? NotificationLevel::Error : NotificationLevel::Info,
            true,
            4000
        )
    );
}

// ── generateGameInfo (два запроса к LlamaClient) ──────────────────────────────

QVariantMap GameInfoModule::generateGameInfo(const QString &title, const QString &sourceText,
                                              const LlamaClient::TokenCallback &onDescriptionToken,
                                              const LlamaClient::TokenCallback &onJsonToken)
{
    GIM_LOG(Logger::kLevelInfo,
            QString("generateGameInfo() start for: \"%1\", sourceText: %2 chars")
                .arg(title).arg(sourceText.length()));

    // ВАЖНО: sourceText может содержать %1, %2 и т.д. (Wikipedia, Steam),
    // поэтому используем конкатенацию вместо QString::arg() везде где есть пользовательский контент.
    const QString sourceBlock = sourceText.isEmpty()
        ? (QStringLiteral("Игра: \"") + title + QStringLiteral("\"\nИсточники недоступны. Используй собственные знания. "
           "Если не уверен — оставь пустую строку."))
        : (QStringLiteral("Игра: \"") + title + QStringLiteral("\"\n\nИсточники (приоритет: Wikipedia > Steam > SteamSpy):\n")
           + sourceText);

    // ── Запрос 1: короткие поля ───────────────────────────────────────────────
    const QString fieldsSystem =
        "Ты — база данных видеоигр. "
        "Отвечай ТОЛЬКО валидным JSON-объектом без каких-либо пояснений. "
        "Никакого markdown, никаких ```json блоков, никакого текста до или после JSON. "
        "Первый символ ответа — '{', последний — '}'. "
        "Все строковые значения на русском языке. "
        "Если данные неизвестны или не упомянуты в источниках — пустая строка \"\". "
        "Не используй значения null, \"неизвестно\", \"н/д\" или подобные заглушки.";

    const QString fieldsSchema =
        "{\n"
        "  \"genre\": \"жанр, 1-3 слова, например: Платформер или RPG, Action\",\n"
        "  \"releaseYear\": \"только год цифрами, например: 2015\",\n"
        "  \"developer\": \"студия-разработчик\",\n"
        "  \"publisher\": \"издатель\",\n"
        "  \"rating\": \"оценка критиков в формате ЧЧ/100, например: 92/100\",\n"
        "  \"sysreqMin\": \"минимальные требования: CPU, RAM, GPU, место на диске\",\n"
        "  \"sysreqRec\": \"рекомендуемые требования: CPU, RAM, GPU, место на диске\"\n"
        "}";

    const QString fieldsUser = sourceText.isEmpty()
        ? (sourceBlock + QStringLiteral("\n\nЗаполни схему:\n") + fieldsSchema)
        : (sourceBlock +
           QStringLiteral("\n\nПравила:\n"
           "- genre: краткий жанр из Wikipedia или Steam тегов.\n"
           "- sysreqMin и sysreqRec: должны быть РАЗНЫМИ если оба есть в источниках.\n"
           "- Не придумывай данные которых нет в источниках.\n\n"
           "Заполни схему:\n") + fieldsSchema);

    GIM_LOG(Logger::kLevelInfo, "generateGameInfo(): запрос 1 — короткие поля");
    const QString fieldsRaw = m_llama->runInference(fieldsSystem, fieldsUser, onJsonToken);
    if (fieldsRaw.isEmpty()) {
        GIM_LOG(Logger::kLevelError,
                QString("generateGameInfo(): пустой ответ (запрос 1) для \"%1\"").arg(title));
        return {};
    }

    QVariantMap result = parseGameInfoJson(fieldsRaw);
    if (result.isEmpty()) {
        GIM_LOG(Logger::kLevelError,
                QString("generateGameInfo(): не удалось распарсить JSON для \"%1\"").arg(title));
        return {};
    }

    GIM_LOG(Logger::kLevelInfo,
            QString("generateGameInfo(): запрос 1 OK, %1 полей. Запрос 2 — description")
                .arg(result.size()));

    // Эмитим короткие поля сразу — до второго запроса.
    // GUI покажет жанр/год/разработчика и закроет JSON попап пока идёт стриминг description.
    {
        const QStringList shortFields = {"genre", "releaseYear", "developer",
                                         "publisher", "rating", "sysreqMin", "sysreqRec"};
        QVariantMap fields;
        for (const QString &key : shortFields)
            if (result.contains(key)) fields[key] = result[key];

        QString tileId;
        {
            QMutexLocker locker(&m_candidateMutex);
            tileId = m_pendingTileId;
        }
        QMetaObject::invokeMethod(this, [this, tileId, fields]() {
            emit fieldsReady(tileId, fields);
        }, Qt::QueuedConnection);
    }

    // ── Запрос 2: description со стримингом ──────────────────────────────────
    const QString descSystem =
        "Ты — автор увлекательных описаний видеоигр. "
        "Напиши описание из 4-5 предложений на русском языке — строго не больше 5. "
        "Фокус: сеттинг, геймплей, атмосфера, что делает игру уникальной и интересной. "
        "Стиль живой и захватывающий — читатель должен захотеть сыграть. "
        "Не упоминай платформы, даты выхода, технические факты. "
        "Без клише вроде «незабываемый», «эпический», «уникальный опыт».";

    const QString descUser = sourceText.isEmpty()
        ? (sourceBlock + QStringLiteral("\n\nОпиши игру увлекательно: сеттинг, геймплей, особенности. "
           "Если игра неизвестна — верни пустую строку."))
        : (sourceBlock + QStringLiteral("\n\nИспользуй Steam-описание как основу для передачи атмосферы и геймплея, "
           "Wikipedia — только для фактов о разработчике и жанре если нужно. "
           "Не упоминай платформы, даты выхода, награды и коммерческий успех."));

    QString description;
    const LlamaClient::TokenCallback descCallback = onDescriptionToken
        ? LlamaClient::TokenCallback([&](const QString &token) {
              description += token;
              onDescriptionToken(token);
          })
        : LlamaClient::TokenCallback([&](const QString &token) { description += token; });

    const QString descRaw = m_llama->runInference(descSystem, descUser, descCallback);
    const QString descTrimmed = descRaw.trimmed();

    if (descTrimmed.isEmpty()) {
        GIM_LOG(Logger::kLevelWarning,
                QString("generateGameInfo(): пустое описание (запрос 2) для \"%1\"").arg(title));
    } else {
        GIM_LOG(Logger::kLevelInfo,
                QString("generateGameInfo(): описание получено, %1 символов").arg(descTrimmed.length()));
        result["description"] = descTrimmed;
    }

    GIM_LOG(Logger::kLevelInfo,
            QString("generateGameInfo(): завершено для \"%1\", итого %2 полей")
                .arg(title).arg(result.size()));
    return result;
}

// ── parseGameInfoJson ─────────────────────────────────────────────────────────

QVariantMap GameInfoModule::parseGameInfoJson(const QString &raw)
{
    QString text = raw.trimmed();

    if (text.startsWith("```")) {
        const int start = text.indexOf('\n') + 1;
        const int end   = text.lastIndexOf("```");
        if (start > 0 && end > start)
            text = text.mid(start, end - start).trimmed();
    }

    const int first = text.indexOf('{');
    const int last  = text.lastIndexOf('}');
    if (first < 0 || last <= first)
        return {};

    text = text.mid(first, last - first + 1);

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError)
        return {};

    return doc.object().toVariantMap();
}
