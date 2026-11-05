// modules/GameMediaModule/CoverFetcher.cpp

#include "CoverFetcher.h"
#include "SteamAppList.h"
#include "tools/HttpClient.h"
#include "tools/AtomicFile.h"

#include <QFile>
#include <QDir>
#include <QDebug>

// Steam CDN URL-шаблоны
static QString urlCover600x900(const QString &appId) {
    return QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/library_600x900.jpg").arg(appId);
}
static QString urlHero(const QString &appId) {
    return QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/library_hero.jpg").arg(appId);
}
static QString urlHeader(const QString &appId) {
    return QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/header.jpg").arg(appId);
}
// Широкоэкранный фон 1920×1080 — лучший вариант для backdrop
static QString urlPageBg(const QString &appId) {
    return QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/page_bg_generated_v6b.jpg").arg(appId);
}
// Capsule 616×353 — резервный backdrop если нет page_bg и hero
static QString urlCapsule(const QString &appId) {
    return QStringLiteral("https://cdn.akamai.steamstatic.com/steam/apps/%1/capsule_616x353.jpg").arg(appId);
}

// ─────────────────────────────────────────────────────────────────────────────

CoverFetcher::CoverFetcher(QObject *parent)
    : QObject(parent)
{}

void CoverFetcher::cancel()
{
    m_cancelled = true;
}

// ── Основная логика ───────────────────────────────────────────────────────────

void CoverFetcher::fetch(const QString &mediaDir, const QString &steamAppId)
{
    m_cancelled = false;

    QString coverPath;
    QString backdropPath;

    // 1. Обложка: library_600x900 → fallback header
    if (!m_cancelled) {
        QByteArray data = HttpClient::get(urlCover600x900(steamAppId), HttpClient::kTimeoutLong, &m_cancelled);
        if (!data.isEmpty()) {
            QString dest = mediaDir + "/cover.jpg";
            if (saveFile(data, dest))
                coverPath = dest;
        }
    }

    if (coverPath.isEmpty() && !m_cancelled) {
        QByteArray data = HttpClient::get(urlHeader(steamAppId), HttpClient::kTimeoutLong, &m_cancelled);
        if (!data.isEmpty()) {
            QString dest = mediaDir + "/cover.jpg";
            if (saveFile(data, dest))
                coverPath = dest;
        }
    }

    // 2. Backdrop: library_hero → page_bg_generated → capsule_616x353
    if (!m_cancelled) {
        QByteArray data = HttpClient::get(urlHero(steamAppId), HttpClient::kTimeoutLong, &m_cancelled);
        if (!data.isEmpty()) {
            QString dest = mediaDir + "/backdrop.jpg";
            if (saveFile(data, dest))
                backdropPath = dest;
        }
    }

    if (backdropPath.isEmpty() && !m_cancelled) {
        QByteArray data = HttpClient::get(urlPageBg(steamAppId), HttpClient::kTimeoutLong, &m_cancelled);
        if (!data.isEmpty()) {
            QString dest = mediaDir + "/backdrop.jpg";
            if (saveFile(data, dest))
                backdropPath = dest;
        }
    }

    if (backdropPath.isEmpty() && !m_cancelled) {
        QByteArray data = HttpClient::get(urlCapsule(steamAppId), HttpClient::kTimeoutLong, &m_cancelled);
        if (!data.isEmpty()) {
            QString dest = mediaDir + "/backdrop.jpg";
            if (saveFile(data, dest))
                backdropPath = dest;
        }
    }

    // ── Fallback из локального списка ─────────────────────────────────────────
    // Если после всех попыток со Steam CDN cover или backdrop всё ещё пустые,
    // пробуем URL из applications.json (header_image, background).
    if (!steamAppId.isEmpty() && (!coverPath.isEmpty() || !backdropPath.isEmpty() || true)) {
        const SteamAppList::LocalAppInfo info =
            SteamAppList::instance().appInfoById(steamAppId.toInt());

        if (coverPath.isEmpty() && !info.headerImage.isEmpty() && !m_cancelled) {
            qDebug() << "CoverFetcher: trying local DB headerImage for appId" << steamAppId;
            QByteArray data = HttpClient::get(info.headerImage, HttpClient::kTimeoutLong, &m_cancelled);
            if (!data.isEmpty()) {
                QString dest = mediaDir + "/cover.jpg";
                if (saveFile(data, dest))
                    coverPath = dest;
            }
        }

        if (backdropPath.isEmpty() && !info.background.isEmpty() && !m_cancelled) {
            qDebug() << "CoverFetcher: trying local DB background for appId" << steamAppId;
            QByteArray data = HttpClient::get(info.background, HttpClient::kTimeoutLong, &m_cancelled);
            if (!data.isEmpty()) {
                QString dest = mediaDir + "/backdrop.jpg";
                if (saveFile(data, dest))
                    backdropPath = dest;
            }
        }
    }

    emit finished(coverPath, backdropPath);
}

// ── Атомарная запись файла ────────────────────────────────────────────────────

bool CoverFetcher::saveFile(const QByteArray &data, const QString &destPath)
{
    // Раньше: запись без проверки + remove() + rename() — при нехватке места
    // сохранялась обрезанная картинка, а сбой между remove и rename терял
    // старую обложку. AtomicFile (QSaveFile) проверяет запись и меняет файл
    // атомарно.
    if (!AtomicFile::writeBytes(destPath, data)) {
        qWarning() << "CoverFetcher: cannot save" << destPath;
        return false;
    }

    qDebug() << "CoverFetcher: saved" << destPath;
    return true;
}
