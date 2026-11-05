// modules/UpdateModule/UpdateManager.cpp
// ─────────────────────────────────────────────────────────────────────────────

#include "UpdateManager.h"
#include "core/EventBus.h"
#include "LoggerModule.h"
#include "NotificationModule.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QVersionNumber>
#include <QDebug>
#include <QTimer>
#include <QDir>
#include <QDateTime>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>

#ifndef MEDIASTATION_VERSION
#  define MEDIASTATION_VERSION "1.0.0"
#endif

// Архивы обновлений, принятые по сети, лежат в %TEMP%/MediaStation_Update
// (NetworkModule::prepareUpdateArchivePath). Удаляем только файлы оттуда,
// чтобы случайно не стереть что-то по чужому пути.
static void removeReceivedArchive(const QString &path)
{
    if (path.isEmpty()) return;
    const QString dir = QDir::cleanPath(QDir::temp().absoluteFilePath("MediaStation_Update"));
    const QString file = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!file.startsWith(dir + "/", Qt::CaseInsensitive)) return;
    if (QFile::remove(file))
        qDebug() << "[UpdateManager] removed update archive:" << file;
}

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent)
    , m_appDir(QCoreApplication::applicationDirPath())
{}

void UpdateManager::initialize(EventBus *bus, SettingsManager *)
{
    m_bus = bus;

    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data)
            {
                if (mod != QLatin1String("updater")) return;

                if (ev == QLatin1String("updateReceived")) {
                    onUpdateReceived(
                        data.value("version").toString(),
                        data.value("archivePath").toString(),
                        data.value("expectedHash").toString(),
                        data.value("releaseNotes").toString());

                } else if (ev == QLatin1String("downloadProgress")) {
                    onDownloadProgress(data.value("progress").toReal());
                }
            });

    qDebug() << "[UpdateManager] Initialized, current version:" << currentVersion();

    // Updater не может удалить свою же временную папку (внутри лежит его
    // запущенный exe), поэтому там копились копии Updater от каждого обновления.
    // Чистим устаревшие (старше суток) в фоне.
    (void)QtConcurrent::run([]() {
        // Старые архивы обновлений (отклонённые, прерванные, от прошлых версий)
        const QDir arch(QDir::temp().absoluteFilePath("MediaStation_Update"));
        if (arch.exists()) {
            const QDateTime limit = QDateTime::currentDateTime().addDays(-1);
            for (const QFileInfo &f : arch.entryInfoList({"update_*.7z", "update_*.7z.part"}, QDir::Files))
                if (f.lastModified() < limit)
                    QFile::remove(f.absoluteFilePath());
        }

        const QDir base(QDir::temp().absoluteFilePath("MediaStationUpdater"));
        if (!base.exists()) return;
        const QDateTime limit = QDateTime::currentDateTime().addDays(-1);
        const QFileInfoList dirs = base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &d : dirs) {
            if (d.lastModified() < limit)
                QDir(d.absoluteFilePath()).removeRecursively();
        }
    });
}

void UpdateManager::onUpdateReceived(const QString &version,
                                     const QString &archivePath,
                                     const QString &expectedHash,
                                     const QString &releaseNotes)
{
    const QVersionNumber incoming = QVersionNumber::fromString(version);
    const QVersionNumber current  = QVersionNumber::fromString(currentVersion());

    if (incoming <= current) {
        qDebug() << "[UpdateManager] Received version" << version
                 << "is not newer than current" << currentVersion() << "— ignoring";
        if (m_bus) {
            emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                    Notifications::makeEvent(
                                        QString("Обновление %1: версия не новее текущей (%2).")
                                            .arg(version, currentVersion()),
                                        NotificationLevel::Info, true, 4000));
        }
        if (archivePath != m_archivePath)
            removeReceivedArchive(archivePath);
        return;
    }

    // Пришёл новый архив, а предыдущий ещё ждёт — старый больше не нужен
    if (!m_archivePath.isEmpty() && m_archivePath != archivePath && !m_preparing)
        removeReceivedArchive(m_archivePath);

    m_newVersion    = version;
    m_archivePath   = archivePath;
    m_expectedHash  = expectedHash;
    m_releaseNotes  = releaseNotes;
    m_updateReady   = true;
    m_downloading   = false;

    emit updateReadyChanged();
    emit downloadingChanged();

    if (m_bus) {
        emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                Logger::makeEvent(Logger::kLevelInfo, "UpdateManager",
                                                  qPrintable(QString("Update available: %1").arg(version))));
    }
}

