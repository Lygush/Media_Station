// tools/WofCompact.cpp
#include "WofCompact.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace WofCompact {

QString program()
{
    const QString overridePath = qEnvironmentVariable("MS_COMPACT_EXE");
    if (!overridePath.isEmpty()) return overridePath;
#ifdef Q_OS_WIN
    wchar_t sys[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const QString p = QDir(QString::fromWCharArray(sys, int(n))).filePath(QStringLiteral("compact.exe"));
        if (QFileInfo::exists(p)) return p;
    }
    return QStandardPaths::findExecutable(QStringLiteral("compact.exe"));
#else
    return {};
#endif
}

bool isAvailable()
{
    const QString p = program();
    return !p.isEmpty() && QFileInfo::exists(p);
}

QStringList arguments(bool compress)
{
    // /s — с подпапками, /a — и скрытые/системные, /i — не останавливаться на
    // ошибках (файл занят), /exe:lzx — самое сильное сжатие WOF; /u /exe —
    // снять именно WOF-сжатие
    if (compress)
        return { QStringLiteral("/c"), QStringLiteral("/s"), QStringLiteral("/a"),
                 QStringLiteral("/i"), QStringLiteral("/exe:lzx") };
    return { QStringLiteral("/u"), QStringLiteral("/s"), QStringLiteral("/a"),
             QStringLiteral("/i"), QStringLiteral("/exe") };
}

bool isFileLine(const QString &line)
{
    // «file.pak     1 234 567 :   456 789 = 2,7 к 1 [OK]» — разделители
    // разрядов бывают пробелами (в т.ч. неразрывными), точками и запятыми
    static const QRegularExpression re(QStringLiteral("\\d[\\d\\s.,\\x{00A0}\\x{202F}]*:\\s*\\d[\\d\\s.,\\x{00A0}\\x{202F}]*="));
    return re.match(line).hasMatch();
}

qint64 sizeOnDisk(const QFileInfo &fi)
{
#ifdef Q_OS_WIN
    DWORD high = 0;
    const QString native = QDir::toNativeSeparators(fi.absoluteFilePath());
    const DWORD low = GetCompressedFileSizeW(reinterpret_cast<LPCWSTR>(native.utf16()), &high);
    if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR)
        return (qint64(high) << 32) | qint64(low);
#endif
    return fi.size();
}

} // namespace WofCompact
