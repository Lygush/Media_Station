// modules/GameInfoModule/GameInfoFetcher.cpp
#include "GameInfoFetcher.h"
#include "HtmlStripper.h"
#include "TitleNormalizer.h"
#include "SteamAppList.h"
#include "tools/HttpClient.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDateTime>
#include <QDebug>

static constexpr int kSourceMaxChars = 12000; // ~3000 токенов на источник

GameInfoFetcher::GameInfoFetcher(QObject *parent)
    : QObject(parent)
{}

QString GameInfoFetcher::fetchWikipedia(const QString &normalizedTitle)
{
    // Шаг 1: OpenSearch API — ищем статью
    const QString searchUrl = QString(
        "https://ru.wikipedia.org/w/api.php"
        "?action=opensearch&search=%1&limit=3&format=json"
    ).arg(QString::fromUtf8(QUrl::toPercentEncoding(normalizedTitle)));

    const QByteArray searchData = HttpClient::get(searchUrl);
    if (searchData.isEmpty()) return {};

    const QJsonDocument doc = QJsonDocument::fromJson(searchData);
    if (!doc.isArray()) return {};

    const QJsonArray arr = doc.array();
    // Формат: [query, [titles], [descriptions], [urls]]
    if (arr.size() < 4) return {};

    const QJsonArray urls = arr[3].toArray();
    if (urls.isEmpty()) return {};

    const QString pageUrl = urls[0].toString();
    if (pageUrl.isEmpty()) return {};

    qDebug() << "GameInfoFetcher: Wikipedia URL:" << pageUrl;

    // Шаг 2: скачиваем страницу
    const QByteArray html = HttpClient::get(pageUrl);
    if (html.isEmpty()) return {};

    const QString text = HtmlStripper::strip(QString::fromUtf8(html), kSourceMaxChars);
    return text.isEmpty() ? QString() : QString("[Wikipedia RU]\n") + text;
}

// ── Локальный список приложений ───────────────────────────────────────────────

// Первый источник: локальный JSON tools/applicationslist/applications.json.
// Даёт appId и краткое описание без сетевых запросов.
QString GameInfoFetcher::fetchLocalAppInfo(const QString &normalizedTitle, QString &outAppId)
{
    const SteamAppList &appList = SteamAppList::instance();
    if (!appList.isLoaded()) {
        qDebug() << "GameInfoFetcher: local app list not loaded yet, skip";
        return {};
    }

    const SteamAppList::LocalAppInfo info = appList.findAppInfo(normalizedTitle);
    if (!info.isValid()) {
        qDebug() << "GameInfoFetcher: not found in local app list:" << normalizedTitle;
        return {};
    }

    outAppId = QString::number(info.appid);
    qDebug() << "GameInfoFetcher: local app list hit appId=" << outAppId
             << "for" << normalizedTitle;

    QStringList parts;
    if (!info.shortDescription.isEmpty())
        parts << "Описание: " + info.shortDescription;
    if (info.metacriticScore >= 0)
        parts << "Metacritic: " + QString::number(info.metacriticScore, 'f', 0) + "/100";
    if (!info.releaseDate.isEmpty())
        parts << "Дата выхода: " + info.releaseDate;

    if (parts.isEmpty()) return {};

    return QStringLiteral("[Local DB]\n") + parts.join("\n");
}

// ── Вспомогательные методы ────────────────────────────────────────────────────

QString GameInfoFetcher::simplifyTitle(const QString &s)
{
    QString r = s.toLower();
    r.remove(QRegularExpression("[^\\w\\d\\s]"));
    return r.simplified();
}

double GameInfoFetcher::titleSimilarity(const QString &query, const QString &candidate)
{
    const QString q = simplifyTitle(query);
    const QString c = simplifyTitle(candidate);

    if (c == q)              return 1.0;
    if (c.contains(q) || q.contains(c)) return 0.85;

    const QStringList qWords = q.split(' ', Qt::SkipEmptyParts);
    const QStringList cWords = c.split(' ', Qt::SkipEmptyParts);
    int matches = 0;
    for (const QString &w : qWords)
        if (cWords.contains(w)) matches++;

    return qWords.isEmpty() ? 0.0 : double(matches) / qWords.size();
}