void UpdateManager::onDownloadProgress(qreal progress)
{
    if (!m_downloading) {
        m_downloading = true;
        emit downloadingChanged();
    }
    m_downloadProgress = progress;
    emit downloadProgressChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// Подготовка временной папки и запуск Updater
// ─────────────────────────────────────────────────────────────────────────────

bool UpdateManager::copyDirectory(const QString &srcPath, const QString &dstPath)
{
    QDir srcDir(srcPath);
    if (!srcDir.exists())
        return false;

    QDir dstDir;
    if (!dstDir.mkpath(dstPath))
        return false;

    bool success = true;
    const QFileInfoList entries = srcDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QString srcItem = entry.absoluteFilePath();
        const QString dstItem = dstPath + QDir::separator() + entry.fileName();

        if (entry.isDir()) {
            if (!copyDirectory(srcItem, dstItem))
                success = false;
        } else {
            if (QFile::exists(dstItem))
                QFile::remove(dstItem);
            if (!QFile::copy(srcItem, dstItem))
                success = false;
        }
    }
    return success;
}

QString UpdateManager::prepareTempUpdater()
{
    // Исходная папка Updater (рядом с MediaStation.exe)
    const QString updaterSrcDir = m_appDir + "/Updater";
    if (!QDir(updaterSrcDir).exists()) {
        qWarning() << "[UpdateManager] Updater folder not found at:" << updaterSrcDir;
        return QString();
    }

    // Уникальная временная папка
    const QString baseTemp = QDir::temp().absoluteFilePath("MediaStationUpdater");
    QDir().mkpath(baseTemp);
    const QString tempDir = baseTemp + "/" + QString::number(QDateTime::currentSecsSinceEpoch());
    if (!QDir().mkpath(tempDir)) {
        qWarning() << "[UpdateManager] Failed to create temp dir:" << tempDir;
        return QString();
    }

    // Копируем всю папку Updater во временную
    const QString dstUpdaterDir = tempDir + "/Updater";
    if (!copyDirectory(updaterSrcDir, dstUpdaterDir)) {
        qWarning() << "[UpdateManager] Failed to copy Updater folder from" << updaterSrcDir << "to" << dstUpdaterDir;
        QDir(tempDir).removeRecursively();
        return QString();
    }

    // Updater'у из tools нужен только архиватор (UpdaterWindow ищет tools/7zr.exe).
    // Раньше копировалась ВСЯ папка tools — Android SDK, JDK, системные образы
    // (гигабайты) — обновление шло десятки минут и могло переполнить диск.
    const QString toolsSrc = m_appDir + "/tools";
    const QString toolsDst = dstUpdaterDir + "/tools";
    QDir().mkpath(toolsDst);
    int copied = 0;
    for (const QString &name : {QStringLiteral("7zr.exe"), QStringLiteral("7z.exe"), QStringLiteral("7z.dll")}) {
        const QString src = toolsSrc + "/" + name;
        if (!QFile::exists(src)) continue;
        QFile::remove(toolsDst + "/" + name);
        if (QFile::copy(src, toolsDst + "/" + name)) ++copied;
        else qWarning() << "[UpdateManager] Failed to copy" << src;
    }
    if (copied == 0)
        qWarning() << "[UpdateManager] 7zr.exe not found in" << toolsSrc
                   << "— Updater will fall back to the target dir";

    qDebug() << "[UpdateManager] Prepared temp updater at:" << dstUpdaterDir;
    return tempDir; // возвращаем корень временной папки
}

// ─────────────────────────────────────────────────────────────────────────────
// acceptUpdate — тяжёлые операции (verifyHash + copyDirectory) вынесены
// в фоновый поток через QtConcurrent, чтобы UI не замерзал.
// ─────────────────────────────────────────────────────────────────────────────

