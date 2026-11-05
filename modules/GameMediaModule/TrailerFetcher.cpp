// modules/GameMediaModule/TrailerFetcher.cpp
// ─────────────────────────────────────────────────────────────────────────────
// TrailerFetcher — получает трейлер через Steam appdetails API.
//   • Если найден прямой mp4 – скачивает в файл (стриминг, без загрузки в ОЗУ).
//   • Если HLS/DASH стрим – конвертирует через ffmpeg в локальный mp4.
//
// Зависимость: ffmpeg.exe (статическая сборка) должен лежать в
//   <ApplicationDir>/tools/ffmpeg/ffmpeg.exe
// или путь может быть переопределён через setFfmpegPath().
// ─────────────────────────────────────────────────────────────────────────────

#include "TrailerFetcher.h"

#include <QCoreApplication>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QEventLoop>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QDebug>
#include <QRegularExpression>

static const char *kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";

// ─────────────────────────────────────────────────────────────────────────────

TrailerFetcher::TrailerFetcher(QObject *parent)
    : QObject(parent)
{
    // Путь по умолчанию – tools/ffmpeg/ffmpeg.exe рядом с исполняемым файлом
    m_ffmpegPath = QCoreApplication::applicationDirPath()
                   + QStringLiteral("/tools/ffmpeg/ffmpeg.exe");
}

TrailerFetcher::~TrailerFetcher()
{
    if (m_trailerReply && m_trailerReply->isRunning())
        m_trailerReply->abort();
    if (m_metaReply && m_metaReply->isRunning())
        m_metaReply->abort();
    if (m_ffmpegProcess && m_ffmpegProcess->state() != QProcess::NotRunning) {
        m_ffmpegProcess->kill();
        m_ffmpegProcess->waitForFinished(3000);
    }
}

void TrailerFetcher::cancel()
{
    // Thread-safe отмена (может вызываться из GUI потока)
    QMetaObject::invokeMethod(this, [this]() {
        m_cancelled = true;
        if (m_metaReply && m_metaReply->isRunning())
            m_metaReply->abort();
        if (m_trailerReply && m_trailerReply->isRunning())
            m_trailerReply->abort();
        if (m_ffmpegProcess && m_ffmpegProcess->state() != QProcess::NotRunning) {
            m_ffmpegProcess->kill();
        }
    }, Qt::QueuedConnection);
}

void TrailerFetcher::setFfmpegPath(const QString &path)
{
    m_ffmpegPath = path;
}

// ── Запуск: запрос метаданных из Steam API ────────────────────────────────

