// LoggerModule.h
#ifndef LOGGERMODULE_H
#define LOGGERMODULE_H

#include <QObject>
#include <QThread>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QVariantMap>

#include "core/IModule.h"
#include "core/EventBus.h"

namespace Logger {
inline constexpr const char *kModule    = "logger";
inline constexpr const char *kEventLog  = "log";

inline constexpr const char *kLevelDebug   = "debug";
inline constexpr const char *kLevelInfo    = "info";
inline constexpr const char *kLevelWarning = "warning";
inline constexpr const char *kLevelError   = "error";

inline QVariantMap makeEvent(const char *level,
                             const char *source,
                             const char *message) {
    QVariantMap data;
    data["level"]   = level;
    data["source"]  = source;
    data["message"] = message;
    return data;
}
}

class LoggerWorker : public QObject
{
    Q_OBJECT
public:
    explicit LoggerWorker(QObject *parent = nullptr);
    ~LoggerWorker();

public slots:
    void setLogFilePath(const QString &path);
    void writeLog(const QString &level, const QString &source, const QString &message);
    void stop();

private:
    QFile m_file;
    QTextStream m_stream;
    bool m_running = true;
    bool m_stopped = false;
};

class LoggerModule : public QObject, public IModule
{
    Q_OBJECT
    Q_PROPERTY(QString logFilePath READ logFilePath WRITE setLogFilePath NOTIFY logFilePathChanged)

public:
    explicit LoggerModule(QObject *parent = nullptr);
    ~LoggerModule();

    // IModule implementation
    QString moduleId() const override { return "logger"; }
    void initialize(EventBus *bus, SettingsManager *settings) override;
    void shutdown() override;
    QObject *qmlObject() override { return this; }
    QString qmlName() const override { return "LoggerModule"; }

    QString logFilePath() const { return m_logFilePath; }
    void setLogFilePath(const QString &path);

    // QML доступ
    Q_INVOKABLE void log(const QString &level, const QString &source, const QString &message);

signals:
    void logFilePathChanged();
    void logRequested(const QString &level, const QString &source, const QString &message);

private slots:
    void onModuleEvent(const QString &module, const QString &event, const QVariantMap &data);

private:
    EventBus *m_bus = nullptr;
    SettingsManager *m_settings = nullptr;
    QString m_logFilePath;
    QThread m_workerThread;
    LoggerWorker *m_worker = nullptr;
};

#endif // LOGGERMODULE_H
