// modules/NetworkModule/NetworkLogSender.cpp
#include "NetworkModule.h"
#include "tools/SevenZip.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QNetworkInterface>
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <QUuid>
#include <QThreadPool>
#include <QPointer>

// ─── Конструктор / деструктор ───────────────────────────────────────────────
NetworkLogSender::NetworkLogSender(QObject *parent)
    : QObject(parent)
    , m_udpSocket(new QUdpSocket(this))
{
    m_udpSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress);
    m_logsDir     = QCoreApplication::applicationDirPath() + "/logs";
    m_archivePath = QDir::temp().absoluteFilePath("mediacenter_logs_upload.7z");
    setStatus("Готов");
}

NetworkLogSender::~NetworkLogSender()
{
    m_cancelled = true;
    cleanup();
}

void NetworkLogSender::shutdown()
{
    m_cancelled = true;
    cleanup();
}

// ─── discoverServers ─────────────────────────────────────────────────────────
void NetworkLogSender::discoverServers()
{
    if (m_busy) return;
    // Повторный поиск во время текущего — перезапускаем, а не плодим таймеры/обработчики
    if (m_discoverTimer) {
        m_discoverTimer->stop();
        m_discoverTimer->deleteLater();
        m_discoverTimer = nullptr;
    }
    if (m_discoverConn) disconnect(m_discoverConn);
    m_foundServers.clear();
    emit foundServersChanged();
    setStatus("Поиск серверов...");

    QByteArray datagram = "DISCOVER_MEDIACENTER";

    // Отправляем на оба порта: стандартный (12346) и порт лог-сервера (12348)
    m_udpSocket->writeDatagram(datagram, QHostAddress::Broadcast, kDiscoverPort);
    m_udpSocket->writeDatagram(datagram, QHostAddress::Broadcast, kLogsDiscoverPort);

    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if ((iface.flags() & QNetworkInterface::IsUp) &&
            !(iface.flags() & QNetworkInterface::IsLoopBack)) {
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                QHostAddress bc = entry.broadcast();
                if (!bc.isNull() && bc.protocol() == QAbstractSocket::IPv4Protocol) {
                    m_udpSocket->writeDatagram(datagram, bc, kDiscoverPort);
                    m_udpSocket->writeDatagram(datagram, bc, kLogsDiscoverPort);
                }
            }
        }
    }

    QTimer *timer = new QTimer(this);
    m_discoverTimer = timer;
    timer->setSingleShot(true);
    m_discoverConn = connect(m_udpSocket, &QUdpSocket::readyRead, this, [this]() {
        while (m_udpSocket->hasPendingDatagrams()) {
            QByteArray reply(static_cast<int>(m_udpSocket->pendingDatagramSize()), 0);
            QHostAddress sender;
            m_udpSocket->readDatagram(reply.data(), reply.size(), &sender);
            QString ip = sender.toString();
            if (ip.startsWith("::ffff:")) ip = ip.mid(7);
            // Принимаем только ответы от лог-серверов (содержат "logs")
            if (reply.contains("logs") && !m_foundServers.contains(ip)) {
                m_foundServers.append(ip);
                emit foundServersChanged();
            }
        }
    });
    connect(timer, &QTimer::timeout, this, [this, timer]() {
        disconnect(m_discoverConn);
        if (m_discoverTimer == timer) m_discoverTimer = nullptr;
        timer->deleteLater();
        setStatus(m_foundServers.isEmpty()
                      ? "Серверы не найдены"
                      : QString("Найдено: %1").arg(m_foundServers.join(", ")));
    });
    timer->start(3000);
}

