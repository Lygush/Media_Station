// modules/GameMediaModule/GameMediaModule.cpp

#include "GameMediaModule.h"
#include "CoverFetcher.h"
#include "TrailerFetcher.h"
#include "SteamAppList.h"

#include "core/EventBus.h"
#include "SettingsManager.h"
#include "tools/AtomicFile.h"

#include <QThread>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

// ─────────────────────────────────────────────────────────────────────────────

GameMediaModule::GameMediaModule(QObject *parent)
    : QObject(parent)
{}

GameMediaModule::~GameMediaModule()
{
    shutdown();
}

// ── IModule ───────────────────────────────────────────────────────────────────

void GameMediaModule::initialize(EventBus *bus, SettingsManager *settings)
{
    m_bus      = bus;
    m_settings = settings;

    // Подписка на универсальный канал шины
    connect(m_bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data) {
                if (mod != QLatin1String("gamemedia")) return;
                if (ev  != QLatin1String("fetch"))     return;

                const QString tileId        = data["tileId"].toString();
                const QString installFolder = data["installFolder"].toString();
                const QString steamAppId    = data["steamAppId"].toString();

                if (tileId.isEmpty() || installFolder.isEmpty() || steamAppId.isEmpty()) {
                    qWarning() << "GameMediaModule: incomplete moduleEvent data";
                    return;
                }

                requestFetch(tileId, data["title"].toString(), installFolder, steamAppId);
            });

    qDebug() << "GameMediaModule: initialized";
    // Загрузка локальной БД (SteamAppList) перенесена в GameInfoModule::doFetch —
    // запускается только при реальном запросе информации об игре, не при старте.
}

void GameMediaModule::shutdown()
{
    // Отменяем фоновое скачивание списка приложений
    SteamAppList::instance().cancel();
    // Отменяем загрузки, потоки сами завершатся через quit/wait
    if (m_coverFetcher)
        m_coverFetcher->cancel();
    if (m_trailerFetcher)
        m_trailerFetcher->cancel();

    // Дожидаемся потоков (HttpClient реагирует на отмену за ~100 мс, ffmpeg убивается)
    for (QThread *t : {m_coverThread.data(), m_trailerThread.data()}) {
        if (!t) continue;
        t->quit();
        if (!t->wait(5000)) {
            qWarning() << "GameMediaModule: fetch thread did not stop, terminating";
            t->terminate();
            t->wait(1000);
        }
    }
}

// ── Публичный API (QML + EventBus) ───────────────────────────────────────────

void GameMediaModule::requestFetch(const QString &tileId,
                                   const QString &/*title*/,
                                   const QString &installFolder,
                                   const QString &steamAppId)
{
    if (m_busy) {
        notify("Загрузка медиа уже выполняется, дождитесь завершения.", true);
        return;
    }

    if (steamAppId.isEmpty()) {
        notify("Steam App ID не указан — невозможно загрузить медиа.", true);
        return;
    }

    startFetch(tileId, installFolder, steamAppId);
}

void GameMediaModule::cancelTrailer()
{
    if (m_trailerFetcher)
        m_trailerFetcher->cancel();
}

// ── Запуск загрузки ───────────────────────────────────────────────────────────

void GameMediaModule::startFetch(const QString &tileId,
                                 const QString &installFolder,
                                 const QString &steamAppId)
{
    m_busy                = true;
    m_currentTileId       = tileId;
    m_currentInstallFolder= installFolder;
    m_currentSteamAppId   = steamAppId;

    m_resultCoverPath    = {};
    m_resultBackdropPath = {};
    m_resultTrailerPath  = {};
    m_coversDone         = false;
    m_trailerDone        = false;

    // Создать папку launcher/media/ если нет
    const QString mediaDir = installFolder + "/launcher/media";
    QDir().mkpath(mediaDir);

    // ── CoverFetcher в потоке ─────────────────────────────────────────────────
    {
        auto *thread = new QThread(this);
        m_coverThread = thread;
        auto *fetcher = new CoverFetcher();
        m_coverFetcher = fetcher;
        fetcher->moveToThread(thread);

        // Контекст — сам fetcher (живёт в рабочем потоке). Раньше контекстом был
        // this (GUI-поток), и вся блокирующая загрузка выполнялась в GUI —
        // интерфейс замирал на время скачивания обложек и трейлера.
        connect(thread, &QThread::started, fetcher, [fetcher, mediaDir, steamAppId]() {
            fetcher->fetch(mediaDir, steamAppId);
        });
        connect(m_coverFetcher, &CoverFetcher::finished,
                this,           &GameMediaModule::onCoversDone,
                Qt::QueuedConnection);
        connect(m_coverFetcher, &CoverFetcher::finished, thread, &QThread::quit);
        connect(thread, &QThread::finished, m_coverFetcher, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread,         &QObject::deleteLater);

        m_fetchingCovers = true;
        emit fetchingCoversChanged();

        thread->start();
    }

    // ── TrailerFetcher в потоке ───────────────────────────────────────────────
    {
        auto *thread = new QThread(this);
        m_trailerThread = thread;
        auto *fetcher = new TrailerFetcher();
        m_trailerFetcher = fetcher;
        fetcher->moveToThread(thread);

        connect(thread, &QThread::started, fetcher, [fetcher, mediaDir, steamAppId]() {
            fetcher->fetch(mediaDir, steamAppId);
        });
        connect(m_trailerFetcher, &TrailerFetcher::finished,
                this,             &GameMediaModule::onTrailerDone,
                Qt::QueuedConnection);
        connect(m_trailerFetcher, &TrailerFetcher::downloadProgress,
                this,             &GameMediaModule::onTrailerProgress,
                Qt::QueuedConnection);
        connect(m_trailerFetcher, &TrailerFetcher::finished, thread, &QThread::quit);
        connect(thread, &QThread::finished, m_trailerFetcher, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread,           &QObject::deleteLater);

        m_fetchingTrailer  = true;
        m_trailerProgress  = 0.0;
        emit fetchingTrailerChanged();
        emit trailerProgressChanged();

        thread->start();
    }
}

