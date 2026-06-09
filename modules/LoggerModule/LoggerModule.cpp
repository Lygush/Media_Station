// LoggerModule.cpp
#include "LoggerModule.h"
#include <QDebug>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>

// ---------- LoggerWorker ----------
LoggerWorker::LoggerWorker(QObject *parent)
    : QObject(parent)
{
}

LoggerWorker::~LoggerWorker()
{
    stop();
    if (m_file.isOpen())
        m_file.close();
}

void LoggerWorker::setLogFilePath(const QString &path)
{
    if (m_file.isOpen())
        m_file.close();

    // Создаём директорию для логов, если она не существует
    QFileInfo fileInfo(path);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qWarning() << "LoggerWorker: cannot create log directory" << dir.absolutePath();
            return;
        }
    }

    m_file.setFileName(path);
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_stream.setDevice(&m_file);
        m_stream << "=== Log started at " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ===\n";
        m_stream.flush();
    } else {
        qWarning() << "LoggerWorker: cannot open log file" << path;
    }
}

void LoggerWorker::writeLog(const QString &level, const QString &source, const QString &message)
{
    if (!m_running || !m_file.isOpen())
        return;

    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    m_stream << timestamp << " [" << level.toUpper() << "] " << source << ": " << message << "\n";
    m_stream.flush();
}

void LoggerWorker::stop()
{
    if (m_stopped)  // уже останавливали
        return;
    m_stopped = true;

    m_running = false;
    if (m_file.isOpen()) {
        m_stream << "=== Log stopped at " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ===\n";
        m_stream.flush();
    }
}

// ---------- LoggerModule ----------
LoggerModule::LoggerModule(QObject *parent)
    : QObject(parent)
{
    QString appDir = QCoreApplication::applicationDirPath();
    m_logFilePath = appDir + "/logs/MediaStation.log";
}

LoggerModule::~LoggerModule()
{
    shutdown();
}

void LoggerModule::initialize(EventBus *bus, SettingsManager *settings)
{
    m_bus = bus;
    m_settings = settings;

    connect(m_bus, &EventBus::moduleEvent, this, &LoggerModule::onModuleEvent);

    m_worker = new LoggerWorker();
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &LoggerModule::logRequested, m_worker, &LoggerWorker::writeLog, Qt::QueuedConnection);

    m_workerThread.start();

    // Безопасный вызов слота в потоке воркера
    QMetaObject::invokeMethod(m_worker, "setLogFilePath", Qt::QueuedConnection, Q_ARG(QString, m_logFilePath));

    qDebug() << "LoggerModule initialized, log file:" << m_logFilePath;
}

void LoggerModule::shutdown()
{
    if (m_worker) {
        // Блокирующий вызов stop() гарантирует завершение записи до выхода
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait();
        m_worker = nullptr;
    }
}

void LoggerModule::setLogFilePath(const QString &path)
{
    if (m_logFilePath == path)
        return;
    m_logFilePath = path;
    emit logFilePathChanged();

    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "setLogFilePath", Qt::QueuedConnection, Q_ARG(QString, m_logFilePath));
    }
}

void LoggerModule::log(const QString &level, const QString &source, const QString &message)
{
    emit logRequested(level, source, message);
}

void LoggerModule::onModuleEvent(const QString &module, const QString &event, const QVariantMap &data)
{
    if (module != Logger::kModule)
        return;
    if (event != Logger::kEventLog)
        return;

    QString level   = data.value("level").toString();
    QString source  = data.value("source").toString();
    QString message = data.value("message").toString();

    emit logRequested(level, source, message);
}