void TrailerFetcher::fetch(const QString &mediaDir, const QString &steamAppId)
{
    m_cancelled   = false;
    m_writeFailed = false;
    m_destPath  = QDir::cleanPath(mediaDir + "/trailer.mp4");
    m_tmpPath   = m_destPath + ".tmp";

    const QString url = QStringLiteral(
                            "https://store.steampowered.com/api/appdetails?appids=%1&cc=us&filters=movies"
                            ).arg(steamAppId);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", kUserAgent);
    req.setRawHeader("Cookie", "birthtime=0; mature_content=1; lastagecheckage=1-0-1990");
    req.setTransferTimeout(30'000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    m_nam = new QNetworkAccessManager(this);
    m_metaReply = m_nam->get(req);
    connect(m_metaReply, &QNetworkReply::finished,
            this,        &TrailerFetcher::onMetaReplyFinished);

    // Блокируем до получения ответа (TrailerFetcher в отдельном потоке)
    QEventLoop loop;
    connect(m_metaReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
}

// ── Обработка ответа Steam API ───────────────────────────────────────────

void TrailerFetcher::onMetaReplyFinished()
{
    if (!m_metaReply) return;

    if (m_cancelled || m_metaReply->error() != QNetworkReply::NoError) {
        qDebug() << "TrailerFetcher: meta request failed or cancelled:"
                 << m_metaReply->errorString();
        m_metaReply->deleteLater();
        m_metaReply = nullptr;
        emit finished(QString());
        return;
    }

    const QByteArray body = m_metaReply->readAll();
    m_metaReply->deleteLater();
    m_metaReply = nullptr;

    const QString trailerUrl = parseTrailerUrl(body, QString());
    if (trailerUrl.isEmpty()) {
        qDebug() << "TrailerFetcher: no trailer found in appdetails";
        emit finished(QString());
        return;
    }

    // HLS/DASH стрим → конвертируем через ffmpeg
    if (trailerUrl.contains(".m3u8") || trailerUrl.contains(".mpd")) {
        qDebug() << "TrailerFetcher: stream detected, starting ffmpeg conversion";
        startFfmpegConversion(trailerUrl);
        return;
    }

    // Прямой mp4 → скачиваем
    m_tmpFile.setFileName(m_tmpPath);
    if (!m_tmpFile.open(QIODevice::WriteOnly)) {
        qWarning() << "TrailerFetcher: cannot open tmp file:" << m_tmpPath;
        emit finished(QString());
        return;
    }

    QNetworkRequest req(trailerUrl);
    req.setRawHeader("User-Agent", kUserAgent);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    m_trailerReply = m_nam->get(req);
    connect(m_trailerReply, &QNetworkReply::readyRead,
            this, &TrailerFetcher::onTrailerReadyRead);
    connect(m_trailerReply, &QNetworkReply::downloadProgress,
            this, &TrailerFetcher::onTrailerProgress);
    connect(m_trailerReply, &QNetworkReply::finished,
            this, &TrailerFetcher::onTrailerFinished);

    QEventLoop loop;
    connect(m_trailerReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
}

// ── Запуск ffmpeg для конвертации HLS/DASH в mp4 ─────────────────────────

void TrailerFetcher::startFfmpegConversion(const QString &streamUrl)
{
    if (!QFile::exists(m_ffmpegPath)) {
        qWarning() << "TrailerFetcher: ffmpeg not found at" << m_ffmpegPath;
        emit finished(QString());
        return;
    }

    // Удалим возможный остаток от предыдущих попыток
    QFile::remove(m_destPath);
    QFile::remove(m_tmpPath); // не используется при конвертации, но на всякий случай

    // Парсинг длительности будем делать по мере появления вывода
    m_durationMs = 0;

    m_ffmpegProcess = new QProcess(this);
    connect(m_ffmpegProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &TrailerFetcher::onFfmpegFinished);
    connect(m_ffmpegProcess, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError e) {
                if (!m_ffmpegProcess) return;
                qWarning() << "TrailerFetcher: ffmpeg process error:" << m_ffmpegProcess->errorString();
                // Crashed (в т.ч. kill при отмене) придёт ещё и в finished —
                // результат отдаём там, иначе finished эмитился дважды
                if (e != QProcess::FailedToStart) return;
                m_ffmpegProcess->deleteLater();
                m_ffmpegProcess = nullptr;
                emit finished(QString());
            });
    connect(m_ffmpegProcess, &QProcess::readyReadStandardError,
            this, &TrailerFetcher::onFfmpegProgress);

    QStringList args;
    args << "-y"                     // перезаписывать выходной файл
         << "-i" << streamUrl        // входной поток
         << "-c" << "copy"           // копировать кодеки без перекодирования
         << "-movflags" << "+faststart"
         << "-bsf:a" << "aac_adtstoasc" // для совместимости с mp4
         << m_destPath;

    qDebug() << "TrailerFetcher: starting ffmpeg:" << m_ffmpegPath << args;
    m_ffmpegProcess->start(m_ffmpegPath, args);
}

// ── Чтение прогресса ffmpeg из stderr ─────────────────────────────────────

void TrailerFetcher::onFfmpegProgress()
{
    if (!m_ffmpegProcess) return;

    const QByteArray data = m_ffmpegProcess->readAllStandardError();
    const QString text = QString::fromUtf8(data);

    // Извлекаем общую длительность (один раз)
    if (m_durationMs == 0) {
        static const QRegularExpression durRe(QStringLiteral("Duration: (\\d{2}):(\\d{2}):(\\d{2})\\.(\\d{2})"));
        QRegularExpressionMatch match = durRe.match(text);
        if (match.hasMatch()) {
            const qint64 h  = match.captured(1).toLongLong();
            const qint64 m  = match.captured(2).toLongLong();
            const qint64 s  = match.captured(3).toLongLong();
            const qint64 cs = match.captured(4).toLongLong(); // сотые доли секунды
            m_durationMs = (h * 3600 + m * 60 + s) * 1000 + cs * 10;
        }
    }

    // Извлекаем текущее время "time=HH:MM:SS.ms"
    static const QRegularExpression timeRe(QStringLiteral("time=(\\d{2}):(\\d{2}):(\\d{2})\\.(\\d{2})"));
    QRegularExpressionMatchIterator it = timeRe.globalMatch(text);
    qint64 currentMs = 0;
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const qint64 h  = m.captured(1).toLongLong();
        const qint64 min = m.captured(2).toLongLong();
        const qint64 s  = m.captured(3).toLongLong();
        const qint64 cs = m.captured(4).toLongLong();
        currentMs = (h * 3600 + min * 60 + s) * 1000 + cs * 10;
        // last one wins
    }

    // Отправляем прогресс в процентах (0..1) как downloadProgress(perc*1000, 1000)
    if (m_durationMs > 0 && currentMs > 0) {
        double percent = qBound(0.0, double(currentMs) / m_durationMs, 1.0);
        emit downloadProgress(qint64(percent * 1000), 1000);
    } else {
        // Не знаем длительности, показываем неопределённый прогресс
        emit downloadProgress(0, 0);
    }
}

// ── Завершение работы ffmpeg ──────────────────────────────────────────────

void TrailerFetcher::onFfmpegFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_ffmpegProcess) return;

    m_ffmpegProcess->deleteLater();
    m_ffmpegProcess = nullptr;

    if (m_cancelled) {
        QFile::remove(m_destPath);
        emit finished(QString());
        return;
    }

    // Падение ffmpeg (CrashExit) — брак, даже если exitCode() вернул 0
    if (status == QProcess::NormalExit && exitCode == 0 && QFile::exists(m_destPath)) {
        qDebug() << "TrailerFetcher: ffmpeg conversion completed →" << m_destPath;
        emit finished(m_destPath);
    } else {
        qWarning() << "TrailerFetcher: ffmpeg failed, exit code" << exitCode;
        QFile::remove(m_destPath);
        emit finished(QString());
    }
}

