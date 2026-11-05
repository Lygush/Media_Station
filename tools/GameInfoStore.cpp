#include "GameInfoStore.h"
#include "AtomicFile.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>

namespace {

// Один мьютекс на все игры: операции короткие (файл в несколько КБ),
// а так не нужно заводить и чистить таблицу блокировок по путям.
QMutex &storeMutex()
{
    static QMutex m;
    return m;
}

QJsonObject readUnlocked(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "GameInfoStore: bad JSON in" << path << err.errorString();
        return {};
    }
    return doc.object();
}

bool writeUnlocked(const QString &path, const QJsonObject &obj)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!AtomicFile::writeJson(path, QJsonDocument(obj))) {
        qWarning() << "GameInfoStore: cannot write" << path;
        return false;
    }
    return true;
}

} // namespace

namespace GameInfoStore {

QString filePath(const QString &gameRoot)
{
    return QDir::fromNativeSeparators(gameRoot) + "/game_info.json";
}

QJsonObject read(const QString &gameRoot)
{
    if (gameRoot.isEmpty()) return {};
    QMutexLocker lock(&storeMutex());
    return readUnlocked(filePath(gameRoot));
}

bool modify(const QString &gameRoot, const std::function<bool(QJsonObject &)> &fn)
{
    if (gameRoot.isEmpty()) return false;
    QMutexLocker lock(&storeMutex());
    const QString path = filePath(gameRoot);
    QJsonObject obj = readUnlocked(path);
    if (!fn(obj))
        return true;
    return writeUnlocked(path, obj);
}

bool update(const QString &gameRoot, const QJsonObject &patch)
{
    return modify(gameRoot, [&patch](QJsonObject &obj) {
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            if (it.value().isNull())
                obj.remove(it.key());
            else
                obj.insert(it.key(), it.value());
        }
        return true;
    });
}

bool write(const QString &gameRoot, const QJsonObject &obj)
{
    if (gameRoot.isEmpty()) return false;
    QMutexLocker lock(&storeMutex());
    return writeUnlocked(filePath(gameRoot), obj);
}

QString toStored(const QString &gameRoot, const QString &path)
{
    if (path.isEmpty() || gameRoot.isEmpty())
        return path;
    if (path.startsWith(QLatin1String("http://")) || path.startsWith(QLatin1String("https://")))
        return path;
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(gameRoot));
    const QString abs  = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (QDir::isRelativePath(abs))
        return abs;
    const QString rel = QDir(root).relativeFilePath(abs);
    // Снаружи папки игры (другой диск или "../") — оставляем абсолютный путь
    if (rel.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(rel))
        return QDir::toNativeSeparators(abs);
    return rel;
}

QString resolve(const QString &gameRoot, const QString &stored)
{
    if (stored.isEmpty() || gameRoot.isEmpty())
        return stored;
    if (stored.startsWith(QLatin1String("http://")) || stored.startsWith(QLatin1String("https://")))
        return stored;
    const QString p = QDir::fromNativeSeparators(stored);
    if (QDir::isAbsolutePath(p))
        return stored;
    return QDir::toNativeSeparators(
        QDir::cleanPath(QDir::fromNativeSeparators(gameRoot) + "/" + p));
}

const QStringList &mediaPathKeys()
{
    static const QStringList keys = { QStringLiteral("coverPath"),
                                      QStringLiteral("backdropPath"),
                                      QStringLiteral("trailerPath") };
    return keys;
}

void resolveMediaPaths(const QString &gameRoot, QVariantMap &info)
{
    for (const QString &k : mediaPathKeys()) {
        auto it = info.find(k);
        if (it != info.end() && it->typeId() == QMetaType::QString)
            *it = resolve(gameRoot, it->toString());
    }
}

} // namespace GameInfoStore
