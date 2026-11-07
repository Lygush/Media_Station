// tools/SevenZip.cpp
#include "SevenZip.h"

#include <QCoreApplication>
#include <QProcess>
#include <QFile>
#include <QRegularExpression>
#include <QDebug>
#include <QStandardPaths>

namespace SevenZip {

// ── Поиск бинарника ───────────────────────────────────────────────────────────

QString findBinary()
{
    const QString appDir = QCoreApplication::applicationDirPath();

    // 7zr.exe — облегчённая версия только для .7z, предпочтительна
    const QString sevenZr = appDir + "/tools/7zr.exe";
    if (QFile::exists(sevenZr))
        return sevenZr;

    // 7z.exe — полная версия, поддерживает все форматы
    const QString sevenZ = appDir + "/tools/7z.exe";
    if (QFile::exists(sevenZ))
        return sevenZ;

    // Фолбэк: ищем в PATH
    for (const char *name : {"7zr", "7z"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty()) return found;
    }
    return QStringLiteral("7zr");
}

bool isAvailable()
{
    const QString bin = findBinary();
    // Если не фолбэк — файл точно есть (мы проверили выше)
    if (bin != QLatin1String("7zr"))
        return true;
    // Фолбэк — проверяем PATH через QProcess
    QProcess p;
    p.start(bin, {"--help"});
    const bool started = p.waitForStarted(2000);
    // Дожидаемся выхода, иначе деструктор QProcess убивает работающий процесс
    if (started && !p.waitForFinished(3000))
        p.kill();
    return started;
}

// ── Внутренний запуск процесса ────────────────────────────────────────────────

static Result runProcess(const QString          &binary,
                         const QStringList      &args,
                         const QString          &workDir,
                         const std::atomic<bool> *cancelFlag,
                         ProgressFn              onProgress)
{
    // Единый regex для парсинга строк прогресса от 7zr:
    //   "  42% 12 + filename.dat"  или  "  42%"
    static const QRegularExpression kProgressRe(QStringLiteral(R"(\s*(\d+)%)"));

    QProcess proc;
    // stderr читается в цикле вместе с stdout: раньше его забирали только в
    // конце, и при тысячах предупреждений (заблокированные файлы при
    // архивации) буфер канала переполнялся — 7z зависал на записи.
    QByteArray stderrTail;
    constexpr int kStderrTailMax = 16 * 1024;
    auto drainStderr = [&]() {
        stderrTail += proc.readAllStandardError();
        if (stderrTail.size() > kStderrTailMax)
            stderrTail = stderrTail.right(kStderrTailMax);
    };
    if (!workDir.isEmpty())
        proc.setWorkingDirectory(workDir);

    proc.start(binary, args);

    if (!proc.waitForStarted(5000)) {
        qWarning() << "SevenZip: failed to start" << binary << ":" << proc.errorString();
        return StartFail;
    }

    while (!proc.waitForFinished(200)) {
        drainStderr();
        // Проверяем флаг отмены каждые 200 мс
        if (cancelFlag && cancelFlag->load()) {
            proc.kill();
            proc.waitForFinished(3000);
            return Cancelled;
        }

        if (!onProgress) {
            proc.readAllStandardOutput();   // не даём переполниться и stdout
            continue;
        }

        // Читаем stdout — 7zr пишет прогресс туда при -bsp1.
        // Прогресс перерисовывается на месте через '\b'/'\r', а не '\n' —
        // поэтому режем по всем трём и берём ПОСЛЕДНЕЕ значение в порции
        // (раньше бралось первое, и прогресс сильно отставал).
        const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
        int lastPercent = -1;
        QString lastFile;
        static const QRegularExpression kSplitRe(QStringLiteral("[\\r\\n\\x08]+"));
        for (const QString &line : out.split(kSplitRe, Qt::SkipEmptyParts)) {
            const QRegularExpressionMatch m = kProgressRe.match(line);
            if (!m.hasMatch()) continue;
            lastPercent = m.captured(1).toInt();

            // Текущий файл идёт после "+ " в строке прогресса архивации
            lastFile.clear();
            const int plusIdx = line.indexOf(QLatin1Char('+'));
            if (plusIdx >= 0)
                lastFile = line.mid(plusIdx + 2).trimmed();
        }
        if (lastPercent >= 0)
            onProgress(lastPercent, lastFile);
    }

    // Падение процесса — это ошибка, даже если exitCode() вернул 0.
    // Раньше аварийное завершение 7z считалось успехом, и после «успешной»
    // архивации GameLibrary удалял папку game/ — данные игры терялись.
    const bool crashed  = proc.exitStatus() != QProcess::NormalExit;
    const int  exitCode = proc.exitCode();

    proc.readAllStandardOutput();
    drainStderr();
    if (crashed || exitCode != 0) {
        const QString err = QString::fromLocal8Bit(stderrTail).trimmed();
        qWarning() << "SevenZip:" << (crashed ? "process crashed" : "process exited with code")
                   << (crashed ? QString() : QString::number(exitCode))
                   << "args:" << args
                   << (err.isEmpty() ? QString() : "stderr: " + err.left(2000));
        return ExitError;
    }

    if (onProgress)
        onProgress(100, {});
    return Ok;
}

// ── Публичный API ─────────────────────────────────────────────────────────────

Result compress(const QString          &workDir,
                const QString          &source,
                const QString          &archivePath,
                const std::atomic<bool> *cancelFlag,
                ProgressFn              onProgress,
                int                     level)
{
    if (cancelFlag && cancelFlag->load())
        return Cancelled;

    const QString binary = findBinary();
    if (binary == QLatin1String("7zr") && !QFile::exists(binary)) {
        qWarning() << "SevenZip::compress: binary not found";
        return NotFound;
    }

    // -bsp1 — прогресс в stdout, -mmt=on — все ядра.
    // Уровень по умолчанию 3, а не 9: файлы игр в основном уже сжаты, а -mx=9
    // на десятках ГБ — часы работы и ~0.7 ГБ ОЗУ на поток при разнице в
    // размере в несколько процентов.
    const QStringList args = {
        QStringLiteral("a"),
        QStringLiteral("-t7z"),
        QStringLiteral("-m0=lzma2"),
        QStringLiteral("-mx=%1").arg(qBound(0, level, 9)),
        QStringLiteral("-mmt=on"),
        QStringLiteral("-ms=on"),
        QStringLiteral("-bsp1"),
        archivePath,
        source
    };

    return runProcess(binary, args, workDir, cancelFlag, onProgress);
}

Result extract(const QString          &archivePath,
               const QString          &destDir,
               const std::atomic<bool> *cancelFlag,
               ProgressFn              onProgress)
{
    if (cancelFlag && cancelFlag->load())
        return Cancelled;

    const QString binary = findBinary();
    if (binary == QLatin1String("7zr") && !QFile::exists(binary)) {
        qWarning() << "SevenZip::extract: binary not found";
        return NotFound;
    }

    QStringList args = {
        QStringLiteral("x"),
        archivePath,
        QStringLiteral("-y"),
        QStringLiteral("-bsp1")
    };

    // -o задаёт папку назначения; если пустая — распаковываем в рабочую директорию
    if (!destDir.isEmpty())
        args << (QStringLiteral("-o") + destDir);

    // workDir для extract не нужен — путь к архиву абсолютный
    return runProcess(binary, args, {}, cancelFlag, onProgress);
}

qint64 unpackedSize(const QString &archivePath)
{
    const QString binary = findBinary();
    if (binary == QLatin1String("7zr") && !QFile::exists(binary)) return -1;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(binary, { QStringLiteral("l"), QStringLiteral("-slt"), archivePath });
    if (!p.waitForStarted(5000)) return -1;
    // Список читаем по мере вывода — у игр бывают сотни тысяч файлов
    qint64 total = 0;
    bool any = false;
    QByteArray tail;
    auto consume = [&](const QByteArray &chunk) {
        tail += chunk;
        int nl;
        while ((nl = tail.indexOf('\n')) >= 0) {
            const QByteArray line = tail.left(nl).trimmed();
            tail.remove(0, nl + 1);
            // «Size = N» — только у файлов; у архива — «Physical Size», «Headers Size»
            if (line.startsWith("Size = ")) {
                bool ok = false;
                const qint64 v = line.mid(7).toLongLong(&ok);
                if (ok) { total += v; any = true; }
            }
        }
    };
    while (p.state() != QProcess::NotRunning) {
        if (p.waitForReadyRead(60000)) consume(p.readAll());
        else if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(2000); return -1; }
    }
    consume(p.readAll() + '\n');
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return -1;
    return any ? total : 0;
}

const char *resultToString(Result r)
{
    switch (r) {
    case Ok:        return "Ok";
    case Cancelled: return "Cancelled";
    case NotFound:  return "NotFound";
    case StartFail: return "StartFail";
    case ExitError: return "ExitError";
    }
    return "Unknown";
}

} // namespace SevenZip
