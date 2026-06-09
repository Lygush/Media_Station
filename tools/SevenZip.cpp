// tools/SevenZip.cpp
#include "SevenZip.h"

#include <QCoreApplication>
#include <QProcess>
#include <QFile>
#include <QRegularExpression>
#include <QDebug>

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
    return p.waitForStarted(2000);
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

        // Читаем stdout — 7zr пишет прогресс туда при -bsp1
        const QString out = QString::fromUtf8(proc.readAllStandardOutput());
        for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QRegularExpressionMatch m = kProgressRe.match(line);
            if (!m.hasMatch()) continue;

            const int percent = m.captured(1).toInt();

            // Текущий файл идёт после "+ " в строке прогресса архивации
            QString currentFile;
            const int plusIdx = line.indexOf(QLatin1Char('+'));
            if (plusIdx >= 0)
                currentFile = line.mid(plusIdx + 2).trimmed();

            onProgress(percent, currentFile);
        }
    }

    // Читаем оставшийся вывод после завершения
    if (onProgress) {
        const QString tail = QString::fromUtf8(proc.readAllStandardOutput());
        for (const QString &line : tail.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QRegularExpressionMatch m = kProgressRe.match(line);
            if (m.hasMatch())
                onProgress(m.captured(1).toInt(), {});
        }
    }

    const int exitCode = proc.exitCode();
    if (exitCode != 0) {
        qWarning() << "SevenZip: process exited with code" << exitCode
                   << "args:" << args;
        return ExitError;
    }

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
