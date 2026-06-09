// modules/KaraokeModule/KaraokeModule.cpp
#include "KaraokeModule.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrent>
#include <QDebug>

// Для чтения ID3-тегов используем TagLib — если недоступен,
// fallback на имя файла. Подключается опционально через DEFINES += USE_TAGLIB.
#ifdef USE_TAGLIB
#  include <taglib/fileref.h>
#  include <taglib/tag.h>
#endif

#include "KaraokeWorker.h"

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static QString sanitizeDirName(const QString &s)
{
    QString r = s.trimmed();
    // Запрещённые в именах папок Windows символы
    const QString forbidden = R"(\/:*?"<>|)";
    for (QChar c : forbidden)
        r.replace(c, '_');
    // Убираем точки в конце (Windows не любит)
    while (r.endsWith('.') || r.endsWith(' '))
        r.chop(1);
    return r.isEmpty() ? QStringLiteral("Unknown") : r;
}

// ─────────────────────────────────────────────────────────────────────────────
// KaraokeModule
// ─────────────────────────────────────────────────────────────────────────────

KaraokeModule::KaraokeModule(QObject *parent)
    : QObject(parent)
{
    m_watcher = new QFutureWatcher<void>(this);
    connect(m_watcher, &QFutureWatcher<void>::finished,
            this,      &KaraokeModule::onWorkerFinished);
}

KaraokeModule::~KaraokeModule()
{
    shutdown();
}

// ── IModule ───────────────────────────────────────────────────────────────────

void KaraokeModule::initialize(EventBus *bus, SettingsManager *)
{
    m_bus = bus;
    qDebug() << "[KaraokeModule] initialized";

    // Создаём корневую папку songs если её нет
    const QString songsRoot = QCoreApplication::applicationDirPath()
                              + "/data/songs";
    QDir().mkpath(songsRoot);
}

void KaraokeModule::shutdown()
{
    cancel();
    if (m_watcher && m_watcher->isRunning())
        m_watcher->waitForFinished();
    qDebug() << "[KaraokeModule] shutdown";
}

// ── Q_INVOKABLE ───────────────────────────────────────────────────────────────

void KaraokeModule::openSong(const QString &filePath)
{
    if (m_processing) {
        qWarning() << "[KaraokeModule] уже идёт обработка, игнорируем:" << filePath;
        return;
    }
    if (!QFile::exists(filePath)) {
        emit error(QStringLiteral("Файл не найден: ") + filePath);
        return;
    }

    m_currentSongPath = filePath;
    m_cancelled.store(false);

    // ── Проверка кэша ─────────────────────────────────────────────────────────
    if (isCached(filePath)) {
        qDebug() << "[KaraokeModule] кэш найден для" << filePath;
        const QString lrc   = cachedLrcPath(filePath);
        const QString voc   = cachedVocalsPath(filePath);
        const QString inst  = cachedInstrumentalPath(filePath);
        emit ready(lrc, voc, inst);
        if (m_bus)
            emit m_bus->moduleEvent("karaoke", "ready", {
                {"songPath",            filePath},
                {"lrcPath",             lrc},
                {"vocalsPath",          voc},
                {"instrumentalPath",    inst}
            });
        return;
    }

    // ── Читаем метаданные ─────────────────────────────────────────────────────
    const SongMeta meta = readMeta(filePath);
    const QString cacheDir = songCacheDir(filePath);
    QDir().mkpath(cacheDir);

    qDebug() << "[KaraokeModule] запуск пайплайна:"
             << meta.artist << "-" << meta.title << "→" << cacheDir;

    // ── Запускаем KaraokeWorker в фоновом потоке ──────────────────────────────
    m_processing = true;

    const QString appDir = QCoreApplication::applicationDirPath();

    auto future = QtConcurrent::run([=]() {
        KaraokeWorker worker(appDir, meta, cacheDir, &m_cancelled);

        // Прогресс пробрасываем через Qt::QueuedConnection из лямбды —
        // воркер вызывает колбэк, мы эмитируем из GUI-потока через invokeMethod
        worker.setProgressCallback([this](int pct, const QString &stage) {
            QMetaObject::invokeMethod(this, [this, pct, stage]() {
                emit progress(pct, stage);
                if (m_bus)
                    emit m_bus->moduleEvent("karaoke", "progress", {
                        {"songPath", m_currentSongPath},
                        {"percent",  pct},
                        {"stage",    stage}
                    });
            }, Qt::QueuedConnection);
        });

        worker.run();
    });

    m_watcher->setFuture(future);
}