// ─── sendLogs / cancelSend ───────────────────────────────────────────────────
void NetworkLogSender::sendLogs(const QString &ip)
{
    if (m_busy) return;
    if (!QDir(m_logsDir).exists()) {
        emit finished(false, QString("Папка с логами не найдена:\n%1").arg(m_logsDir));
        return;
    }
    m_serverIp    = ip.trimmed();
    m_cancelled   = false;
    m_bytesSent   = 0;
    m_archiveSize = 0;
    m_incoming.clear();
    m_tmpLogsDir.clear();

    setBusy(true);
    setProgress(0);
    setStatus("Копирование логов...");
    m_step = StepPacking;

    // 1. Создаём временную папку для копий логов
    m_tmpLogsDir = QDir::temp().absoluteFilePath(
        "mediacenter_logs_tmp_" + QUuid::createUuid().toString(QUuid::Id128));
    QDir tmpDir(m_tmpLogsDir);
    if (!tmpDir.mkpath(".")) {
        cleanup();
        setBusy(false);
        m_step = StepIdle;
        emit finished(false, "Не удалось создать временную папку для копирования логов.");
        return;
    }

    // 2. Копируем все файлы из m_logsDir во временную папку (рекурсивно)
    QDir srcDir(m_logsDir);
    const auto files = srcDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    int copiedCount = 0;
    for (const QFileInfo &fi : files) {
        if (fi.isFile()) {
            const QString destPath = tmpDir.filePath(fi.fileName());
            if (QFile::copy(fi.absoluteFilePath(), destPath)) {
                ++copiedCount;
            } else {
                qDebug() << "[LogSender] Cannot copy" << fi.fileName()
                << "(in use) — skipping";
            }
        } else if (fi.isDir()) {
            const QString subSrc = fi.absoluteFilePath();
            const QString subDst = tmpDir.filePath(fi.fileName());
            QDir().mkpath(subDst);
            const auto subFiles = QDir(subSrc).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
            for (const QFileInfo &subFi : subFiles) {
                if (QFile::copy(subFi.absoluteFilePath(), subDst + "/" + subFi.fileName()))
                    ++copiedCount;
            }
        }
    }

    qDebug() << "[LogSender] Copied" << copiedCount << "log files to" << m_tmpLogsDir;
    if (copiedCount == 0) {
        setStatus("Не удалось скопировать файлы логов (все заняты). Попытка упаковки оригиналов...");
    }

    QFile::remove(m_archivePath);
    startPacking();
}

void NetworkLogSender::cancelSend()
{
    if (!m_busy) return;
    m_cancelled = true;
    cleanup();
    setProgress(0);
    setBusy(false);
    m_step = StepIdle;
    setStatus("Отменено");
}

// ─── Упаковка через SevenZip ──────────────────────────────────────────────────
void NetworkLogSender::startPacking()
{
    QString sourceDir = m_logsDir;
    if (!m_tmpLogsDir.isEmpty() && QDir(m_tmpLogsDir).exists()) {
        QDir td(m_tmpLogsDir);
        if (!td.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty()) {
            sourceDir = m_tmpLogsDir;
        } else {
            QDir().rmdir(m_tmpLogsDir);
            m_tmpLogsDir.clear();
        }
    }

    qDebug() << "[LogSender] Packing from source dir:" << sourceDir;

    if (!SevenZip::isAvailable()) {
        cleanup();
        setBusy(false);
        m_step = StepIdle;
        emit finished(false,
                      QString("Архиватор 7zr.exe не найден.\n"
                              "Поместите 7zr.exe в папку tools/ или рядом с приложением."));
        return;
    }

    // SevenZip::compress — блокирующий, запускаем в пуле потоков.
    // Прогресс и завершение передаём обратно в GUI-поток через invokeMethod.
    const QString archivePath = m_archivePath;
    // Отмена и закрытие приложения во время упаковки: раньше поток продолжал
    // писать архив и обращался к уже удалённому объекту.
    m_packCancel = std::make_shared<std::atomic<bool>>(false);
    const auto cancelFlag = m_packCancel;
    const QPointer<NetworkLogSender> self(this);
    QThreadPool::globalInstance()->start([self, sourceDir, archivePath, cancelFlag]() {

        const SevenZip::Result r = SevenZip::compress(
            sourceDir,
            QStringLiteral("*"),    // архивируем всё в sourceDir
            archivePath,
            cancelFlag.get(),
            [self](int pct, const QString &) {
                if (!self) return;
                // Прогресс архивации занимает 0–40% общего прогресса
                QMetaObject::invokeMethod(self.data(), [self, pct]() {
                    if (self) self->setProgress(pct * 40 / 100);
                }, Qt::QueuedConnection);
            });

        if (cancelFlag->load()) { QFile::remove(archivePath); return; }
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, r, archivePath]() {
            if (self) self->onPackFinishedInternal(r, archivePath);
        }, Qt::QueuedConnection);
    });
}

