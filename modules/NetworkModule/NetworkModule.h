// modules/NetworkModule/NetworkModule.h
// ─────────────────────────────────────────────────────────────────────────────
// NetworkModule — единый модуль сетевого взаимодействия.
//   • TCP-сервер для приёма установок игр и обновлений MediaStation
//   • UDP-обнаружение
//   • Разовая отправка файлов логов на сервер по запросу из QML
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include "core/IModule.h"
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QProcess>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QQueue>
#include <QScopedPointer>
#include <QFile>
#include <QElapsedTimer>
#include <QStringList>

class EventBus;
class SettingsManager;

// ─────────────────────────────────────────────────────────────────────────────
// NetworkLogSender — разовая отправка папки логов на сервер.
//
// Жизненный цикл одной отправки:
//   1. QML вызывает discoverServers() → UDP-broadcast → foundServersChanged()
//   2. QML вызывает sendLogs(ip)     → 7zr упаковывает logs/ → TCP → сервер
//   3. Сигналы прогресса: statusChanged, progressChanged
//   4. По завершению: finished(success, message)
// ─────────────────────────────────────────────────────────────────────────────
class NetworkLogSender : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString     status        READ status        NOTIFY statusChanged)
    Q_PROPERTY(int         progress      READ progress      NOTIFY progressChanged)
    Q_PROPERTY(bool        busy          READ busy          NOTIFY busyChanged)
    Q_PROPERTY(QStringList foundServers  READ foundServers  NOTIFY foundServersChanged)

public:
    explicit NetworkLogSender(QObject *parent = nullptr);
    ~NetworkLogSender() override;

    // Путь к папке с лог-файлами (по умолчанию <appDir>/logs)
    void setLogsDir(const QString &path) { m_logsDir = path; }

    QString     status()      const { return m_status; }
    int         progress()    const { return m_progress; }
    bool        busy()        const { return m_busy; }
    QStringList foundServers() const { return m_foundServers; }

    void shutdown();

public slots:
    Q_INVOKABLE void discoverServers();             // UDP-поиск серверов в сети
    Q_INVOKABLE void sendLogs(const QString &ip);   // упаковать + отправить
    Q_INVOKABLE void cancelSend();

signals:
    void statusChanged();
    void progressChanged();
    void busyChanged();
    void foundServersChanged();
    void finished(bool success, const QString &message);

private slots:
    // Упаковка
    void onPackFinishedInternal(int result, const QString &archivePath);

    // Сеть
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError err);
    void onSocketReadyRead();
    void onBytesWritten(qint64 bytes);

private:
    enum Step { StepIdle, StepPacking, StepConnecting, StepSending } m_step = StepIdle;

    void setStatus(const QString &s);
    void setProgress(int p);
    void setBusy(bool b);
    void startPacking();
    void startConnecting();
    void sendHandshake();
    void sendChunk();
    void cleanup();

    // ── Состояние ─────────────────────────────────────────────────────────────
    QString     m_status;
    int         m_progress  = 0;
    bool        m_busy      = false;
    bool        m_cancelled = false;
    QString     m_logsDir;
    QString     m_archivePath;
    QString     m_serverIp;
    qint64      m_archiveSize = 0;
    qint64      m_bytesSent   = 0;
    QStringList m_foundServers;

    // ── Сеть ──────────────────────────────────────────────────────────────────
    QTcpSocket *m_socket     = nullptr;
    QUdpSocket *m_udpSocket  = nullptr;
    QFile      *m_archiveFile = nullptr;
    QByteArray  m_incoming;
    QString     m_tmpLogsDir;
    QElapsedTimer m_speedTimer;
    qint64        m_lastBytes = 0;
    qint64        m_lastMs    = 0;

    static constexpr quint16 kLogsPort        = 12347;
    static constexpr quint16 kDiscoverPort     = 12346;
    static constexpr quint16 kLogsDiscoverPort = 12348;   // <-- добавлено
    static constexpr qint64  kChunkSize        = 2 * 1024 * 1024;
};


// ─────────────────────────────────────────────────────────────────────────────
// NetworkModule
// ─────────────────────────────────────────────────────────────────────────────
class NetworkModule : public QObject, public IModule
{
    Q_OBJECT

    // NetworkLogSender доступен в QML как NetworkListener.logSender
    // (или регистрируется отдельно через setContextProperty)
    Q_PROPERTY(QObject* logSender READ logSenderObject CONSTANT)

public:
    explicit NetworkModule(QObject *parent = nullptr);
    ~NetworkModule() override;

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId() const override { return "network"; }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown() override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return "NetworkListener"; }

    QObject *logSenderObject() const { return m_logSender; }

public slots:
    // ── Существующие слоты (обратная совместимость) ─────────────────────────
    void acceptInstall(QTcpSocket *socket, const QString &tileId);
    void rejectInstall(QTcpSocket *socket);
    void cancelInstallByTileId(const QString &tileId);
    void cancelInstall(QTcpSocket *socket);

signals:
    // ── Существующие сигналы (серверная часть) ──────────────────────────────
    void showInstallDialog(const QString &title,            const QString &kind,
                           const QString &exeRelativePath,  const QString &iconRelativePath,
                           QTcpSocket *socket);
    void installProgress(const QString &tileId, qreal progress, const QString &phase);
    void serverCancelledInstall(const QString &tileId);

private slots:
    void newConnection();
    void readyRead();
    void socketDisconnected();
    void processUdpDatagrams();
    void onSettingsChanged(const QString &key, const QVariant &value);

private:
    void initServer();
    void initLogSender();

    void receiveArchiveData(QTcpSocket *socket, const QByteArray &data);
    void extractAndInstall(QTcpSocket *socket);
    void finalizeUpdate(QTcpSocket *socket);
    void cleanupClient(QTcpSocket *socket, bool sendCancel = false);
    QString prepareUpdateArchivePath() const;
    QString prepareInstallDir(const QString &tileId, const QString &title) const;
    static QString generateUniqueFolderName(const QString &baseName,
                                            const QString &gamesDirPath);

    EventBus        *m_bus      = nullptr;
    SettingsManager *m_settings = nullptr;
    QString          m_installPath;

    NetworkLogSender *m_logSender = nullptr;

    QTcpServer *m_server    = nullptr;
    QUdpSocket *m_udpSocket = nullptr;

    enum ClientPhase {
        PhaseNegotiating,
        PhasePackProgress,
        PhaseReceiving,
        PhaseExtracting,
        PhaseUpdate
    };

    struct ClientState {
        ClientPhase        phase = PhaseNegotiating;
        QByteArray         buffer;
        qint64             expectedSize = 0;
        qint64             receivedBytes = 0;
        QJsonObject        metadata;
        QString            tileId;
        QString            tempArchivePath;
        QString            gameRootPath;
        QPointer<QProcess> extractProcess;
        QFile             *archiveFile = nullptr;
        bool               cancelled = false;
        bool               isUpdate = false;
    };

    QMap<QTcpSocket *, ClientState> m_clients;
};