void KaraokeModule::cancel()
{
    if (!m_processing) return;
    qDebug() << "[KaraokeModule] отмена";
    m_cancelled.store(true);
}

bool KaraokeModule::isCached(const QString &filePath) const
{
    const QString dir = songCacheDir(filePath);
    return QFile::exists(dir + "/song.lrc")
        && QFile::exists(dir + "/vocals.wav")
        && QFile::exists(dir + "/instrumental.wav");
}

QString KaraokeModule::cachedLrcPath(const QString &filePath) const
{
    const QString p = songCacheDir(filePath) + "/song.lrc";
    return QFile::exists(p) ? p : QString{};
}

QString KaraokeModule::cachedVocalsPath(const QString &filePath) const
{
    const QString p = songCacheDir(filePath) + "/vocals.wav";
    return QFile::exists(p) ? p : QString{};
}

QString KaraokeModule::cachedInstrumentalPath(const QString &filePath) const
{
    const QString p = songCacheDir(filePath) + "/instrumental.wav";
    return QFile::exists(p) ? p : QString{};
}

// ── Private slots ─────────────────────────────────────────────────────────────

void KaraokeModule::onWorkerFinished()
{
    m_processing = false;

    if (m_cancelled.load()) {
        qDebug() << "[KaraokeModule] обработка отменена";
        return;
    }

    // Проверяем результат — если файлы появились, значит успех
    if (isCached(m_currentSongPath)) {
        const QString lrc  = cachedLrcPath(m_currentSongPath);
        const QString voc  = cachedVocalsPath(m_currentSongPath);
        const QString inst = cachedInstrumentalPath(m_currentSongPath);

        emit ready(lrc, voc, inst);
        if (m_bus)
            emit m_bus->moduleEvent("karaoke", "ready", {
                {"songPath",         m_currentSongPath},
                {"lrcPath",          lrc},
                {"vocalsPath",       voc},
                {"instrumentalPath", inst}
            });
    } else {
        // Воркер завершился без результата — значит была ошибка.
        // Воркер уже вызвал setProgressCallback с деталями,
        // здесь эмитируем общий сигнал ошибки.
        const QString msg = QStringLiteral("Не удалось обработать: ")
                            + QFileInfo(m_currentSongPath).fileName();
        emit error(msg);
        if (m_bus)
            emit m_bus->moduleEvent("karaoke", "error", {
                {"songPath", m_currentSongPath},
                {"message",  msg}
            });
    }
}

// ── Private helpers ───────────────────────────────────────────────────────────

QString KaraokeModule::songCacheDir(const QString &filePath) const
{
    const SongMeta meta = readMeta(filePath);
    const QString dirName = sanitizeDirName(meta.artist) + " - " + sanitizeDirName(meta.title);
    return QCoreApplication::applicationDirPath()
           + "/data/songs/" + dirName;
}

SongMeta KaraokeModule::readMeta(const QString &filePath) const
{
    SongMeta meta;
    meta.filePath = filePath;

#ifdef USE_TAGLIB
    TagLib::FileRef f(filePath.toLocal8Bit().constData());
    if (!f.isNull() && f.tag()) {
        meta.artist = QString::fromStdWString(f.tag()->artist().toWString()).trimmed();
        meta.title  = QString::fromStdWString(f.tag()->title().toWString()).trimmed();
        meta.album  = QString::fromStdWString(f.tag()->album().toWString()).trimmed();
    }
    if (!f.isNull() && f.audioProperties())
        meta.durationSec = f.audioProperties()->lengthInSeconds();
#endif

    // Fallback: парсим имя файла по " - "
    if (meta.artist.isEmpty() && meta.title.isEmpty()) {
        const QString stem = QFileInfo(filePath).completeBaseName();
        const int sep = stem.indexOf(" - ");
        if (sep > 0) {
            meta.artist = stem.left(sep).trimmed();
            meta.title  = stem.mid(sep + 3).trimmed();
        } else {
            meta.artist = QStringLiteral("Unknown");
            meta.title  = stem;
        }
    }

    return meta;
}
