// LoggerModule.cpp
#include "LoggerModule.h"
#include <QDebug>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QAtomicPointer>
#include <QRegularExpression>
#include <QSysInfo>
#include <QLibraryInfo>
#include "Version.h"

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

    // Ротация: раньше MediaStation.log дописывался бесконечно
    m_file.setFileName(path);
    rotateIfTooBig();
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_stream.setDevice(&m_file);
        m_stream << "=== Log started at " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ===\n";
        m_stream.flush();
    } else {
        qWarning() << "LoggerWorker: cannot open log file" << path;
    }
}

// Ротация: <log> → <log>.1, когда файл больше kMaxLogSize. Раньше размер
// проверялся только при запуске, а медиастанция может работать неделями —
// журнал рос без ограничений.
void LoggerWorker::rotateIfTooBig()
{
    static constexpr qint64 kMaxLogSize = 10 * 1024 * 1024;
    const QString path = m_file.fileName();
    if (path.isEmpty() || QFileInfo(path).size() <= kMaxLogSize)
        return;

    const bool wasOpen = m_file.isOpen();
    if (wasOpen) {
        m_stream.flush();
        m_file.close();
    }
    const QString old = path + ".1";
    QFile::remove(old);
    QFile::rename(path, old);
    if (wasOpen && m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_stream.setDevice(&m_file);
        m_stream << "=== Log rotated at " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ===\n";
    }
}

// Формат строки (одинаковый для всех модулей, колонки выровнены):
//   2026-11-04 16:20:01.123  WARN   [Network]          текст
static QString levelTag(const QString &level)
{
    const QString l = level.toLower();
    if (l.startsWith(QLatin1String("err")) || l == QLatin1String("critical")
        || l == QLatin1String("fatal"))                return QStringLiteral("ERROR");
    if (l.startsWith(QLatin1String("warn")))           return QStringLiteral("WARN ");
    if (l == QLatin1String("debug"))                   return QStringLiteral("DEBUG");
    return QStringLiteral("INFO ");
}

void LoggerWorker::writeLine(const QString &level, const QString &source, const QString &message)
{
    const QString ts  = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    const QString src = QLatin1Char('[') + (source.isEmpty() ? QStringLiteral("app") : source) + QLatin1Char(']');
    // Ширина колонки источника — под самое длинное имя модуля
    // (GameLibraryModule); многострочные сообщения — с отступом под текст.
    constexpr int kSrcW   = 20;
    constexpr int kIndent = 23 + 2 + 5 + 2 + kSrcW + 1;
    QString text = message;
    text.replace(QLatin1Char('\n'), QStringLiteral("\n") + QString(kIndent, QLatin1Char(' ')));
    m_stream << ts << "  " << levelTag(level) << "  " << src.leftJustified(kSrcW) << ' ' << text << '\n';
}

void LoggerWorker::flushRepeats()
{
    if (m_repeat > 0) {
        writeLine(m_lastLevel, m_lastSource,
                  QStringLiteral("… предыдущее сообщение повторилось ещё %1 раз").arg(m_repeat));
        m_repeat = 0;
    }
}

void LoggerWorker::writeLog(const QString &level, const QString &source, const QString &message)
{
    if (!m_running || !m_file.isOpen())
        return;

    if (++m_writesSinceSizeCheck >= 500) {
        m_writesSinceSizeCheck = 0;
        flushRepeats();
        rotateIfTooBig();
        if (!m_file.isOpen()) return;
    }

    const QString key = level + QLatin1Char('\x1f') + source + QLatin1Char('\x1f') + message;
    if (key == m_lastKey) {
        ++m_repeat;
        return;
    }
    flushRepeats();
    m_lastKey    = key;
    m_lastLevel  = level;
    m_lastSource = source;
    writeLine(level, source, message);
    m_stream.flush();
}

void LoggerWorker::stop()
{
    if (m_stopped)  // уже останавливали
        return;
    m_stopped = true;

    m_running = false;
    if (m_file.isOpen()) {
        flushRepeats();
        m_stream << "=== Log stopped at " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ===\n";
        m_stream.flush();
    }
}

// ---------- Перехват qWarning/qCritical ----------
// Предупреждения Qt (ошибки QML, сетевые ошибки и т.п.) раньше были видны только
// в отладчике. Теперь они дублируются в MediaStation.log.
static QAtomicPointer<LoggerModule> s_logInstance;
static QtMessageHandler s_prevHandler = nullptr;