// ── Скачивание прямого mp4 (стриминг) ─────────────────────────────────────

void TrailerFetcher::onTrailerReadyRead()
{
    if (!m_trailerReply || !m_tmpFile.isOpen() || m_writeFailed) return;
    const QByteArray chunk = m_trailerReply->readAll();
    if (m_tmpFile.write(chunk) != chunk.size()) {
        // Нет места на диске: раньше обрезанный файл сохранялся как трейлер
        qWarning() << "TrailerFetcher: write failed:" << m_tmpFile.errorString();
        m_writeFailed = true;
        m_trailerReply->abort();
    }
}

void TrailerFetcher::onTrailerProgress(qint64 received, qint64 total)
{
    emit downloadProgress(received, total);
}

void TrailerFetcher::onTrailerFinished()
{
    if (!m_trailerReply) return;

    const bool ok = !m_cancelled && !m_writeFailed
                    && m_trailerReply->error() == QNetworkReply::NoError;

    if (!ok) {
        qDebug() << "TrailerFetcher: download failed or cancelled:"
                 << m_trailerReply->errorString();
    }

    m_trailerReply->deleteLater();
    m_trailerReply = nullptr;

    finalize(ok);
}

// ── Атомарное переименование .tmp → .mp4 ─────────────────────────────────

void TrailerFetcher::finalize(bool success)
{
    m_tmpFile.close();

    if (!success) {
        QFile::remove(m_tmpPath);
        emit finished(QString());
        return;
    }

    if (QFile::exists(m_destPath))
        QFile::remove(m_destPath);

    if (!QFile::rename(m_tmpPath, m_destPath)) {
        qWarning() << "TrailerFetcher: rename failed:" << m_tmpPath << "→" << m_destPath;
        QFile::remove(m_tmpPath);
        emit finished(QString());
        return;
    }

    qDebug() << "TrailerFetcher: saved" << m_destPath;
    emit finished(m_destPath);
}

// ── Парсинг JSON appdetails ───────────────────────────────────────────────

QString TrailerFetcher::parseTrailerUrl(const QByteArray &json, const QString & /*appId*/) const
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "TrailerFetcher: JSON parse error:" << err.errorString();
        return {};
    }

    const QJsonObject root = doc.object();
    if (root.isEmpty()) return {};

    const QString appKey = root.keys().first();
    const QJsonObject appObj = root[appKey].toObject();
    if (!appObj["success"].toBool()) {
        qDebug() << "TrailerFetcher: appdetails success=false for" << appKey;
        return {};
    }

    const QJsonObject data   = appObj["data"].toObject();
    const QJsonArray  movies = data["movies"].toArray();
    if (movies.isEmpty()) {
        qDebug() << "TrailerFetcher: no movies for" << appKey;
        return {};
    }

    const QJsonObject first = movies[0].toObject();
    qDebug() << "TrailerFetcher: movies[0] keys:" << first.keys();

    // Новый формат: hls_h264, dash_h264, dash_av1
    // Старый формат: mp4.max, mp4.480
    // Прямой mp4 предпочтительнее: не требует ffmpeg. Раньше сначала брался
    // HLS-поток, и без tools/ffmpeg/ffmpeg.exe трейлер не скачивался вовсе,
    // даже если у игры был обычный mp4.
    QString url;
    const QJsonObject mp4 = first["mp4"].toObject();
    url = mp4["max"].toString();
    if (url.isEmpty())
        url = mp4["480"].toString();

    if (url.isEmpty() && QFile::exists(m_ffmpegPath)) {
        url = first["hls_h264"].toString();
        if (url.isEmpty())
            url = first["dash_h264"].toString();
    }
    if (url.isEmpty() && !QFile::exists(m_ffmpegPath)
        && (first.contains("hls_h264") || first.contains("dash_h264")))
        qWarning() << "TrailerFetcher: only HLS/DASH trailer available, but ffmpeg not found at"
                   << m_ffmpegPath;

    if (url.isEmpty())
        qDebug() << "TrailerFetcher: no playable URL found in movies[0]";

    return url;
}
