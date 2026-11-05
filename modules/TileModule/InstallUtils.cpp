// InstallUtils.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Реализация утилит для установки игр.
// ─────────────────────────────────────────────────────────────────────────────

#include "InstallUtils.h"
#include "tools/AtomicFile.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QStringConverter>
#include <QDebug>

namespace InstallUtils {

// ─────────────────────────────────────────────────────────────────────────────
// kSkipExePatterns
// ─────────────────────────────────────────────────────────────────────────────

const QStringList kSkipExePatterns = {
    "unins", "uninst", "setup", "install", "redist",
    "vcredist", "directx", "dxsetup", "vc_redist", "_commonredist"
};

// ─────────────────────────────────────────────────────────────────────────────
// parseDependencyAnalyzerOutput
// ─────────────────────────────────────────────────────────────────────────────

DepsResult parseDependencyAnalyzerOutput(const QString &output)
{
    static const QStringList kSectionHeaders = {
        "System Dependencies:",
        "Runtime Dependencies:",
        "Virtual Dependencies (API Sets):",
        "Other Dependencies:",
        "Summary:"
    };

    // \s+ вместо \s{2,}: PEDependencyAnalyzer может использовать одиночный таб
    // как разделитель между именем dll и путём. Имена dll пробелов не содержат,
    // поэтому первый whitespace-символ всегда является разделителем.
    static const QRegularExpression kSeparatorRe(QStringLiteral("\\s+"));

    DepsResult result;
    bool inOtherSection = false;
    bool inSummary      = false;

    const QStringList lines = output.split('\n');
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;

        // Определяем текущую секцию.
        bool isHeader = false;
        for (const QString &h : kSectionHeaders) {
            if (line.startsWith(h)) {
                inOtherSection = (h == QLatin1String("Other Dependencies:"));
                inSummary      = (h == QLatin1String("Summary:"));
                isHeader = true;
                break;
            }
        }
        if (isHeader) continue;
        if (line.startsWith("---") || line.startsWith("===")) continue;
        if (inSummary) continue;

        // Первый whitespace-разделитель делит строку на имя dll и путь.
        const int     sepIdx     = line.indexOf(kSeparatorRe);
        const QString dllName    = (sepIdx >= 0 ? line.left(sepIdx) : line).trimmed();
        const QString dllPath    = (sepIdx >= 0 ? line.mid(sepIdx).trimmed() : QString());

        if (dllName.isEmpty()) continue;

        // Определяем тип ввода по имени dll (все секции, кроме Summary).
        const QString dllLow = dllName.toLower();
        if (dllLow.contains(QLatin1String("xinput")) &&
            !result.inputTypes.contains(QLatin1String("xinput")))
            result.inputTypes << QStringLiteral("xinput");
        if (dllLow.contains(QLatin1String("dinput")) &&
            !result.inputTypes.contains(QLatin1String("directinput")))
            result.inputTypes << QStringLiteral("directinput");

        // Отсутствующие зависимости — только из "Other Dependencies" без пути.
        if (inOtherSection && dllPath.isEmpty())
            result.missing << dllName;
    }

