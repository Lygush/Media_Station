// modules/NetworkModule/NetworkModule.cpp

#include "NetworkModule.h"
#include "core/EventBus.h"
#include "SettingsManager.h"
#include "LoggerModule.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDateTime>
#include <QTimer>

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
    m_stopped = true;
    if (m_idleTimer) m_idleTimer->stop();
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
    connect(m_server, &QTcpServer::newConnection, this, &NetworkModule::newConnection);

    m_udpSocket = new QUdpSocket(this);
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &NetworkModule::processUdpDatagrams);

    // Клиенты, которые замолчали на середине передачи, не должны висеть вечно
    // (открытый временный архив на гигабайты + запись в m_clients).
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(10'000);
    connect(m_idleTimer, &QTimer::timeout, this, &NetworkModule::checkIdleClients);
    m_idleTimer->start();

    startListening();
}

// Раньше при занятом порте (остался прошлый экземпляр, порт держит другая
// программа) в лог писалось предупреждение, и приём игр/обновлений молча не
// работал до перезапуска. Теперь пробуем снова каждые 10 с.
void NetworkModule::startListening()
{
    if (m_stopped) return;

    bool allUp = true;

    if (!m_server->isListening()) {
        if (m_server->listen(QHostAddress::Any, 12345)) {
            log("INFO", "TCP 12345: слушаю");
        } else {
            allUp = false;
            log("WARN", QString("TCP 12345 недоступен: %1 — повтор через 10 с")
                            .arg(m_server->errorString()));
        }
    }

    if (m_udpSocket->state() != QAbstractSocket::BoundState) {
        if (m_udpSocket->bind(QHostAddress::Any, 12346,
                              QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            log("INFO", "UDP 12346: слушаю");
        } else {
            allUp = false;
            log("WARN", QString("UDP 12346 недоступен: %1 — повтор через 10 с")
                            .arg(m_udpSocket->errorString()));
        }
    }

    if (!allUp)
        QTimer::singleShot(10'000, this, &NetworkModule::startListening);
}

void NetworkModule::checkIdleClients()
{
    constexpr qint64 kIdleLimitMs = 60'000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QList<QTcpSocket *> stale;
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        const ClientState &st = it.value();
        // Упаковка на стороне отправителя может долго идти без данных,
        // распаковка идёт у нас — эти фазы не трогаем.
        const bool watched = st.phase == PhaseNegotiating
                          || st.phase == PhaseReceiving
                          || st.phase == PhaseUpdate;
        if (watched && st.lastActivityMs > 0 && now - st.lastActivityMs > kIdleLimitMs)
            stale.append(it.key());
    }
    for (QTcpSocket *s : std::as_const(stale)) {
        if (!m_clients.contains(s)) continue;
        log("WARN", QString("Клиент %1 не отвечает более 60 с — соединение закрыто")
                        .arg(s->peerAddress().toString()));
        cleanupClient(s, true);   // отправит CANCEL, уберёт плитку и закроет сокет
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

// ── Логирование ──────────────────────────────────────────────────────────────
// Раньше NetworkModule писал только через qDebug/qWarning, которые нигде не
// сохраняются в продакшене — из-за этого сетевые сбои были не видны в
// logs/app.log. Теперь дублируем в LoggerModule, как это делает InstallManager.
void NetworkModule::log(const char *level, const QString &msg) const
{
    Logger::write(level, "NetworkModule", msg);
}

// ─── Реализация серверных методов вставляется из NetworkServer.cpp ─────────
// (остальной код сервера без изменений)