// Проверяем через appdetails что appId действительно основная игра, не DLC/bundle
bool GameInfoFetcher::verifySteamAppType(const QString &appId)
{
    const QString url = QString(
        "https://store.steampowered.com/api/appdetails?appids=%1&filters=basic"
    ).arg(appId);

    const QByteArray data = HttpClient::get(url);
    if (data.isEmpty()) return false;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) return false;

    const QJsonObject root = doc.object();
    const QJsonObject appObj = root[appId].toObject();
    if (!appObj["success"].toBool()) return false;

    const QString type = appObj["data"].toObject()["type"].toString();
    qDebug() << "GameInfoFetcher: appdetails type for" << appId << ":" << type;

    return type == QLatin1String("game") || type == QLatin1String("app");
}

// ── Steam ─────────────────────────────────────────────────────────────────────

// Загружает страницу магазина Steam по известному appId с указанным регионом.
QString GameInfoFetcher::fetchSteamPageById(const QString &appId, const QString &cc)
{
    if (appId.isEmpty()) return {};

    const QString url = QString(
        "https://store.steampowered.com/app/%1/?l=russian&cc=%2"
    ).arg(appId, cc);

    qDebug() << "GameInfoFetcher: fetching Steam page by appId" << appId << "cc=" << cc;
    const QByteArray html = HttpClient::get(url);
    if (html.isEmpty()) return {};

    const QString text = HtmlStripper::strip(QString::fromUtf8(html), kSourceMaxChars);
    return text.isEmpty() ? QString() : QString("[Steam]\n") + text;
}

