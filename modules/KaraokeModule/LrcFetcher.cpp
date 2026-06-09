// modules/KaraokeModule/LrcFetcher.cpp
#include "LrcFetcher.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>

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

    if (artist.trimmed().isEmpty() || title.trimmed().isEmpty()) {
        qDebug() << "[LrcFetcher] Пустой artist или title — пропускаем запрос";
        return result;
    }

    const QString url = buildUrl(artist, title, album, durationSec);
    qDebug() << "[LrcFetcher] GET" << url;

    const QByteArray raw = HttpClient::get(url, HttpClient::kTimeoutDefault, cancelled);

    if (cancelled && cancelled->load())
        return result;

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
