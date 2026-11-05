// modules/NetworkModule/NetworkServer.cpp
// Реализация серверной части NetworkModule

#include "NetworkModule.h"
#include "core/EventBus.h"
#include "tools/SevenZip.h"
#include "InstallUtils.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QThreadPool>
#include <QPointer>
#include <QStorageInfo>
#include <utility>

// ─── UDP-обнаружение ─────────────────────────────────────────────────────────

void NetworkModule::processUdpDatagrams()
{
    while (m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_udpSocket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        if (datagram.trimmed() == "DISCOVER_MEDIACENTER")
            m_udpSocket->writeDatagram("MEDIACENTER_RESPONSE", sender, senderPort);
    }
}

// ─── Новое TCP-подключение ────────────────────────────────────────────────────

void NetworkModule::newConnection()
{
    QTcpSocket *socket = m_server->nextPendingConnection();
    if (!socket) return;

    socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 512 * 1024);
    socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,    512 * 1024);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    connect(socket, &QTcpSocket::readyRead,    this, &NetworkModule::readyRead);
    connect(socket, &QTcpSocket::disconnected, this, &NetworkModule::socketDisconnected);

    ClientState st;
    st.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    m_clients[socket] = std::move(st);
}

// ─── Приём данных ─────────────────────────────────────────────────────────────