QString GameInfoFetcher::fetchSteam(const QString &normalizedTitle, QString &outAppId)
{
    // Пробуем сначала RU, затем US если RU не дал результатов
    const QStringList regions = { "ru", "us" };

    for (const QString &cc : regions) {
        const QString searchUrl = QString(
            "https://store.steampowered.com/api/storesearch/?term=%1&l=english&cc=%2"
        ).arg(QString::fromUtf8(QUrl::toPercentEncoding(normalizedTitle)), cc);

        const QByteArray searchData = HttpClient::get(searchUrl);
        if (searchData.isEmpty()) continue;

        const QJsonDocument doc = QJsonDocument::fromJson(searchData);
        if (!doc.isObject()) continue;

        const QJsonArray items = doc.object()["items"].toArray();
        if (items.isEmpty()) {
            qDebug() << "GameInfoFetcher: Steam storesearch empty for cc=" << cc;
            continue;
        }

        // Выбираем наиболее подходящий результат из первых 5
        int    bestAppId = -1;
        double bestScore = 0.0;
        QString bestName;

        for (int i = 0; i < std::min(10, int(items.size())); ++i) {
            const QJsonObject item = items[i].toObject();
            const QString name     = item["name"].toString();
            const int     id       = item["id"].toInt();
            const QString type     = item["type"].toString();

            double score = titleSimilarity(normalizedTitle, name);

            // Штрафуем не-игры
            if (type == QLatin1String("dlc"))    score *= 0.4;
            if (type == QLatin1String("bundle")) score *= 0.5;
            if (type == QLatin1String("sub"))    score *= 0.5;

            // Штраф за инструменты/редакторы/саундтреки
            static const QStringList kToolMarkers = {
                "redkit", "editor", "sdk", "dedicated server",
                "soundtrack", "ost", "artbook", "toolkit", "demo"
            };
            const QString nameLower = name.toLower();
            for (const QString &marker : kToolMarkers) {
                if (nameLower.contains(marker)) {
                    score *= 0.3;
                    break;
                }
            }

            // Штраф за DLC персонажей: "GameName CharacterName" — одно лишнее слово
            {
                const QString qs = simplifyTitle(normalizedTitle);
                const QString cs = simplifyTitle(name);
                if (cs.startsWith(qs + " ")) {
                    const QString suffix = cs.mid(qs.length() + 1).trimmed();
                    if (!suffix.isEmpty() && !suffix.contains(' '))
                        score *= 0.35;
                }
            }

            qDebug() << "Steam candidate [cc=" << cc << "]:" << name
                     << "type:" << type << "score:" << score << "id:" << id;

            if (score > bestScore) {
                bestScore = score;
                bestAppId = id;
                bestName  = name;
            }
        }

        if (bestAppId <= 0 || bestScore < 0.4) {
            qDebug() << "GameInfoFetcher: no good Steam match for cc=" << cc
                     << "(best score:" << bestScore << ")";
            continue;
        }

        // Подтверждаем через appdetails что это действительно игра
        const QString appIdStr = QString::number(bestAppId);
        if (!verifySteamAppType(appIdStr)) {
            qDebug() << "GameInfoFetcher: appId" << appIdStr
                     << "is not a game (DLC/bundle/tool), skipping";
            continue;
        }

        outAppId = appIdStr;
        qDebug() << "GameInfoFetcher: Steam best match:" << bestName
                 << "score:" << bestScore << "appId:" << outAppId << "cc=" << cc;

        // Страницу магазина тоже берём через cc=us чтобы обойти региональные блокировки
        const QString storeUrl = QString(
            "https://store.steampowered.com/app/%1/?l=russian&cc=us"
        ).arg(bestAppId);

        const QByteArray html = HttpClient::get(storeUrl);
        if (html.isEmpty()) return {};

        const QString text = HtmlStripper::strip(QString::fromUtf8(html), kSourceMaxChars);
        return text.isEmpty() ? QString() : QString("[Steam]\n") + text;
    }

    // ── Фолбэк: локальный список всех приложений Steam ────────────────────────
    // storesearch не нашёл игру (удалена из магазина, региональные ограничения)
    // Пробуем найти appId в закешированном полном списке
    {
        const SteamAppList &appList = SteamAppList::instance();
        if (appList.isLoaded()) {
            const QString appIdStr = appList.findAppId(normalizedTitle);
            if (!appIdStr.isEmpty() && verifySteamAppType(appIdStr)) {
                outAppId = appIdStr;
                qDebug() << "GameInfoFetcher: found via SteamAppList appId:" << appIdStr;

                const QString storeUrl = QString(
                    "https://store.steampowered.com/app/%1/?l=russian&cc=us"
                ).arg(appIdStr);
                const QByteArray html = HttpClient::get(storeUrl);
                if (!html.isEmpty()) {
                    const QString text = HtmlStripper::strip(QString::fromUtf8(html), kSourceMaxChars);
                    if (!text.isEmpty())
                        return QString("[Steam]\n") + text;
                }
                // Страница недоступна но appId нашли — возвращаем пустой текст,
                // appId всё равно сохранится для медиа
                return {};
            }
        } else {
            qDebug() << "GameInfoFetcher: SteamAppList not loaded yet, skip fallback";
        }
    }

    return {};
}



// Берём страницу сообщества — там описание, теги, системные требования.
// appId уже известен из fetchSteam, передаём его напрямую.
QString GameInfoFetcher::fetchSteamCommunity(const QString &appId)
{
    if (appId.isEmpty()) return {};

    const QString url = QString("https://store.steampowered.com/app/%1/?l=russian&cc=us").arg(appId);
    const QByteArray html = HttpClient::get(url);
    if (html.isEmpty()) return {};

    const QString text = HtmlStripper::strip(QString::fromUtf8(html), kSourceMaxChars);
    if (text.isEmpty()) return {};

    qDebug() << "GameInfoFetcher: SteamCommunity OK for appId" << appId;
    return QString("[Steam Store]\n") + text;
}

