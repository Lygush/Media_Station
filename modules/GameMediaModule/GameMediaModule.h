// modules/GameMediaModule/GameMediaModule.h
// ─────────────────────────────────────────────────────────────────────────────
// GameMediaModule — скачивание обложек и трейлера игры со Steam CDN.
//
// Подписывается на moduleEvent("gamemedia", "fetch", {...}).
// По завершении эмитит moduleEvent("gamemedia", "fetched", {...})
// и QML-сигнал mediaFetched(tileId, media).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/IModule.h"
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QString>
#include <QVariantMap>

class CoverFetcher;
class TrailerFetcher;
class EventBus;
class SettingsManager;

class GameMediaModule : public QObject, public IModule
{
    Q_OBJECT

    // true пока идёт загрузка обложек
    Q_PROPERTY(bool  fetchingCovers  READ fetchingCovers  NOTIFY fetchingCoversChanged)
    // true пока идёт загрузка трейлера
    Q_PROPERTY(bool  fetchingTrailer READ fetchingTrailer NOTIFY fetchingTrailerChanged)
    // прогресс трейлера 0.0–1.0
    Q_PROPERTY(qreal trailerProgress READ trailerProgress NOTIFY trailerProgressChanged)

public:
    explicit GameMediaModule(QObject *parent = nullptr);
    ~GameMediaModule() override;

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId() const override { return QStringLiteral("gamemedia"); }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown()  override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return QStringLiteral("GameMediaModule"); }

    // ── Свойства ──────────────────────────────────────────────────────────────
    bool  fetchingCovers()  const { return m_fetchingCovers; }
    bool  fetchingTrailer() const { return m_fetchingTrailer; }
    qreal trailerProgress() const { return m_trailerProgress; }

    // ── Вызов из QML (кнопка «Получить медиа») ───────────────────────────────
    Q_INVOKABLE void requestFetch(const QString &tileId,
                                  const QString &title,
                                  const QString &installFolder,
                                  const QString &steamAppId);

    // ── Отмена загрузки трейлера ─────────────────────────────────────────────
    Q_INVOKABLE void cancelTrailer();

signals:
    // Для QML — обновить coverPath/backdropPath/trailerPath в gameData
    void mediaFetched(const QString &tileId, const QVariantMap &media);

    // Уведомление о прогрессе трейлера (0.0–1.0) — для QML прогресс-бара
    void trailerDownloadProgress(const QString &tileId, qreal progress);

    void fetchingCoversChanged();
    void fetchingTrailerChanged();
    void trailerProgressChanged();

private slots:
    // Результат CoverFetcher
    void onCoversDone(const QString &coverPath, const QString &backdropPath);

    // Результат TrailerFetcher
    void onTrailerDone(const QString &trailerPath);

    // Прогресс трейлера
    void onTrailerProgress(qint64 received, qint64 total);

private:
    // Запуск загрузки для одной игры
    void startFetch(const QString &tileId,
                    const QString &installFolder,
                    const QString &steamAppId);

    // Вызывается когда оба фетчера завершились
    void checkAllDone();

    // Сохранить пути в game_info.json
    void saveToGameInfo(const QString &installFolder,
                        const QString &coverPath,
                        const QString &backdropPath,
                        const QString &trailerPath) const;

    // Уведомление пользователю через NotificationModule
    void notify(const QString &msg, bool isError = false) const;

    // ── Состояние текущей загрузки ────────────────────────────────────────────
    QString m_currentTileId;
    QString m_currentInstallFolder;
    QString m_currentSteamAppId;

    // Результаты, накапливаемые по мере завершения фетчеров
    QString m_resultCoverPath;
    QString m_resultBackdropPath;
    QString m_resultTrailerPath;

    bool  m_coversDone   = false;
    bool  m_trailerDone  = false;
    bool  m_busy         = false;

    bool  m_fetchingCovers  = false;
    bool  m_fetchingTrailer = false;
    qreal m_trailerProgress = 0.0;

    CoverFetcher   *m_coverFetcher   = nullptr;
    TrailerFetcher *m_trailerFetcher = nullptr;
    // Рабочие потоки фетчеров — нужно дождаться их в shutdown(), иначе
    // QThread уничтожается работающим (аварийное завершение при выходе)
    QPointer<QThread> m_coverThread;
    QPointer<QThread> m_trailerThread;

    EventBus        *m_bus      = nullptr;
    SettingsManager *m_settings = nullptr;
};