void NetworkModule::readyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !m_clients.contains(socket)) return;

    ClientState &st = m_clients[socket];
    st.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    st.buffer.append(socket->readAll());

    // Строковые фазы: строка протокола без перевода строки длиннее 1 МБ —
    // мусор; иначе буфер рос бы без ограничений.
    if ((st.phase == PhaseNegotiating || st.phase == PhasePackProgress)
        && st.buffer.size() > 1024 * 1024 && !st.buffer.contains('\n')) {
        log("WARN", "Слишком длинная строка протокола — соединение закрыто");
        cleanupClient(socket, false);
        socket->disconnectFromHost();
        return;
    }

    if (st.phase == PhaseNegotiating) {
        while (st.buffer.contains('\n')) {
            const int idx         = st.buffer.indexOf('\n');
            const QByteArray line = st.buffer.left(idx).trimmed();
            st.buffer             = st.buffer.mid(idx + 1);

            if (line == "CANCEL") {
                cleanupClient(socket, false);   // сам сообщит UI об отмене
                socket->disconnectFromHost();
                return;
            }

            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                qWarning() << "[NetworkModule] invalid negotiation JSON from"
                           << socket->peerAddress().toString();
                socket->disconnectFromHost();
                return;
            }
            st.metadata = doc.object();

            const QString kind = st.metadata["kind"].toString();

            if (kind == QLatin1String("mediastation_update")) {
                st.isUpdate = true;
                st.phase = PhaseUpdate;
                st.expectedSize = st.metadata["archiveSize"].toInteger(-1);

                if (st.expectedSize <= 0) {
                    qWarning() << "[NetworkModule] invalid archiveSize for update";
                    socket->write("FAIL: invalid archiveSize\n");
                    socket->disconnectFromHost();
                    return;
                }

                st.tempArchivePath = prepareUpdateArchivePath();
                if (st.tempArchivePath.isEmpty()) {
                    socket->write("FAIL: cannot prepare update directory\n");
                    socket->disconnectFromHost();
                    return;
                }
                // Не принимаем гигабайты, если архив некуда положить
                if (!hasFreeSpace(QFileInfo(st.tempArchivePath).absolutePath(), st.expectedSize)) {
                    log("WARN", "Недостаточно места для архива обновления");
                    st.tempArchivePath.clear();
                    socket->write("FAIL: not enough disk space\n");
                    socket->disconnectFromHost();
                    return;
                }

                st.receivedBytes = 0;
                socket->write("READY\n");
                qDebug() << "[NetworkModule] update negotiation OK, size:" << st.expectedSize;

                // Буфер забираем ДО вызова: receiveArchiveData может удалить
                // ClientState (cleanupClient), и ссылка st станет висячей.
                if (!st.buffer.isEmpty()) {
                    const QByteArray chunk = std::exchange(st.buffer, QByteArray());
                    receiveArchiveData(socket, chunk);
                }
                return;
            }

            // Установка игры
            st.phase = PhasePackProgress;
            emit showInstallDialog(
                st.metadata["title"].toString(),
                st.metadata["kind"].toString("app"),
                st.metadata["exeRelativePath"].toString(),
                st.metadata["iconRelativePath"].toString(),
                socket
                );
            // Слот диалога мог синхронно отклонить/отменить установку —
            // тогда состояния клиента уже нет и ссылка st недействительна.
            if (!m_clients.contains(socket)) return;
            // Остаток буфера (PACK:…, размер архива), пришедший в том же TCP-сегменте,
            // разбираем уже как фазу упаковки. Раньше цикл продолжал парсить его как
            // JSON согласования: строка «PACK:5» считалась битым JSON и рвала соединение.
            break;
        }
        if (st.phase == PhaseNegotiating) return;   // ждём полную строку согласования
    }

    if (st.phase == PhasePackProgress) {
        while (st.buffer.contains('\n')) {
            const int idx         = st.buffer.indexOf('\n');
            const QByteArray line = st.buffer.left(idx).trimmed();
            st.buffer             = st.buffer.mid(idx + 1);

            if (line == "CANCEL") {
                cleanupClient(socket, false);   // сам сообщит UI об отмене
                socket->disconnectFromHost();
                return;
            }

            if (line.startsWith("PACK:")) {
                const int percent = line.mid(5).toInt();
                if (!st.tileId.isEmpty())
                    emit installProgress(st.tileId, percent / 100.0, "packing");
            } else {
                QJsonParseError err;
                const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
                if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;

                const QJsonObject obj = doc.object();
                const qint64 size = obj["archiveSize"].toInteger(-1);
                if (size <= 0) {
                    qWarning() << "[NetworkModule] invalid archiveSize";
                    socket->disconnectFromHost();
                    return;
                }

                // Проверяем место ДО приёма: иначе отправитель передаёт гигабайты,
                // а ошибка «диск заполнен» всплывает только в конце.
                // Нужно место и под архив во временной папке, и под распаковку.
                const QString installRoot = m_installPath.isEmpty()
                    ? QCoreApplication::applicationDirPath() + "/data/games"
                    : m_installPath;
                if (!hasFreeSpace(QDir::tempPath(), size)
                    || !hasFreeSpace(installRoot, size)) {
                    log("WARN", QString("Недостаточно места для установки (%1 МБ)")
                                    .arg(size / (1024 * 1024)));
                    socket->write("FAIL: not enough disk space\n");
                    socket->disconnectFromHost();
                    return;
                }

                // 🔧 Создаём временный файл для архива игры.
                // Адрес сокета в имени — две параллельные установки в одну
                // миллисекунду больше не пишут в один и тот же файл.
                st.tempArchivePath = QDir::temp().filePath(
                    QString("mediastation_install_%1_%2.7z")
                        .arg(QDateTime::currentMSecsSinceEpoch())
                        .arg(reinterpret_cast<quintptr>(socket), 0, 16));
                st.expectedSize  = size;
                st.receivedBytes = 0;
                st.phase         = PhaseReceiving;
                socket->write("READY\n");

                if (!st.buffer.isEmpty()) {
                    const QByteArray chunk = std::exchange(st.buffer, QByteArray());
                    receiveArchiveData(socket, chunk);
                }
                return;   // остальное придёт уже в фазе PhaseReceiving
            }
        }
        return;
    }

    if (st.phase == PhaseReceiving || st.phase == PhaseUpdate) {
        const QByteArray chunk = std::exchange(st.buffer, QByteArray());
        receiveArchiveData(socket, chunk);
        return;
    }

    // PhaseExtracting: архив уже принят, лишние данные игнорируем
    st.buffer.clear();
}