void UpdateManager::acceptUpdate()
{
    if (!m_updateReady || m_preparing) return;

    // Сразу переходим в состояние "подготовка" — диалог покажет спиннер,
    // повторные нажатия кнопки будут заблокированы
    m_preparing   = true;
    m_updateReady = false;
    emit preparingChanged();
    emit updateReadyChanged();

    qDebug() << "[UpdateManager] User accepted update to" << m_newVersion;

    // Быстрая синхронная проверка наличия архива
    if (m_archivePath.isEmpty() || !QFile::exists(m_archivePath)) {
        qWarning() << "[UpdateManager] Archive not found:" << m_archivePath;
        m_preparing = false;
        emit preparingChanged();
        if (m_bus) {
            emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                    Notifications::makeEvent(
                                        "Ошибка обновления: архив не найден.",
                                        NotificationLevel::Error));
        }
        return;
    }

    // Захватываем все нужные данные до ухода в фоновый поток
    const QString archivePath  = m_archivePath;
    const QString expectedHash = m_expectedHash;
    const QString newVersion   = m_newVersion;
    const QString appDir       = m_appDir;

    // Структура результата фонового шага
    struct BgResult {
        bool    hashOk      = true;
        bool    hashChecked = false;
        QString tempDir;
    };

    // Запускаем тяжёлые операции (verifyHash + copyDirectory) в фоне
    auto *watcher = new QFutureWatcher<BgResult>(this);

    QFuture<BgResult> future = QtConcurrent::run(
        [this, archivePath, expectedHash]() -> BgResult
        {
            BgResult res;
            res.hashChecked = !expectedHash.isEmpty();
            if (res.hashChecked) {
                res.hashOk = verifyHash(archivePath, expectedHash);
                if (!res.hashOk)
                    return res; // копировать смысла нет
            }
            res.tempDir = prepareTempUpdater();
            return res;
        });

    connect(watcher, &QFutureWatcher<BgResult>::finished, this,
            [this, watcher, archivePath, expectedHash, newVersion, appDir]()
            {
                const BgResult res = watcher->result();
                watcher->deleteLater();

                // Хэш не совпал — сообщаем и удаляем повреждённый архив
                if (res.hashChecked && !res.hashOk) {
                    qWarning() << "[UpdateManager] Hash mismatch for" << archivePath;
                    m_preparing = false;
                    emit preparingChanged();
                    if (m_bus) {
                        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                Notifications::makeEvent(
                                                    "Ошибка обновления: контрольная сумма не совпадает. Файл повреждён.",
                                                    NotificationLevel::Error));
                    }
                    QFile::remove(archivePath);
                    m_updateReady = false;
                    emit updateReadyChanged();
                    return;
                }

                // Не удалось подготовить временную папку
                if (res.tempDir.isEmpty()) {
                    m_preparing = false;
                    m_updateReady = true;   // архив цел — даём возможность повторить
                    emit preparingChanged();
                    emit updateReadyChanged();
                    if (m_bus) {
                        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                Notifications::makeEvent(
                                                    "Ошибка обновления: не удалось подготовить среду для обновления.",
                                                    NotificationLevel::Error));
                    }
                    return;
                }

                const QString updaterExe = res.tempDir + "/Updater/Updater.exe";
                if (!QFile::exists(updaterExe)) {
                    qWarning() << "[UpdateManager] Updater.exe not found after copy to" << updaterExe;
                    m_preparing = false;
                    m_updateReady = true;
                    emit preparingChanged();
                    emit updateReadyChanged();
                    if (m_bus) {
                        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                Notifications::makeEvent(
                                                    "Ошибка обновления: Updater.exe отсутствует.",
                                                    NotificationLevel::Error));
                    }
                    QDir(res.tempDir).removeRecursively();
                    return;
                }

                // Меняем рабочий каталог, чтобы не держать блокировку на папке приложения.
                // Запоминаем прежний: если Updater не запустится, приложение продолжит
                // работать, и его относительные пути не должны сломаться.
                const QString prevCwd = QDir::currentPath();
                QDir::setCurrent(QDir::tempPath());

                const QStringList args = {
                    "--archive",  archivePath,
                    "--target",   appDir,
                    "--hash",     expectedHash,
                    "--wait-pid", QString::number(QCoreApplication::applicationPid()),
                    "--version",  newVersion,
                    "--temp-dir", res.tempDir
                };

                qDebug() << "[UpdateManager] Launching Updater from temp:" << updaterExe << args;

                if (!QProcess::startDetached(updaterExe, args, QDir::tempPath())) {
                    qWarning() << "[UpdateManager] startDetached failed for:" << updaterExe;
                    QDir::setCurrent(prevCwd);
                    m_preparing = false;
                    m_updateReady = true;
                    emit preparingChanged();
                    emit updateReadyChanged();
                    if (m_bus) {
                        emit m_bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                                                Notifications::makeEvent(
                                                    "Ошибка обновления: не удалось запустить Updater.",
                                                    NotificationLevel::Error));
                    }
                    QDir(res.tempDir).removeRecursively();
                    return;
                }

                if (m_bus) {
                    emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                            Logger::makeEvent(Logger::kLevelInfo, "UpdateManager",
                                                              "Updater launched from temp dir, quitting MediaStation"));
                }

                // 200мс достаточно — Updater дождётся завершения через --wait-pid
                QTimer::singleShot(200, this, []() { QCoreApplication::quit(); });
            });

    watcher->setFuture(future);
}

void UpdateManager::declineUpdate()
{
    qDebug() << "[UpdateManager] User declined update";
    // Архив весит сотни мегабайт; раньше он оставался во временной папке навсегда.
    // Если обновление понадобится, сервер отправит его заново.
    if (!m_preparing) {
        removeReceivedArchive(m_archivePath);
        m_archivePath.clear();
    }
    m_updateReady = false;
    emit updateReadyChanged();
}

bool UpdateManager::verifyHash(const QString &filePath,
                               const QString &expectedHash) const
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[UpdateManager] Cannot open for hash check:" << filePath;
        return false;
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    if (!hasher.addData(&f)) {
        qWarning() << "[UpdateManager] Hash computation failed for:" << filePath;
        return false;
    }

    const QString actual = QString::fromLatin1(hasher.result().toHex());
    const bool ok = actual.compare(expectedHash, Qt::CaseInsensitive) == 0;

    qDebug() << "[UpdateManager] Hash check:"
             << (ok ? "OK" : "FAIL")
             << "\n  expected:" << expectedHash
             << "\n  actual  :" << actual;
    return ok;
}

QString UpdateManager::currentVersion() const
{
    return QStringLiteral(MEDIASTATION_VERSION);
}
