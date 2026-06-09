// modules/GameInfoModule/GameInfoModule.h
#pragma once

#include "core/IModule.h"
#include "tools/LlamaClient.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <QMutex>
#include <QWaitCondition>

class EventBus;
class SettingsManager;

class GameInfoModule : public QObject, public IModule
{
    Q_OBJECT
    Q_PROPERTY(bool    fetching READ fetching NOTIFY fetchingChanged)
    Q_PROPERTY(QString stage    READ stage    NOTIFY stageChanged)

public:
    explicit GameInfoModule(QObject *parent = nullptr);
    ~GameInfoModule() override;

    // IModule
    QString  moduleId() const override { return QStringLiteral("gameinfo"); }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown() override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return QStringLiteral("GameInfoModule"); }

    bool    fetching() const { return m_fetching; }
    QString stage()    const { return m_stage; }

    Q_INVOKABLE void requestFetch(const QString &tileId,
                                  const QString &title,
                                  const QString &installFolder);

    Q_INVOKABLE void selectCandidate(const QString &tileId, const QString &appId);

signals:
    void infoFetched(const QString &tileId, const QVariantMap &metadata);
    void fetchingChanged();
    void stageChanged();
    void fetchError(const QString &tileId, const QString &reason);
    void candidatesReady(const QString &tileId, const QVariantList &candidates);
    void fieldsReady(const QString &tileId, const QVariantMap &fields);  // короткие поля после запроса 1
    void tokenGenerated(const QString &token);                           // каждый токен описания
    void jsonTokenGenerated(const QString &token);                       // каждый токен первого запроса (JSON)

private:
    void onFetchRequested(const QVariantMap &data);
    void doFetch(const QString &tileId,
                 const QString &title,
                 const QString &installFolder);
    void saveToGameInfo(const QString &installFolder, const QVariantMap &meta,
                        const QString &steamAppId);
    void notifyUser(const QString &msg, bool isError = false);
    void setStage(const QString &stage);

    // Логирование через EventBus → LoggerModule. Безопасно из любого потока.
    void logMsg(const char *level, const QString &message);

    // LLM: два запроса подряд — короткие поля + description со стримингом.
    // Промпты и парсинг инкапсулированы здесь, LlamaClient — чистый инструмент.
    QVariantMap generateGameInfo(const QString &title, const QString &sourceText,
                                 const LlamaClient::TokenCallback &onDescriptionToken = nullptr,
                                 const LlamaClient::TokenCallback &onJsonToken = nullptr);
    static QVariantMap parseGameInfoJson(const QString &raw);

    EventBus        *m_bus      = nullptr;
    SettingsManager *m_settings = nullptr;
    LlamaClient     *m_llama    = nullptr;
    bool             m_fetching = false;
    QString          m_stage;

    QMutex         m_candidateMutex;
    QWaitCondition m_candidateCond;
    QString        m_selectedAppId;
    QString        m_pendingTileId;
    bool           m_aborted = false;  // выставляется в shutdown() чтобы разбудить wait
};