void NetworkModule::receiveArchiveData(QTcpSocket *socket, const QByteArray &data)
{
    if (data.isEmpty() || !m_clients.contains(socket)) return;
    ClientState &st = m_clients[socket];
    if (st.receivedBytes >= st.expectedSize) return;   // архив уже принят целиком

    if (!st.archiveFile) {
        st.archiveFile = new QFile(st.tempArchivePath, this);
        if (!st.archiveFile->open(QIODevice::Append)) {
            qWarning() << "[NetworkModule] cannot open archive for writing:" << st.tempArchivePath;
            cleanupClient(socket, true);
            return;
        }
    }

    // Не пишем в архив больше объявленного размера (хвост — уже не архив)
    const qint64 remaining = st.expectedSize - st.receivedBytes;
    const QByteArray part  = data.size() > remaining ? data.left(remaining) : data;
    const qint64 written   = st.archiveFile->write(part);
    if (written != part.size()) {
        qWarning() << "[NetworkModule] partial write (disk full?)";
        cleanupClient(socket, true);
        return;
    }
    st.receivedBytes += part.size();

    reportProgress(st, st.receivedBytes >= st.expectedSize);

    if (st.receivedBytes >= st.expectedSize) {
        st.archiveFile->close();
        st.archiveFile->deleteLater();
        st.archiveFile = nullptr;

        if (st.isUpdate)
            finalizeUpdate(socket);
        else
            extractAndInstall(socket);
    }
}

void NetworkModule::reportProgress(ClientState &st, bool force)
{
    if (st.expectedSize <= 0) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - st.lastProgressMs < 100) return;
    st.lastProgressMs = now;

    const qreal progress = qMin(qreal(st.receivedBytes) / st.expectedSize, 1.0);
    if (st.isUpdate) {
        if (m_bus) {
            emit m_bus->moduleEvent("updater", "downloadProgress",
                                    QVariantMap{ {"progress", progress} });
        }
    } else if (!st.tileId.isEmpty()) {
        emit installProgress(st.tileId, progress, "receiving");
    }
}

bool NetworkModule::hasFreeSpace(const QString &dirPath, qint64 neededBytes)
{
    // Поднимаемся до существующей папки: каталога установки может ещё не быть
    QDir d(dirPath);
    while (!d.exists() && d.cdUp()) {}
    const QStorageInfo si(d.absolutePath());
    if (!si.isValid() || !si.isReady())
        return true;                                   // объём неизвестен — не блокируем
    constexpr qint64 kMargin = 64LL * 1024 * 1024;     // запас под служебные файлы
    return si.bytesAvailable() >= neededBytes + kMargin;
}

void NetworkModule::finalizeUpdate(QTcpSocket *socket)
{
    ClientState &st = m_clients[socket];

    const QString archivePath   = st.tempArchivePath;
    const QString version       = st.metadata["version"].toString();
    const QString expectedHash  = st.metadata["expectedHash"].toString();
    const QString releaseNotes  = st.metadata["releaseNotes"].toString();

    qDebug() << "[NetworkModule] update archive received, size:"
             << st.receivedBytes << "bytes, version:" << version;

    st.tempArchivePath.clear();

    if (m_bus) {
        emit m_bus->moduleEvent("updater", "updateReceived", QVariantMap{
                                                                 { "version",       version      },
                                                                 { "archivePath",   archivePath  },
                                                                 { "expectedHash",  expectedHash },
                                                                 { "releaseNotes",  releaseNotes }
                                                             });
    }

    socket->write("OK: update received\n");
    socket->disconnectFromHost();
}