// ── SteamSpy ─────────────────────────────────────────────────────────────────

// SteamSpy: теги, разработчик, издатель — без описания игры.
// Оставлен для возможного использования, в основную цепочку не входит.
QString GameInfoFetcher::fetchSteamSpy(const QString &appId)
{
    if (appId.isEmpty()) return {};

    const QString url = QString(
        "https://steamspy.com/api.php?request=appdetails&appid=%1"
    ).arg(appId);
    const QByteArray data = HttpClient::get(url);
    if (data.isEmpty()) return {};

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (obj.isEmpty()) return {};

    QStringList parts;
    const QString developer = obj["developer"].toString();
    const QString publisher = obj["publisher"].toString();
    if (!developer.isEmpty()) parts << "Разработчик: " + developer;
    if (!publisher.isEmpty()) parts << "Издатель: "   + publisher;

    const QJsonObject tags = obj["tags"].toObject();
    if (!tags.isEmpty()) {
        QList<QPair<int,QString>> tagList;
        for (auto it = tags.begin(); it != tags.end(); ++it)
            tagList.append({ it.value().toInt(), it.key() });
        std::sort(tagList.begin(), tagList.end(),
                  [](const auto &a, const auto &b){ return a.first > b.first; });
        QStringList topTags;
        for (int i = 0; i < std::min(5, int(tagList.size())); ++i)
            topTags << tagList[i].second;
        parts << "Теги: " + topTags.join(", ");
    }

    if (parts.isEmpty()) return {};
    qDebug() << "GameInfoFetcher: SteamSpy OK for appId" << appId;
    return QString("[SteamSpy]\n") + parts.join("\n");
}

// ── Фолбэки ───────────────────────────────────────────────────────────────────

QString GameInfoFetcher::fetchWikipediaEn(const QString &normalizedTitle)
{
    const QString searchUrl = QString(
        "https://en.wikipedia.org/w/api.php"
        "?action=opensearch&search=%1&limit=3&format=json"
    ).arg(QString::fromUtf8(QUrl::toPercentEncoding(normalizedTitle)));

    const QByteArray searchData = HttpClient::get(searchUrl);
    if (searchData.isEmpty()) return {};

    const QJsonDocument doc = QJsonDocument::fromJson(searchData);
    if (!doc.isArray()) return {};

    const QJsonArray arr = doc.array();
    if (arr.size() < 4) return {};

    const QJsonArray urls = arr[3].toArray();
    if (urls.isEmpty()) return {};

    const QString pageUrl = urls[0].toString();
    if (pageUrl.isEmpty()) return {};

    qDebug() << "GameInfoFetcher: Wikipedia EN URL:" << pageUrl;

    const QByteArray pageHtml = HttpClient::get(pageUrl);
    if (pageHtml.isEmpty()) return {};

    const QString text = HtmlStripper::strip(QString::fromUtf8(pageHtml), kSourceMaxChars);
    return text.isEmpty() ? QString() : QString("[Wikipedia EN]\n") + text;
}

QString GameInfoFetcher::fetchMetacritic(const QString &normalizedTitle, QString &outAppId)
{
    // Metacritic как фолбэк для текста (appId не даёт)
    Q_UNUSED(outAppId)

    const QString searchUrl = QString(
        "https://www.metacritic.com/search/%1/?category=13" // 13 = games
    ).arg(QString::fromUtf8(QUrl::toPercentEncoding(normalizedTitle)));

    const QByteArray searchHtml = HttpClient::get(searchUrl);
    if (searchHtml.isEmpty()) return {};

    const QString html = QString::fromUtf8(searchHtml);

    // Ищем первую ссылку на игру: /game/название/
    QRegularExpression re("href=\"(/game/[^\"]+)\"");
    QRegularExpressionMatch match = re.match(html);
    if (!match.hasMatch()) {
        qDebug() << "GameInfoFetcher: Metacritic no game link for" << normalizedTitle;
        return {};
    }

    const QString gameUrl = "https://www.metacritic.com" + match.captured(1);
    qDebug() << "GameInfoFetcher: Metacritic URL:" << gameUrl;

    const QByteArray gameHtml = HttpClient::get(gameUrl);
    if (gameHtml.isEmpty()) return {};

    const QString text = HtmlStripper::strip(QString::fromUtf8(gameHtml), kSourceMaxChars);
    return text.isEmpty() ? QString() : QString("[Metacritic]\n") + text;
}