// Вызывается из GUI-потока после завершения фонового сжатия
void NetworkLogSender::onPackFinishedInternal(int result, const QString &archivePath)
{
    if (m_cancelled || m_step != StepPacking) return;

    const auto r = static_cast<SevenZip::Result>(result);

    if (r == SevenZip::NotFound || r == SevenZip::StartFail) {
        cleanup();
        setBusy(false);
        m_step = StepIdle;
        emit finished(false, QString("Не удалось запустить архиватор: %1")
                                 .arg(SevenZip::resultToString(r)));
        return;
    }

    if (r == SevenZip::ExitError) {
        // exitCode == 1 от 7zr означает предупреждения (заблокированные файлы) — допускаем
        // Проверяем что архив всё же создан
        const qint64 size = QFileInfo(archivePath).size();
        if (size == 0) {
            cleanup();
            setBusy(false);
            m_step = StepIdle;
            emit finished(false, "Ошибка упаковки: архив пуст или не создан.");
            return;
        }
        // Архив создан несмотря на предупреждения — продолжаем со статусом
        m_archiveSize = size;
        setProgress(40);
        setStatus(QString("Архив готов (%1 КБ). Часть файлов пропущена (заняты).")
                      .arg(m_archiveSize / 1024));
        m_step = StepConnecting;
        startConnecting();
        return;
    }

    m_archiveSize = QFileInfo(archivePath).size();
    if (m_archiveSize == 0) {
        const QString files = QDir(m_logsDir)
                                  .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)
                                  .join(", ");
        cleanup();
        setBusy(false);
        m_step = StepIdle;
        emit finished(false, QString("Архив пуст. Исходная папка (%1) содержит: %2")
                                 .arg(m_logsDir, files.isEmpty() ? "(пусто)" : files));
        return;
    }

    setProgress(40);
    setStatus(QString("Архив готов (%1 КБ). Подключение...").arg(m_archiveSize / 1024));
    m_step = StepConnecting;
    startConnecting();
}

// ─── TCP подключение / хендшейк ────────────────────────────────────────────
void NetworkLogSender::startConnecting()
{
    if (m_socket) { m_socket->disconnect(); m_socket->deleteLater(); m_socket = nullptr; }
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected,     this, &NetworkLogSender::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected,  this, &NetworkLogSender::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &NetworkLogSender::onSocketError);
    connect(m_socket, &QTcpSocket::readyRead,     this, &NetworkLogSender::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::bytesWritten,  this, &NetworkLogSender::onBytesWritten);
    m_socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,    512 * 1024);
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 512 * 1024);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->connectToHost(m_serverIp, kLogsPort);
}

void NetworkLogSender::onSocketConnected()
{
    m_bytesSent = m_lastBytes = m_lastMs = 0;
    m_incoming.clear();
    m_speedTimer.start();
    sendHandshake();
}

void NetworkLogSender::sendHandshake()
{
    QJsonObject obj;
    obj["type"]        = "logs";
    obj["archiveSize"] = m_archiveSize;
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    data.append('\n');
    m_socket->write(data);
    setStatus("Ожидание сервера...");
}

// ─── Чтение ответов сервера ─────────────────────────────────────────────────
void NetworkLogSender::onSocketReadyRead()
{
    m_incoming.append(m_socket->readAll());
    while (m_incoming.contains('\n')) {
        int nl = m_incoming.indexOf('\n');
        QByteArray line = m_incoming.left(nl).trimmed();
        m_incoming = m_incoming.mid(nl + 1);
        if (line == "READY") {
            m_step = StepSending;
            setStatus("Отправка архива...");
            sendChunk();
        } else if (line == "OK") {
            m_step = StepIdle;
            cleanup();
            setBusy(false);
            setProgress(100);
            setStatus("Логи успешно отправлены");
            emit finished(true, "Логи успешно доставлены на сервер.");
        } else if (line.startsWith("FAIL")) {
            QString serverMsg = QString::fromUtf8(line);
            cleanup();
            setBusy(false);
            m_step = StepIdle;
            setStatus("Ошибка на сервере");
            emit finished(false, "Сервер сообщил об ошибке:\n" + serverMsg);
        }
    }
}