void NetworkModule::extractAndInstall(QTcpSocket *socket)
{
    ClientState &st = m_clients[socket];

    const QString archivePath      = st.tempArchivePath;
    const QString tileId           = st.tileId;
    const QString title            = st.metadata["title"].toString();
    const QString kind             = st.metadata["kind"].toString("app");
    const QString exeRelativePath  = st.metadata["exeRelativePath"].toString();
    const QString iconRelativePath = st.metadata["iconRelativePath"].toString();

    if (!tileId.isEmpty())
        emit installProgress(tileId, 0.0, "extracting");

    const QString gameRootPath = prepareInstallDir(tileId, title);
    if (gameRootPath.isEmpty()) {
        log("ERROR", "Не удалось создать папку установки для " + title);
        socket->write("FAIL: cannot create install directory\n");
        cleanupClient(socket, false);   // удалит архив и уберёт плитку из UI
        socket->disconnectFromHost();
        return;
    }

    st.phase        = PhaseExtracting;
    st.gameRootPath = gameRootPath;

    const QString extractDir = gameRootPath + "/game";
    const auto cancelFlag    = st.cancelFlag;
    // Модуль может быть удалён, пока поток пула ещё распаковывает
    const QPointer<NetworkModule> self(this);
    // Сокет может быть удалён (отправитель отключился) до конца распаковки;
    // сырой указатель после deleteLater мог совпасть с адресом нового клиента.
    const QPointer<QTcpSocket> sock(socket);

    // Запускаем разархивацию в пуле потоков — SevenZip::extract блокирующий
    QThreadPool::globalInstance()->start([self, sock, gameRootPath, archivePath,
                                          extractDir, tileId, title, cancelFlag,
                                          exeRelativePath, iconRelativePath, kind]() {
        int lastPct = -1;
        const SevenZip::Result r = SevenZip::extract(
            archivePath,
            extractDir,
            cancelFlag.get(),
            [self, tileId, &lastPct](int pct, const QString &) {
                if (tileId.isEmpty() || pct == lastPct || !self) return;
                lastPct = pct;
                QMetaObject::invokeMethod(self.data(), [self, tileId, pct]() {
                    if (self) emit self->installProgress(tileId, pct / 100.0, "extracting");
                }, Qt::QueuedConnection);
            });

        if (!self) {   // приложение закрывается
            QFile::remove(archivePath);
            if (r != SevenZip::Ok) QDir(gameRootPath).removeRecursively();
            return;
        }

        QMetaObject::invokeMethod(self.data(),
            [self, sock, gameRootPath, archivePath, cancelFlag,
             tileId, title, exeRelativePath, iconRelativePath, kind, r]() {
                if (!self) return;
                NetworkModule *m = self.data();

                QFile::remove(archivePath);

                // Явная отмена (пользователь, CANCEL, таймаут, выход) — папку удаляем
                // здесь, когда 7z уже точно не пишет в неё. UI уже уведомлён.
                if (cancelFlag->load() || r == SevenZip::Cancelled) {
                    QDir(gameRootPath).removeRecursively();
                    return;
                }

                // Отправитель мог отключиться после передачи — архив у нас целиком,
                // установку доводим до конца, просто некому отвечать.
                const bool clientAlive = sock && m->m_clients.contains(sock.data());

                if (r != SevenZip::Ok) {
                    m->log("ERROR", QString("Ошибка распаковки «%1»: %2")
                                        .arg(title, SevenZip::resultToString(r)));
                    QDir(gameRootPath).removeRecursively();
                    if (clientAlive) {
                        QTcpSocket *s = sock.data();
                        m->m_clients[s].gameRootPath.clear();
                        m->m_clients[s].tempArchivePath.clear();
                        s->write("FAIL: extraction error\n");
                        m->cleanupClient(s, false);    // уберёт плитку из UI
                        s->disconnectFromHost();
                    } else if (!tileId.isEmpty()) {
                        emit m->serverCancelledInstall(tileId);
                    }
                    return;
                }

                if (clientAlive)
                    m->m_clients.remove(sock.data());   // всё временное уже убрано

                if (m->m_bus)
                    emit m->m_bus->networkExtractFinished(
                        tileId, title, gameRootPath, exeRelativePath, iconRelativePath, kind);

                if (clientAlive) {
                    sock->write("OK: installation started\n");
                    sock->disconnectFromHost();
                }
            }, Qt::QueuedConnection);
    });
}

void NetworkModule::socketDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;
    auto it = m_clients.find(socket);
    if (it != m_clients.end()) {
        if (it->phase == PhaseExtracting) {
            // Архив уже принят целиком: отключение отправителя больше не повод
            // выбрасывать установку (раньше 7z убивался, а папка удалялась).
            log("INFO", "Отправитель отключился во время распаковки — установка продолжается");
            m_clients.erase(it);
        } else {
            cleanupClient(socket, false);
        }
    }
    socket->deleteLater();
}

