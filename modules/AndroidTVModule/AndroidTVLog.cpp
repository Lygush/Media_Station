// modules/AndroidTVModule/AndroidTVLog.cpp

#include "AndroidTVModule.h"
#include "LoggerModule.h"
#include "core/EventBus.h"

#include <QDateTime>
#include <QDir>
#include <QDebug>

QString AndroidTVModule::emulatorLogPath() const
{
    QString logsDir = m_appDir + "/logs";
    QDir().mkpath(logsDir);
    return logsDir + "/Android_TV.log";
}

QString AndroidTVModule::logcatLogPath() const
{
    QString logsDir = m_appDir + "/logs";
    QDir().mkpath(logsDir);
    return logsDir + "/Android_TV_logcat.log";
}

void AndroidTVModule::openEmulatorLogFile()
{
    if (m_emulatorLogFile.isOpen()) return;
    m_emulatorLogFile.setFileName(emulatorLogPath());
    if (!m_emulatorLogFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[AndroidTV] Cannot open emulator log file:" << emulatorLogPath();
        return;
    }
    m_emulatorLogStream.setDevice(&m_emulatorLogFile);
    m_emulatorLogStream << "\n=== Android TV emulator log started at "
                        << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
                        << " ===\n";
    m_emulatorLogStream.flush();
}

void AndroidTVModule::closeEmulatorLogFile()
{
    if (m_emulatorLogFile.isOpen()) {
        m_emulatorLogStream << "=== Android TV emulator log ended ===\n\n";
        m_emulatorLogStream.flush();
        m_emulatorLogFile.close();
    }
}

void AndroidTVModule::logToEmulatorFile(const QString &msg, bool fromEmulator)
{
    if (!m_emulatorLogFile.isOpen()) openEmulatorLogFile();
    if (!m_emulatorLogFile.isOpen()) return;

    const char *prefix = fromEmulator ? "[EMULATOR] " : "[DETAIL] ";
    const QString profileStr = m_profileManager && m_profileManager->activeProfile()
                                   ? " [" + m_profileManager->activeProfile()->avdName + "]"
                                   : QString();
    m_emulatorLogStream << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                        << " " << prefix << profileStr << msg << "\n";
    m_emulatorLogStream.flush();
}

void AndroidTVModule::logImportant(const char *level, const QString &msg)
{
    if (qstrcmp(level, Logger::kLevelError) == 0 ||
        qstrcmp(level, Logger::kLevelWarning) == 0)
        qWarning() << "[AndroidTV]" << msg;
    else
        qDebug()   << "[AndroidTV]" << msg;

    if (!m_bus) return;
    QVariantMap data;
    data["level"]   = QString(level);
    data["source"]  = QStringLiteral("androidtv");
    data["message"] = msg;
    emit m_bus->moduleEvent(QStringLiteral("logger"), QStringLiteral("log"), data);
}
