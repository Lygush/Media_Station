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
    if (!workDir.isEmpty())
        proc.setWorkingDirectory(workDir);

    proc.start(binary, args);

    if (!proc.waitForStarted(5000)) {
        qWarning() << "SevenZip: failed to start" << binary << ":" << proc.errorString();
        return StartFail;
    }

    while (!proc.waitForFinished(200)) {
        // Проверяем флаг отмены каждые 200 мс
        if (cancelFlag && cancelFlag->load()) {
            proc.kill();
            proc.waitForFinished(3000);
            return Cancelled;
        }

        if (!onProgress) continue;

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
    if (crashed || exitCode != 0) {
        const QString err = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
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
                ProgressFn              onProgress)
{
    if (cancelFlag && cancelFlag->load())
        return Cancelled;

    const QString binary = findBinary();
    if (binary == QLatin1String("7zr") && !QFile::exists(binary)) {
        qWarning() << "SevenZip::compress: binary not found";
        return NotFound;
    }

    // -bsp1 — прогресс в stdout
    // -mqs=on — сортировка для лучшего сжатия
    const QStringList args = {
        QStringLiteral("a"),
        QStringLiteral("-t7z"),
        QStringLiteral("-m0=lzma2"),
        QStringLiteral("-mx=9"),
        QStringLiteral("-mfb=64"),
        QStringLiteral("-md=32m"),
        QStringLiteral("-ms=on"),
        QStringLiteral("-mqs=on"),
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