// qDebug() выводит QString в кавычках с экранированием: "F:\\Games\\X".
// В журнале пути должны читаться как есть — убираем экранирование внутри
// кавычек (сами кавычки оставляем: они отделяют пути с пробелами).
static QString unescapeQuoted(const QString &in)
{
    if (!in.contains(QLatin1Char('"')) || !in.contains(QLatin1Char('\\')))
        return in;
    QString out;
    out.reserve(in.size());
    bool inQuotes = false;
    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            out += c;
        } else if (inQuotes && c == QLatin1Char('\\') && i + 1 < in.size()
                   && (in.at(i + 1) == QLatin1Char('\\') || in.at(i + 1) == QLatin1Char('"'))) {
            out += in.at(++i);
        } else {
            out += c;
        }
    }
    return out;
}

// Источник берётся из префикса сообщения, который модули используют в
// разных стилях: «[InstallManager] …», «GameLibraryModule: …»,
// «SevenZip::extract: …». Раньше все такие строки шли с источником «qt»,
// а отладочные (qDebug — запуск игры, подключение геймпада, установка и т.д.)
// в файл не попадали вовсе.
static void splitSource(const QMessageLogContext &ctx, const QString &msg,
                        QString &source, QString &text)
{
    static const QRegularExpression kBracket(QStringLiteral(R"(^\[([A-Za-z][A-Za-z0-9 _.-]{0,40})\]\s*(.*)$)"),
                                             QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression kColon(QStringLiteral(R"(^([A-Z][A-Za-z0-9_]{2,40})(?:::[A-Za-z0-9_]+)?:\s+(.*)$)"),
                                           QRegularExpression::DotMatchesEverythingOption);
    // «GameInfoFetcher::fetch normalized title: …» — метод без двоеточия после
    static const QRegularExpression kMethod(QStringLiteral(R"(^([A-Z][A-Za-z0-9_]{2,40})::[A-Za-z0-9_]+\s+(.*)$)"),
                                            QRegularExpression::DotMatchesEverythingOption);
    // «main: …» из main.cpp
    static const QRegularExpression kMain(QStringLiteral(R"(^main:\s+(.*)$)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatch m = kBracket.match(msg);
    if (!m.hasMatch()) m = kColon.match(msg);
    if (!m.hasMatch()) m = kMethod.match(msg);
    if (m.hasMatch()) {
        source = m.captured(1);
        text   = unescapeQuoted(m.captured(2));
        return;
    }
    m = kMain.match(msg);
    if (m.hasMatch()) {
        source = QStringLiteral("app");
        text   = unescapeQuoted(m.captured(1));
        return;
    }
    const QString file = QString::fromUtf8(ctx.file ? ctx.file : "");
    const QString cat  = QString::fromUtf8(ctx.category ? ctx.category : "");
    if (file.endsWith(QLatin1String(".qml")) || file.endsWith(QLatin1String(".js"))
        || cat == QLatin1String("qml") || cat == QLatin1String("js")) {
        source = QStringLiteral("qml");
    } else if (!cat.isEmpty() && cat != QLatin1String("default")) {
        source = cat;                       // категории Qt: qt.multimedia и т.п.
    } else {
        source = QStringLiteral("app");
    }
    text = unescapeQuoted(msg);
    if (source == QLatin1String("qml") && !file.isEmpty())
        text = QStringLiteral("%1 (%2:%3)").arg(msg, file.section('/', -1)).arg(ctx.line);
}

static void loggerMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (s_prevHandler) s_prevHandler(type, ctx, msg);
    LoggerModule *inst = s_logInstance.loadAcquire();
    if (!inst) return;
    const char *lvl = type == QtDebugMsg   ? "debug"
                    : type == QtInfoMsg    ? "info"
                    : type == QtWarningMsg ? "warning"
                                           : "error";
    QString source, text;
    splitSource(ctx, msg, source, text);
    inst->log(QString::fromLatin1(lvl), source, text);
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

    // Консоль (IDE): время и уровень перед каждым сообщением
    qSetMessagePattern(QStringLiteral(
        "%{time hh:mm:ss.zzz} %{if-debug}DEBUG%{endif}%{if-info}INFO %{endif}"
        "%{if-warning}WARN %{endif}%{if-critical}ERROR%{endif}%{if-fatal}FATAL%{endif} %{message}"));

    s_logInstance.storeRelease(this);
    s_prevHandler = qInstallMessageHandler(loggerMessageHandler);

    // Шапка сеанса — по ней сразу видно, с какой версией и где работали
    log(QStringLiteral("info"), QStringLiteral("app"),
        QStringLiteral("MediaStation %1 | Qt %2 | %3 | %4")
            .arg(QStringLiteral(MEDIASTATION_VERSION), QString::fromLatin1(qVersion()),
                 QSysInfo::prettyProductName(), QCoreApplication::applicationDirPath()));

    qDebug().noquote() << "[Logger] file:" << m_logFilePath;
}

void LoggerModule::shutdown()
{
    if (s_logInstance.loadAcquire() == this) {
        s_logInstance.storeRelease(nullptr);
        qInstallMessageHandler(s_prevHandler);
    }
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