// ─── Отправка чанками ───────────────────────────────────────────────────────
void NetworkLogSender::sendChunk()
{
    if (!m_socket || m_step != StepSending) return;
    if (!m_archiveFile) {
        m_archiveFile = new QFile(m_archivePath, this);
        if (!m_archiveFile->open(QIODevice::ReadOnly)) {
            cleanup();
            setBusy(false);
            m_step = StepIdle;
            emit finished(false, "Не удалось открыть архив для отправки.");
            return;
        }
    }
    QByteArray chunk = m_archiveFile->read(kChunkSize);
    if (chunk.isEmpty()) {
        m_archiveFile->close();
        m_archiveFile->deleteLater();
        m_archiveFile = nullptr;
        setStatus("Архив отправлен. Ожидание подтверждения...");
        return;
    }
    m_socket->write(chunk);
}


void NetworkLogSender::onBytesWritten(qint64 bytes)
{
    if (m_step != StepSending || !m_archiveFile) return;
    m_bytesSent += bytes;
    if (m_archiveSize > 0) {
        int pct = 40 + static_cast<int>(55.0 * m_bytesSent / m_archiveSize);
        setProgress(qMin(pct, 95));
    }
    const qint64 elapsed = m_speedTimer.elapsed();
    if (elapsed - m_lastMs >= 1000) {
        const double bps  = (m_bytesSent - m_lastBytes) * 1000.0 / qMax(elapsed - m_lastMs, qint64(1));
        const double mbps = bps * 8.0 / 1'000'000.0;
        setStatus(QString("Отправка: %1% (%2)")
                      .arg(m_progress)
                      .arg(mbps >= 1.0
                               ? QString("%1 Мбит/с").arg(mbps, 0, 'f', 1)
                               : QString("%1 КБ/с").arg(bps / 1024.0, 0, 'f', 1)));
        m_lastBytes = m_bytesSent;
        m_lastMs    = elapsed;
    }
    // Следующий кусок — только когда предыдущий почти ушёл в сеть. Раньше на
    // каждый bytesWritten дочитывалось ещё 2 МБ, и весь архив оседал в памяти.
    if (m_bytesSent < m_archiveSize) {
        if (m_socket && m_socket->bytesToWrite() < kChunkSize)
            sendChunk();
    }
    else if (m_archiveFile) {
        m_archiveFile->close();
        m_archiveFile->deleteLater();
        m_archiveFile = nullptr;
    }
}

// ─── TCP дисконнект / ошибки ────────────────────────────────────────────────
void NetworkLogSender::onSocketDisconnected()
{
    if (m_step != StepIdle && !m_cancelled) {
        cleanup();
        setBusy(false);
        m_step = StepIdle;
        setStatus("Соединение разорвано");
        emit finished(false, "Соединение с сервером было разорвано.");
    }
}

void NetworkLogSender::onSocketError(QAbstractSocket::SocketError)
{
    if (m_cancelled) return;
    QString err = m_socket ? m_socket->errorString() : "неизвестная ошибка";
    cleanup();
    setBusy(false);
    m_step = StepIdle;
    setStatus("Ошибка сети");
    emit finished(false, QString("Сетевая ошибка:\n%1").arg(err));
}

// ─── Вспомогательные ────────────────────────────────────────────────────────
void NetworkLogSender::cleanup()
{
    if (m_packCancel) {
        m_packCancel->store(true);
        m_packCancel.reset();
    }

    if (!m_tmpLogsDir.isEmpty()) {
        QDir tmp(m_tmpLogsDir);
        if (tmp.exists()) {
            tmp.removeRecursively();
            qDebug() << "[LogSender] Removed temp log folder:" << m_tmpLogsDir;
        }
        m_tmpLogsDir.clear();
    }

    if (m_archiveFile) {
        m_archiveFile->close();
        m_archiveFile->deleteLater();
        m_archiveFile = nullptr;
    }
    if (m_socket) {
        m_socket->disconnect();
        if (m_socket->state() == QAbstractSocket::ConnectedState)
            m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    QFile::remove(m_archivePath);
}

void NetworkLogSender::setStatus(const QString &s)
{ if (m_status != s) { m_status = s; emit statusChanged(); } }
void NetworkLogSender::setProgress(int p)
{ if (m_progress != p) { m_progress = p; emit progressChanged(); } }
void NetworkLogSender::setBusy(bool b)
{ if (m_busy != b) { m_busy = b; emit busyChanged(); } }