    // Приоритет: directinput > xinput — гарантируем порядок в списке.
    // inputTypes уже заполняется в правильном порядке через contains-проверку,
    // но явно сортируем на случай непредсказуемого порядка dll в выводе.
    QStringList ordered;
    if (result.inputTypes.contains(QLatin1String("directinput")))
        ordered << QStringLiteral("directinput");
    if (result.inputTypes.contains(QLatin1String("xinput")))
        ordered << QStringLiteral("xinput");
    result.inputTypes = ordered;

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// copyFileCancelable
// ─────────────────────────────────────────────────────────────────────────────

bool copyFileCancelable(const QString &src,
                        const QString &dst,
                        const std::shared_ptr<std::atomic<bool>> &cancelFlag,
                        qint64 chunkSize)
{
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        qWarning() << "InstallUtils::copyFileCancelable: cannot open source:" << src;
        return false;
    }
    QFile out(dst);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "InstallUtils::copyFileCancelable: cannot open destination:" << dst;
        return false;
    }

    QByteArray buffer;
    buffer.resize(static_cast<int>(chunkSize));

    while (!in.atEnd()) {
        if (cancelFlag->load()) {
            out.close();
            QFile::remove(dst);
            return false;
        }
        const qint64 bytesRead = in.read(buffer.data(), chunkSize);
        if (bytesRead < 0) {
            // Ошибка чтения (битый сектор, отключённый диск). Раньше цикл просто
            // завершался, и недокопированный файл считался успешно скопированным.
            qWarning() << "InstallUtils::copyFileCancelable: read error on:" << src;
            out.close();
            QFile::remove(dst);
            return false;
        }
        if (bytesRead == 0) break;
        const qint64 written = out.write(buffer.constData(), bytesRead);
        if (written != bytesRead) {
            qWarning() << "InstallUtils::copyFileCancelable: write error on:" << dst;
            out.close();
            QFile::remove(dst);
            return false;
        }
    }
    if (!out.flush()) {
        out.close();
        QFile::remove(dst);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// generateUniqueFolderName
// ─────────────────────────────────────────────────────────────────────────────

QString sanitizeFolderName(const QString &title, const QString &fallbackId)
{
    static const QRegularExpression kInvalid("[<>:\"/\\\\|?*\\x00-\\x1f]");
    QString s = title;
    s.replace(kInvalid, "_");
    s = s.simplified();
    while (s.endsWith('.') || s.endsWith(' '))
        s.chop(1);
    if (s.size() > 100)
        s = s.left(100).trimmed();

    static const QRegularExpression kReserved(
        "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?$",
        QRegularExpression::CaseInsensitiveOption);
    if (kReserved.match(s).hasMatch())
        s.prepend('_');

    if (s.isEmpty())
        s = fallbackId.isEmpty() ? QStringLiteral("Game")
                                 : QStringLiteral("Game_") + fallbackId.left(8);
    return s;
}

bool isManagedGameFolder(const QString &folderPath)
{
    if (folderPath.trimmed().isEmpty()) return false;
    const QDir d(folderPath);
    if (!d.exists() || d.isRoot()) return false;
    return QFile::exists(d.filePath("launcher/launch.ps1"))
        || QFile::exists(d.filePath("tile_id.txt"))
        || QFile::exists(d.filePath("game_info.json"));
}

QString generateUniqueFolderName(const QString &baseName, const QString &gamesDirPath)
{
    QString candidate = baseName;
    int counter = 1;
    const QDir gamesDir(gamesDirPath);
    while (gamesDir.exists(candidate))
        candidate = baseName + QString(" (%1)").arg(counter++);
    return candidate;
}

// ─────────────────────────────────────────────────────────────────────────────
// writeTileIdFile
// ─────────────────────────────────────────────────────────────────────────────

void writeTileIdFile(const QString &gameRootPath, const QString &tileId)
{
    QFile f(gameRootPath + "/tile_id.txt");
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(tileId.toUtf8());
}

// ─────────────────────────────────────────────────────────────────────────────
// makeFolderGameId
// ─────────────────────────────────────────────────────────────────────────────

QString makeFolderGameId(const QString &folderPath)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QDir::fromNativeSeparators(folderPath).toUtf8(),
            QCryptographicHash::Md5
        ).toHex()
    ).left(12);
}

// ─────────────────────────────────────────────────────────────────────────────
// writeGameInfoFile
// ─────────────────────────────────────────────────────────────────────────────

void writeGameInfoFile(const QString &gameRootPath,
                       const QString &tileId,
                       const QString &title,
                       const QString &type,
                       const QString &exePath,
                       const QString &iconPath)
{
    const QString infoPath = gameRootPath + "/game_info.json";

    QJsonObject obj;
    obj["id"]             = makeFolderGameId(gameRootPath);
    obj["name"]           = title;
    obj["type"]           = type;
    obj["executablePath"] = exePath;
    obj["installPath"]    = QDir::toNativeSeparators(gameRootPath);
    obj["iconPath"]       = iconPath;
    obj["tileLocations"]  = QJsonArray{ tileId };
    obj["archivePath"]    = "";
    obj["installDate"]    = QDateTime::currentDateTime().toString(Qt::ISODate).left(10);
    obj["sizeCached"]     = -1;
    obj["legacyNoMetadata"] = false;
    obj["inputTypes"]     = QJsonArray{};

    // ── Медиа-данные (Фаза 1) ────────────────────────────────────────────────
    const QString mediaDir = QDir::fromNativeSeparators(gameRootPath) + "/launcher/media";
    QDir().mkpath(mediaDir);

    obj["coverPath"]    = "";
    obj["backdropPath"] = "";
    obj["trailerPath"]  = "";
    obj["mediaFetched"] = false;

    // ── Метаданные игры (Фаза 3) ─────────────────────────────────────────────
    obj["description"]  = "";
    obj["genre"]        = "";
    obj["releaseYear"]  = "";
    obj["developer"]    = "";
    obj["publisher"]    = "";
    obj["rating"]       = "";
    obj["sysreqMin"]    = "";
    obj["sysreqRec"]    = "";
    obj["infoFetched"]  = false;
    obj["steamAppId"]   = "";   // заполняется GameInfoModule после поиска в Steam

    if (!AtomicFile::writeJson(infoPath, QJsonDocument(obj)))
        qWarning() << "InstallUtils::writeGameInfoFile: failed for" << gameRootPath;
}

