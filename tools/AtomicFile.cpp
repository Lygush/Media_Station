// tools/AtomicFile.cpp
#include "AtomicFile.h"

#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QDebug>

namespace AtomicFile {

// ── Внутренняя реализация ────────────────────────────────────────────────────

static bool writeImpl(const QString &path, const QByteArray &data)
{
    // QSaveFile пишет во временный файл и заменяет оригинал атомарно
    // (MoveFileEx/rename). Раньше было remove() + rename(): при сбое между
    // ними (выключение питания, антивирус держит файл) оригинал терялся.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "AtomicFile: cannot open for writing:" << path << "—" << f.errorString();
        return false;
    }
    if (f.write(data) != data.size()) {
        qWarning() << "AtomicFile: partial write to" << path << "—" << f.errorString();
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        qWarning() << "AtomicFile: commit failed for" << path << "—" << f.errorString();
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
