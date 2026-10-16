// modules/KaraokeModule/LrcFetcher.cpp
#include "LrcFetcher.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>
#include <QThread>

#include "../../tools/HttpClient.h"

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static QString cleanTag(const QString &s)
{
    // Убираем ведущие/хвостовые пробелы.
    // ID3-теги иногда содержат лишние символы — оставляем как есть,
    // QUrl::toPercentEncoding справится с кодированием при POST.
    return s.trimmed();
}

// ─────────────────────────────────────────────────────────────────────────────
// LrcFetcher::buildUrl
// ─────────────────────────────────────────────────────────────────────────────

QString LrcFetcher::buildUrl(const QString &artist,
                             const QString &title,
                             const QString &album,
                             int            durationSec)
{
    QUrlQuery q;
    q.addQueryItem("artist_name", cleanTag(artist));
    q.addQueryItem("track_name",  cleanTag(title));
    if (!album.trimmed().isEmpty())
        q.addQueryItem("album_name", cleanTag(album));
    if (durationSec > 0)
        q.addQueryItem("duration", QString::number(durationSec));

    QUrl url("https://lrclib.net/api/get");
    url.setQuery(q);
    return url.toString();
}

// ─────────────────────────────────────────────────────────────────────────────
// LrcFetcher::fetch
// ─────────────────────────────────────────────────────────────────────────────

LrcResult LrcFetcher::fetch(const QString          &artist,
                             const QString          &title,
                             const QString          &album,
                             int                     durationSec,
                             const std::atomic<bool> *cancelled) const
{
    LrcResult result;

    if (title.trimmed().isEmpty()) {
        qDebug() << "[LrcFetcher] Пустой title — пропускаем запрос";
        return result;
    }

    // Нет тегов -> KaraokeModule подставляет артиста "Unknown". Такой артист
    // ломает поиск, поэтому в этом случае ищем только по названию (q=).
    const bool noArtist = artist.trimmed().isEmpty()
                          || artist.trimmed().compare(QLatin1String("Unknown"), Qt::CaseInsensitive) == 0;

    // /api/get требует ВСЕ поля (артист, трек, альбом, длительность). Без TagLib
    // альбома и длительности нет → lrclib отвечает 400 и текст никогда не
    // находился. В этом случае сразу используем /api/search.
    QByteArray raw;
    if (!noArtist && !album.trimmed().isEmpty() && durationSec > 0) {
        const QString url = buildUrl(artist, title, album, durationSec);
        qDebug() << "[LrcFetcher] GET" << url;
        raw = HttpClient::get(url, HttpClient::kTimeoutDefault, cancelled);
        if (cancelled && cancelled->load())
            return result;
    }

    if (raw.isEmpty()) {
        // Поиск с повторами: lrclib иногда отвечает 5xx (503)
        auto search = [&](const QUrlQuery &q) -> QByteArray {
            QUrl url("https://lrclib.net/api/search");
            url.setQuery(q);
            qDebug() << "[LrcFetcher] SEARCH" << url.toString();
            QByteArray found;
            for (int attempt = 0; attempt < 3 && found.isEmpty(); ++attempt) {
                if (attempt > 0) {
                    qDebug() << "[LrcFetcher] повтор запроса, попытка" << attempt + 1;
                    for (int i = 0; i < 15; ++i) {            // пауза 1.5 с с проверкой отмены
                        if (cancelled && cancelled->load()) return {};
                        QThread::msleep(100);
                    }
                }
                found = HttpClient::get(url.toString(), HttpClient::kTimeoutDefault, cancelled);
                if (cancelled && cancelled->load()) return {};
            }
            return found;
        };

        QUrlQuery q;
        if (noArtist) {
            q.addQueryItem("q", cleanTag(title));
        } else {
            q.addQueryItem("artist_name", cleanTag(artist));
            q.addQueryItem("track_name",  cleanTag(title));
        }
        QByteArray found = search(q);
        if (cancelled && cancelled->load())
            return result;

        // Точный поиск ничего не дал (опечатки, "feat.", иное написание) —
        // пробуем свободный запрос "артист название"
        if (!noArtist && QJsonDocument::fromJson(found).array().isEmpty() && !found.isEmpty()) {
            QUrlQuery q2;
            q2.addQueryItem("q", cleanTag(artist) + QLatin1Char(' ') + cleanTag(title));
            found = search(q2);
            if (cancelled && cancelled->load())
                return result;
        }

        const QJsonArray arr = QJsonDocument::fromJson(found).array();
        QJsonObject best;
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            // Предпочитаем синхронизированный текст; при известной длительности —
            // ближайший по ней вариант
            const bool synced = !o.value("syncedLyrics").toString().trimmed().isEmpty();
            const bool bestSynced = !best.value("syncedLyrics").toString().trimmed().isEmpty();
            if (best.isEmpty() || (synced && !bestSynced)) { best = o; continue; }
            if (synced == bestSynced && durationSec > 0
                && qAbs(o.value("duration").toDouble() - durationSec)
                   < qAbs(best.value("duration").toDouble() - durationSec))
                best = o;
        }
        if (!best.isEmpty())
            raw = QJsonDocument(best).toJson(QJsonDocument::Compact);
        else if (!found.isEmpty())
            return result;   // сеть есть, но трек не найден
    }

    if (raw.isEmpty()) {
        // HttpClient логирует детали — здесь просто отмечаем сетевую ошибку
        qDebug() << "[LrcFetcher] Пустой ответ (нет сети или трек не найден)";
        result.networkError = true;
        return result;
    }

    // ── Парсим JSON ───────────────────────────────────────────────────────────
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[LrcFetcher] Невалидный JSON:" << parseErr.errorString();
        return result;
    }

    const QJsonObject obj = doc.object();

    // lrclib возвращает HTTP 404 с телом {"code":404,"name":"..."}
    // HttpClient в этом случае вернёт пустой массив (только HTTP 200 проходит),
    // но на всякий случай проверяем поле "code"
    if (obj.contains("code") && obj["code"].toInt() != 200) {
        qDebug() << "[LrcFetcher] lrclib ответил кодом" << obj["code"].toInt()
                 << "для" << artist << "-" << title;
        return result;
    }

    // ── syncedLyrics — основной результат ────────────────────────────────────
    const QString synced = obj.value("syncedLyrics").toString().trimmed();
    if (!synced.isEmpty() && synced != "null") {
        result.hasSynced = true;
        result.lrc       = synced;
        qDebug() << "[LrcFetcher] syncedLyrics получен, строк:"
                 << synced.count('\n') + 1;
    }

    // ── plainLyrics — fallback если Whisper недоступен ────────────────────────
    const QString plain = obj.value("plainLyrics").toString().trimmed();
    if (!plain.isEmpty() && plain != "null") {
        result.hasPlain    = true;
        result.plainLyrics = plain;
    }

    if (!result.hasSynced && !result.hasPlain) {
        qDebug() << "[LrcFetcher] Трек найден, но оба поля lyrics пусты:"
                 << artist << "-" << title;
    }

    return result;
}