GameInfoFetcher::FetchResult GameInfoFetcher::fetch(const QString &title, const QString &/*installFolder*/)
{
    const QString normalized = TitleNormalizer::normalize(title);
    qDebug() << "GameInfoFetcher::fetch normalized title:" << normalized;

    QStringList parts;
    QString steamAppId;

    // ── 0. Локальный список (без сети, быстро) ────────────────────────────────
    // Даёт приоритетный appId и краткое описание для контекста LLM.
    // appId из локального списка не перезаписывается другими источниками.
    {
        QString localAppId;
        const QString localText = fetchLocalAppInfo(normalized, localAppId);
        if (!localAppId.isEmpty()) {
            steamAppId = localAppId; // приоритетный appId
            if (!localText.isEmpty()) {
                parts << localText;
                qDebug() << "GameInfoFetcher: Local DB OK, appId:" << steamAppId;
            }
        }
    }

    // ── 1. Wikipedia RU (основной) ────────────────────────────────────────────
    const QString wikiText = fetchWikipedia(normalized);
    if (!wikiText.isEmpty()) {
        parts << wikiText;
        qDebug() << "GameInfoFetcher: Wikipedia RU OK";
    } else {
        qDebug() << "GameInfoFetcher: Wikipedia RU failed, trying EN";
        const QString wikiEnText = fetchWikipediaEn(normalized);
        if (!wikiEnText.isEmpty()) {
            parts << wikiEnText;
            qDebug() << "GameInfoFetcher: Wikipedia EN OK";
        } else {
            qDebug() << "GameInfoFetcher: Wikipedia unavailable";
        }
    }

    // ── 2. Steam ──────────────────────────────────────────────────────────────
    // Цель: получить страницу магазина как второй источник для LLM.
    // Цепочка: ru → us → SteamSpy+appdetails → SteamCommunity (последний шанс).
    // SteamCommunity/SteamSpy используются ТОЛЬКО если страница недоступна —
    // не добавляются поверх успешного Steam.
    {
        QString steamText;

        if (!steamAppId.isEmpty()) {
            // appId известен — storesearch не нужен, сразу берём страницу
            steamText = fetchSteamPageById(steamAppId, "ru");
            if (steamText.isEmpty()) {
                qDebug() << "GameInfoFetcher: Steam ru failed, trying us";
                steamText = fetchSteamPageById(steamAppId, "us");
            }
            if (steamText.isEmpty()) {
                qDebug() << "GameInfoFetcher: Steam page unavailable, trying SteamCommunity";
                steamText = fetchSteamCommunity(steamAppId);
            }
        } else {
            // appId неизвестен — делаем storesearch (он сам загружает страницу)
            QString steamFoundId;
            steamText = fetchSteam(normalized, steamFoundId);
            if (!steamFoundId.isEmpty())
                steamAppId = steamFoundId;
        }

        if (!steamText.isEmpty()) {
            parts << steamText;
            qDebug() << "GameInfoFetcher: Steam source OK, appId:" << steamAppId;
        } else {
            qDebug() << "GameInfoFetcher: all Steam sources failed";
        }
    }

    if (parts.isEmpty()) {
        qDebug() << "GameInfoFetcher: all sources failed, LLM will use own knowledge";
        return { {}, steamAppId };
    }

    QString combined = parts.join("\n\n---\n\n");
    // Финальный лимит на случай если все три источника дали максимум
    if (combined.length() > 40000)
        combined = combined.left(40000);

    return { combined, steamAppId };
}