// ─────────────────────────────────────────────────────────────────────────────
// createLaunchScript
// ─────────────────────────────────────────────────────────────────────────────

// PowerShell: строка в одинарных кавычках, апостроф удваивается
// (раньше "Assassin's Creed" ломал синтаксис скрипта).
static QString psQuote(const QString &s)
{
    QString r = s;
    r.replace(QLatin1Char('\''), QLatin1String("''"));
    return QLatin1Char('\'') + r + QLatin1Char('\'');
}

QString createLaunchScript(const QString &gameRootPath,
                           const QString &exePathOrRelative,
                           const QString &kind)
{
    const QString launcherDir = gameRootPath + "/launcher";
    QDir().mkpath(launcherDir);

    const QString ps1Path = launcherDir + "/launch.ps1";
    QFile ps1File(ps1Path);
    if (!ps1File.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "InstallUtils::createLaunchScript: cannot create launch.ps1 at" << ps1Path;
        return {};
    }

    QTextStream out(&ps1File);
    out.setEncoding(QStringConverter::Utf8);
    // BOM обязателен: Windows PowerShell 5.1 читает файл без BOM в ANSI-кодировке,
    // и пути с кириллицей превращались в мусор → игра «не найдена».
    out.setGenerateByteOrderMark(true);

    // Общая часть: PID-файл пишем всегда, даже при ошибке запуска — иначе
    // GameWatcher ждал его 30 секунд.
    out << "# launch.ps1 — generated by MediaCenter\r\n"
        << "# DO NOT EDIT manually\r\n\r\n"
        << "param(\r\n"
        << "    [string]$PidFile     = \"\",\r\n"
        << "    [string]$ExeOverride = \"\"\r\n"
        << ")\r\n\r\n"
        << "function Write-Pid($value) {\r\n"
        << "    if ($PidFile -ne \"\") { Set-Content -LiteralPath $PidFile -Value $value -Encoding Ascii }\r\n"
        << "}\r\n\r\n"
        << "$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition\r\n"
        << "$GameRoot  = Split-Path -Parent $ScriptDir\r\n"
        << "$GameDir   = Join-Path $GameRoot 'game'\r\n\r\n";

    QString exeExpr, workDirExpr;
    if (kind == "app") {
        QString exeRel = exePathOrRelative;
        exeRel.replace('/', '\\');
        exeExpr     = "Join-Path $GameDir " + psQuote(exeRel);
        workDirExpr = "$GameDir";
    } else if (kind == "external") {
        QString exeAbs = exePathOrRelative;
        exeAbs.replace('/', '\\');
        exeExpr     = psQuote(exeAbs);
        workDirExpr = psQuote(QDir::toNativeSeparators(QFileInfo(exePathOrRelative).absolutePath()));
    } else {
        out << "Write-Pid '0'\r\n"
            << "Write-Host " << psQuote("Unsupported game kind: " + kind) << "\r\n"
            << "exit 1\r\n";
        ps1File.close();
        return QDir::toNativeSeparators(ps1Path);
    }

    out << "if ($ExeOverride -ne \"\") {\r\n"
        << "    $ExePath = $ExeOverride\r\n"
        << "} else {\r\n"
        << "    $ExePath = " << exeExpr << "\r\n"
        << "}\r\n"
        << "$WorkDir = " << workDirExpr << "\r\n\r\n"
        // -LiteralPath: квадратные скобки в именах папок не трактуются как шаблон
        << "if (-not (Test-Path -LiteralPath $ExePath)) {\r\n"
        << "    Write-Pid '0'\r\n"
        << "    exit 1\r\n"
        << "}\r\n\r\n"
        << "try {\r\n"
        << "    Set-Location -LiteralPath $WorkDir\r\n"
        << "    $proc = Start-Process -FilePath $ExePath -WorkingDirectory $WorkDir -PassThru -ErrorAction Stop\r\n"
        << "    Write-Pid $proc.Id\r\n"
        << "} catch {\r\n"
        << "    Write-Pid '0'\r\n"
        << "    exit 1\r\n"
        << "}\r\n\r\n"
        << "exit 0\r\n";

    ps1File.close();
    return QDir::toNativeSeparators(ps1Path);
}

} // namespace InstallUtils
