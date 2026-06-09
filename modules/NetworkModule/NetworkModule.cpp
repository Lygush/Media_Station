// modules/NetworkModule/NetworkModule.cpp

#include "NetworkModule.h"
#include "core/EventBus.h"
#include "SettingsManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDateTime>

NetworkModule::NetworkModule(QObject *parent)
    : QObject(parent)
    , m_logSender(new NetworkLogSender(this))
{}

NetworkModule::~NetworkModule() = default;

void NetworkModule::initialize(EventBus *bus, SettingsManager *settings)
{
    m_bus      = bus;
    m_settings = settings;
    m_installPath = settings->installPath();

    connect(bus, &EventBus::settingChanged, this, &NetworkModule::onSettingsChanged);

    initServer();
    initLogSender();

    qDebug() << "[NetworkModule] Initialized";
}

void NetworkModule::shutdown()
{
    if (m_server) {
        m_server->close();
        for (QTcpSocket *socket : m_clients.keys())
            cleanupClient(socket, false);
        m_clients.clear();
    }
    if (m_udpSocket)
        m_udpSocket->close();

    if (m_logSender)
        m_logSender->shutdown();

    qDebug() << "[NetworkModule] Shutdown complete";
}

void NetworkModule::initServer()
{
    m_server = new QTcpServer(this);
    if (m_server->listen(QHostAddress::Any, 12345)) {
        connect(m_server, &QTcpServer::newConnection, this, &NetworkModule::newConnection);
    } else {
        qWarning() << "[NetworkModule] Failed to listen on TCP port 12345:"
                   << m_server->errorString();
    }

    m_udpSocket = new QUdpSocket(this);
    if (m_udpSocket->bind(QHostAddress::Any, 12346)) {
        connect(m_udpSocket, &QUdpSocket::readyRead, this, &NetworkModule::processUdpDatagrams);
    } else {
        qWarning() << "[NetworkModule] Failed to bind UDP port 12346:"
                   << m_udpSocket->errorString();
    }
}

void NetworkModule::initLogSender()
{
    // Путь к папке логов — рядом с exe
    m_logSender->setLogsDir(QCoreApplication::applicationDirPath() + "/logs");
}

void NetworkModule::onSettingsChanged(const QString &key, const QVariant &value)
{
    if (key == QLatin1String("installPath"))
        m_installPath = value.toString();
}

// ─── Реализация серверных методов вставляется из NetworkServer.cpp ─────────
// (остальной код сервера без изменений)