void NetworkModule::cleanupClient(QTcpSocket *socket, bool sendCancel)
{
    if (!m_clients.contains(socket)) return;
    ClientState &st = m_clients[socket];

    st.cancelled = true;
    if (st.cancelFlag) st.cancelFlag->store(true);

    // Единственное место, где UI узнаёт о незавершённой установке. Раньше сигнал
    // слался лишь при CANCEL/отмене пользователем, а при обрыве связи, нехватке
    // места или ошибке записи плитка навсегда оставалась в состоянии «установка».
    const QString unfinishedTileId = st.finished ? QString() : st.tileId;

    const bool closeSocket = sendCancel && socket->state() == QAbstractSocket::ConnectedState;
    if (closeSocket) {
        socket->write("CANCEL\n");
        socket->flush();
    }

    if (st.archiveFile) {
        st.archiveFile->close();
        st.archiveFile->deleteLater();
        st.archiveFile = nullptr;
    }

    if (!st.tempArchivePath.isEmpty()) {
        QFile::remove(st.tempArchivePath);
        st.tempArchivePath.clear();
    }

    if (st.extractProcess) {
        st.extractProcess->kill();
        st.extractProcess->waitForFinished(500);
        st.extractProcess = nullptr;
    }

    // Во время распаковки папку удаляет обработчик завершения фоновой задачи
    // (иначе 7z продолжал бы писать в удаляемую папку).
    if (!st.gameRootPath.isEmpty() && st.phase != PhaseExtracting) {
        QDir dir(st.gameRootPath);
        if (dir.exists()) dir.removeRecursively();
    }
    st.gameRootPath.clear();

    m_clients.remove(socket);

    if (!unfinishedTileId.isEmpty())
        emit serverCancelledInstall(unfinishedTileId);

    // Раньше после CANCEL сокет оставался открытым (состояние клиента уже
    // удалено, readyRead его игнорирует) и утекал вместе с буферами.
    // Закрываем строго ПОСЛЕ remove(): disconnectFromHost может синхронно
    // вызвать socketDisconnected → cleanupClient, а ссылка st уже не нужна.
    if (closeSocket)
        socket->disconnectFromHost();
}

void NetworkModule::acceptInstall(QTcpSocket *socket, const QString &tileId)
{
    if (!m_clients.contains(socket)) return;
    m_clients[socket].tileId = tileId;
    socket->write("OK\n");
}

void NetworkModule::rejectInstall(QTcpSocket *socket)
{
    if (!m_clients.contains(socket)) return;
    socket->write("REJECTED\n");
    socket->disconnectFromHost();
}

void NetworkModule::cancelInstallByTileId(const QString &tileId)
{
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->tileId == tileId) {
            cancelInstall(it.key());
            return;
        }
    }
    if (!tileId.isEmpty())
        emit serverCancelledInstall(tileId);
}

void NetworkModule::cancelInstall(QTcpSocket *socket)
{
    if (!m_clients.contains(socket)) return;
    cleanupClient(socket, true);   // сообщит UI об отмене
}

bool NetworkModule::hasActiveInstall(const QString &tileId) const
{
    if (tileId.isEmpty()) return false;
    for (const ClientState &st : m_clients)
        if (st.tileId == tileId) return true;
    return false;
}

QString NetworkModule::prepareUpdateArchivePath() const
{
    const QString tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
    + "/MediaStation_Update";
    if (!QDir().mkpath(tempRoot)) {
        qWarning() << "[NetworkModule] cannot create update temp dir:" << tempRoot;
        return {};
    }

    const QString timestamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    return tempRoot + "/update_" + timestamp + ".7z";
}

QString NetworkModule::prepareInstallDir(const QString &tileId,
                                         const QString &title) const
{
    const QString gamesInstallDir = m_installPath.isEmpty()
    ? QCoreApplication::applicationDirPath() + "/data/games"
    : m_installPath;

    // Те же правила имени, что и у локальной установки (InstallUtils)
    const QString gameFolderName = InstallUtils::generateUniqueFolderName(
        InstallUtils::sanitizeFolderName(title, tileId), gamesInstallDir);

    const QDir gamesDir(gamesInstallDir);
    if (!gamesDir.mkpath(".")) {
        qWarning() << "[NetworkModule] cannot create games dir:" << gamesInstallDir;
        return {};
    }

    const QString gameRootPath = gamesDir.filePath(gameFolderName);
    if (!QDir(gameRootPath).mkpath(".")           ||
        !QDir(gameRootPath + "/game").mkpath(".")  ||
        !QDir(gameRootPath + "/launcher").mkpath(".")) {
        qWarning() << "[NetworkModule] cannot create subdirs for:" << gameRootPath;
        return {};
    }

    InstallUtils::writeTileIdFile(gameRootPath, tileId);
    return gameRootPath;
}
