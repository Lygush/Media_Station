// modules/AndroidTVModule/AndroidTVLog.cpp

#include "AndroidTVModule.h"
#include "LoggerModule.h"
#include "core/EventBus.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QRegularExpression>

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
    // Первый раз за сессию — перезаписываем файл, дальше только дописываем
    // (иначе запись в лог после closeEmulatorLogFile() стирала весь лог сессии).
    // Лог прошлой сессии не стираем, а сохраняем как Android_TV.prev.log:
    // раньше после неудачного запуска и перезапуска приложения подробности
    // пропадали.
    static bool s_rotatedThisSession = false;
    if (!s_rotatedThisSession) {
        s_rotatedThisSession = true;
        const QString cur  = emulatorLogPath();
        const QString prev = QFileInfo(cur).absolutePath() + "/Android_TV.prev.log";
        if (QFileInfo(cur).size() > 0) {
            QFile::remove(prev);
            QFile::rename(cur, prev);
        }
    }
    const QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append;

    m_emulatorLogFile.setFileName(emulatorLogPath());
    if (!m_emulatorLogFile.open(mode)) {
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

    const char *prefix = fromEmulator ? "[EMULATOR] " : "[DETAIL]   ";
    const QString profileStr = m_profileManager && m_profileManager->activeProfile()
                                   ? "[" + m_profileManager->activeProfile()->avdName + "] "
                                   : QString();
    m_emulatorLogStream << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                        << " " << prefix << profileStr << msg << "\n";
    m_emulatorLogStream.flush();

    // Ошибки самого эмулятора дублируем в основной журнал (с ограничением),
    // чтобы причину неудачного запуска было видно без Android_TV.log.
    if (fromEmulator && m_emulatorLinesForwarded < 40) {
        // Только настоящие ошибки эмулятора («ERROR | …», «FATAL», «PANIC»);
        // «Failed to load snapshot» при холодном старте и предупреждения Qt
        // внутри эмулятора — штатные, им место только в Android_TV.log
        static const QRegularExpression re(QStringLiteral("\\b(ERROR|FATAL|PANIC)\\b"));
        for (const QString &line : msg.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            if (!re.match(line).hasMatch()) continue;
            Logger::write(Logger::kLevelWarning, "AndroidTV", "emulator: " + line.trimmed().left(300));
            if (++m_emulatorLinesForwarded >= 40) {
                Logger::write(Logger::kLevelWarning, "AndroidTV",
                              "emulator: further errors only in logs/Android_TV.log");
                break;
            }
        }
    }
}

void AndroidTVModule::logImportant(const char *level, const QString &msg)
{
    // Один путь записи (раньше — дважды: qDebug/qWarning и шина)
    Logger::write(level, "AndroidTV", msg);
}
