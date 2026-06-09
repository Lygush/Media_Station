// tools/AtomicFile.cpp
#include "AtomicFile.h"

#include <QFile>
#include <QJsonDocument>
#include <QDebug>

namespace AtomicFile {

// ── Внутренняя реализация ────────────────────────────────────────────────────

static bool writeImpl(const QString &path, const QByteArray &data)
{
    const QString tmpPath = path + QStringLiteral(".tmp");

    // 1. Пишем во временный файл
    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "AtomicFile: cannot open for writing:" << tmpPath
                   << "—" << tmp.errorString();
        return false;
    }

    const qint64 written = tmp.write(data);
    tmp.close();

    if (written != data.size()) {
        qWarning() << "AtomicFile: partial write to" << tmpPath
                   << "(wrote" << written << "of" << data.size() << "bytes)";
        QFile::remove(tmpPath);
        return false;
    }

    // 2. Удаляем оригинал если есть, переименовываем tmp → оригинал
    if (QFile::exists(path))
        QFile::remove(path);

    if (!QFile::rename(tmpPath, path)) {
        qWarning() << "AtomicFile: rename failed:" << tmpPath << "→" << path;
        QFile::remove(tmpPath);
        return false;
    }

    return true;
}

// ── Публичный API ────────────────────────────────────────────────────────────

bool writeJson(const QString &path, const QJsonDocument &doc)
{
    return writeImpl(path, doc.toJson(QJsonDocument::Indented));
}

bool writeBytes(const QString &path, const QByteArray &data)
{
    return writeImpl(path, data);
}

} // namespace AtomicFile