// ── Слоты завершения ──────────────────────────────────────────────────────────

void GameMediaModule::onCoversDone(const QString &coverPath, const QString &backdropPath)
{
    m_coverFetcher       = nullptr;   // уже будет deleteLater'd потоком
    m_resultCoverPath    = coverPath;
    m_resultBackdropPath = backdropPath;
    m_coversDone         = true;

    m_fetchingCovers = false;
    emit fetchingCoversChanged();

    if (coverPath.isEmpty() && backdropPath.isEmpty())
        qDebug() << "GameMediaModule: covers not found for" << m_currentSteamAppId;
    else
        qDebug() << "GameMediaModule: covers done — cover:" << coverPath
                 << "backdrop:" << backdropPath;

    checkAllDone();
}

void GameMediaModule::onTrailerDone(const QString &trailerPath)
{
    m_trailerFetcher    = nullptr;
    m_resultTrailerPath = trailerPath;
    m_trailerDone       = true;

    m_fetchingTrailer = false;
    m_trailerProgress = trailerPath.isEmpty() ? 0.0 : 1.0;
    emit fetchingTrailerChanged();
    emit trailerProgressChanged();

    if (trailerPath.isEmpty())
        qDebug() << "GameMediaModule: trailer not found for" << m_currentSteamAppId;
    else
        qDebug() << "GameMediaModule: trailer done —" << trailerPath;

    checkAllDone();
}

void GameMediaModule::onTrailerProgress(qint64 received, qint64 total)
{
    if (total <= 0) return;

    m_trailerProgress = qreal(received) / qreal(total);
    emit trailerProgressChanged();
    emit trailerDownloadProgress(m_currentTileId, m_trailerProgress);
}

// ── Проверка завершения обоих фетчеров ────────────────────────────────────────

void GameMediaModule::checkAllDone()
{
    if (!m_coversDone || !m_trailerDone)
        return;

    m_busy = false;

    const bool anythingFetched = !m_resultCoverPath.isEmpty()
                                 || !m_resultBackdropPath.isEmpty()
                                 || !m_resultTrailerPath.isEmpty();

    if (anythingFetched) {
        saveToGameInfo(m_currentInstallFolder,
                       m_resultCoverPath,
                       m_resultBackdropPath,
                       m_resultTrailerPath);

        QVariantMap media;
        if (!m_resultCoverPath.isEmpty())
            media["coverPath"]    = m_resultCoverPath;
        if (!m_resultBackdropPath.isEmpty())
            media["backdropPath"] = m_resultBackdropPath;
        if (!m_resultTrailerPath.isEmpty())
            media["trailerPath"]  = m_resultTrailerPath;

        emit mediaFetched(m_currentTileId, media);

        // Уведомляем другие модули через шину
        QVariantMap eventData = media;
        eventData["tileId"]        = m_currentTileId;
        eventData["installFolder"] = m_currentInstallFolder;
        if (m_bus) emit m_bus->moduleEvent("gamemedia", "fetched", eventData);

        notify("Медиа успешно загружены.");
    } else {
        notify("Не удалось загрузить медиа. Проверьте Steam App ID.", true);
    }
}

// ── Запись путей в game_info.json ─────────────────────────────────────────────

void GameMediaModule::saveToGameInfo(const QString &installFolder,
                                     const QString &coverPath,
                                     const QString &backdropPath,
                                     const QString &trailerPath) const
{
    const QString infoPath = installFolder + "/game_info.json";
    QFile f(infoPath);

    QJsonObject obj;

    // Читаем существующий файл, если есть
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray raw = f.readAll();
        f.close();
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            obj = doc.object();
    }

    // Обновляем только заполненные поля, не трогаем остальные
    if (!coverPath.isEmpty())
        obj["coverPath"] = coverPath;
    if (!backdropPath.isEmpty())
        obj["backdropPath"] = backdropPath;
    if (!trailerPath.isEmpty())
        obj["trailerPath"] = trailerPath;
    obj["mediaFetched"] = true;

    if (!AtomicFile::writeJson(infoPath, QJsonDocument(obj)))
        qWarning() << "GameMediaModule: failed to update game_info.json at" << infoPath;
    else
        qDebug() << "GameMediaModule: game_info.json updated at" << infoPath;
}

// ── Уведомление пользователю ──────────────────────────────────────────────────

void GameMediaModule::notify(const QString &msg, bool isError) const
{
    if (!m_bus) return;

    emit m_bus->moduleEvent("notification", "show", {
        {"message",     msg},
        {"level",       isError ? QStringLiteral("error") : QStringLiteral("info")},
        {"autoDismiss", true},
        {"duration",    4000}
    });
}
