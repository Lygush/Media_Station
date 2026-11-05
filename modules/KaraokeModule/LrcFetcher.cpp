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
    bool offline = false;
    if (!noArtist && !album.trimmed().isEmpty() && durationSec > 0) {
        const QString url = buildUrl(artist, title, album, durationSec);
        qDebug() << "[LrcFetcher] GET" << url;
        int status = 0;
        raw = HttpClient::get(url, HttpClient::kTimeoutDefault, cancelled, &status);
        if (cancelled && cancelled->load())
            return result;
        offline = (status == 0);   // ответа нет вовсе — сети нет, поиск не поможет
    }

    if (raw.isEmpty() && !offline) {
        // Поиск с повторами: lrclib иногда отвечает 5xx (503). Если ответа нет
        // совсем (нет интернета), повторять бессмысленно — раньше это стоило
        // до 6 × 15 с ожидания перед переходом к распознаванию.
        auto search = [&](const QUrlQuery &q) -> QByteArray {
            QUrl url("https://lrclib.net/api/search");
            url.setQuery(q);
            qDebug() << "[LrcFetcher] SEARCH" << url.toString();
            QByteArray found;
            for (int attempt = 0; attempt < 3 && found.isEmpty() && !offline; ++attempt) {
                if (attempt > 0) {
                    qDebug() << "[LrcFetcher] повтор запроса, попытка" << attempt + 1;
                    for (int i = 0; i < 15; ++i) {            // пауза 1.5 с с проверкой отмены
                        if (cancelled && cancelled->load()) return {};
                        QThread::msleep(100);
                    }
                }
                int status = 0;
                found = HttpClient::get(url.toString(), HttpClient::kTimeoutDefault, cancelled, &status);
                if (cancelled && cancelled->load()) return {};
                if (status == 0) offline = true;
                else if (status < 500) break;          // 4xx повтор не исправит
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

        // ── Выбор варианта ──────────────────────────────────────────────────
        // У одной песни на lrclib обычно несколько записей (альбомная, live,
        // remaster, радио-версия). Тайм-коды подходят только к записи той же
        // длительности. Раньше бралась первая синхронизированная — и при
        // другой версии текст «уезжал» на секунды.
        //  1) синхронизированная с длительностью ±3 с — лучший случай;
        //  2) любая с длительностью ±3 с (текст есть, время — от Whisper);
        //  3) длительность сильно другая — берём только текст (без времени),
        //     время слов даст Whisper.
        //  Если длительность файла неизвестна — как раньше, первая синхронизированная.
        constexpr double kTol = 3.0;
        const QJsonArray arr = QJsonDocument::fromJson(found).array();
        auto hasSynced = [](const QJsonObject &o) {
            return !o.value("syncedLyrics").toString().trimmed().isEmpty();
        };
        auto delta = [durationSec](const QJsonObject &o) {
            return qAbs(o.value("duration").toDouble() - durationSec);
        };
        QJsonObject best;
        bool bestTimingOk = false;
        int  bestRank = -1;   // 3 — синхр. в допуске, 2 — текст в допуске, 1 — вне допуска
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            const bool syn   = hasSynced(o);
            const bool inTol = durationSec <= 0 || delta(o) <= kTol;
            const int  rank  = inTol ? (syn ? 3 : 2) : 1;
            if (rank > bestRank
                || (rank == bestRank && durationSec > 0 && delta(o) < delta(best))) {
                best = o; bestRank = rank; bestTimingOk = inTol;
            }
        }
        if (!best.isEmpty() && !bestTimingOk && hasSynced(best)) {
            qDebug() << "[LrcFetcher] ближайший вариант по длительности:"
                     << best.value("duration").toDouble() << "с против" << durationSec
                     << "с — тайм-коды не подходят, берём только текст";
            best.remove("syncedLyrics");
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

    // /api/get вернул запись: проверяем длительность и там
    {
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        if (durationSec > 0 && o.contains("duration")
            && qAbs(o.value("duration").toDouble() - durationSec) > 3.0
            && !o.value("syncedLyrics").toString().trimmed().isEmpty()) {
            QJsonObject t = o;
            t.remove("syncedLyrics");
            raw = QJsonDocument(t).toJson(QJsonDocument::Compact);
        }
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
