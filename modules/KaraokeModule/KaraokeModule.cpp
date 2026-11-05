// modules/KaraokeModule/KaraokeModule.cpp
#include "KaraokeModule.h"
#include <QRegularExpression>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrent>
#include <QDebug>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QMetaObject>

// Для чтения ID3-тегов используем TagLib — если недоступен,
// fallback на имя файла. Подключается опционально через DEFINES += USE_TAGLIB.
#ifdef USE_TAGLIB
#  include <taglib/fileref.h>
#  include <taglib/tag.h>
#endif

#include "KaraokeWorker.h"
#include "SongNameResolver.h"

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

// Разбор имени файла: «Nirvana_-_Smells_Like_Teen_Spirit_47829249» → artist/title.
struct ParsedName {
    QString artist;      // пусто, если разделителя « - » нет
    QString title;
    QString cleanStem;   // имя без расширения, ID и подчёркиваний
    bool    dirty = false; // похоже на «грязное» имя (ID, сайты, скобки, official…)
};

static ParsedName parseFileName(const QString &filePath)
{
    ParsedName r;
    const QString rawStem = QFileInfo(filePath).completeBaseName();

    static const QRegularExpression kDirtyRe(
        QStringLiteral(R"((\d|_|[\[\](){}]|www\.|\.(com|ru|net|org)\b|official|video|lyrics|)"
                       R"(audio|\bhq\b|скачать|zaycev|muzofond|mp3))"),
        QRegularExpression::CaseInsensitiveOption);
    r.dirty = kDirtyRe.match(rawStem).hasMatch();

    QString stem = rawStem;
    stem.replace(QLatin1Char('_'), QLatin1Char(' '));
    static const QRegularExpression kTrailingIdRe(QStringLiteral("\\s+\\d{5,}$"));
    static const QRegularExpression kSepRe(QStringLiteral("\\s[-\u2013\u2014]\\s"));
    stem.remove(kTrailingIdRe);
    stem = stem.simplified();
    r.cleanStem = stem;

    const QRegularExpressionMatch m = kSepRe.match(stem);
    if (m.hasMatch() && m.capturedStart() > 0) {
        r.artist = stem.left(m.capturedStart()).trimmed();
        r.title  = stem.mid(m.capturedEnd()).trimmed();
    } else {
        r.title = stem;
    }
    return r;
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
    loadOverrides();
    m_resolver = new SongNameResolver(m_bus);
}

void KaraokeModule::shutdown()
{
    cancel();
    if (m_analysisFuture.isValid())
        m_analysisFuture.waitForFinished();
    if (m_watcher && m_watcher->isRunning())
        m_watcher->waitForFinished();

    // Нейросеть для анализа имён освобождаем только после остановки потоков
    delete m_resolver;
    m_resolver = nullptr;
    qDebug() << "[KaraokeModule] shutdown";
}

// ── Q_INVOKABLE ───────────────────────────────────────────────────────────────

void KaraokeModule::openSong(const QString &filePathOrUrl)
{
    // QML может передать file:-URL (в т.ч. сетевой путь) — приводим к локальному пути
    const QString filePath = filePathOrUrl.startsWith(QLatin1String("file:"))
                                 ? QUrl(filePathOrUrl).toLocalFile()
                                 : filePathOrUrl;
    if (m_processing || m_analyzing) {
        qWarning() << "[KaraokeModule] уже идёт обработка, игнорируем:" << filePath;
        return;
    }
    if (!QFile::exists(filePath)) {
        emit error(QStringLiteral("Файл не найден: ") + filePath);
        return;
    }

    m_currentSongPath = filePath;
    m_cancelled.store(false);

    // Название уже подтверждалось раньше и результат лежит в кэше — открываем сразу
    if (m_overrides.contains(filePath) && isCached(filePath)) {
        qDebug() << "[KaraokeModule] кэш найден для" << filePath;
        emitReady(filePath);
        return;
    }

    // Иначе: быстрый анализ имени → metaProposed → пользователь → confirmSong()
    m_analyzing = true;
    m_analysisFuture = QtConcurrent::run([this, filePath]() { analyzeSong(filePath); });
}

void KaraokeModule::confirmSong(const QString &artist, const QString &title)
{
    if (m_processing || m_analyzing) return;
    if (m_currentSongPath.isEmpty() || !QFile::exists(m_currentSongPath)) {
        emit error(QStringLiteral("Файл не найден: ") + m_currentSongPath);
        return;
    }
    const QString t = title.simplified();
    const QString a = artist.simplified();
    if (t.isEmpty()) return;                       // QML не даёт нажать, но на всякий случай

    m_overrides[m_currentSongPath] = { a.isEmpty() ? QStringLiteral("Unknown") : a, t };
    saveOverrides();
    m_cancelled.store(false);

    if (isCached(m_currentSongPath)) {
        qDebug() << "[KaraokeModule] кэш найден для" << m_currentSongPath;
        emitReady(m_currentSongPath);
        return;
    }
    startPipeline(readMeta(m_currentSongPath));
}

void KaraokeModule::startPipeline(const SongMeta &meta)
{
    const QString cacheDir = songCacheDir(meta.filePath);
    QDir().mkpath(cacheDir);

    qDebug() << "[KaraokeModule] запуск пайплайна:"
             << meta.artist << "-" << meta.title << "→" << cacheDir;

    m_processing = true;
    const QString appDir = QCoreApplication::applicationDirPath();

    auto future = QtConcurrent::run([this, appDir, meta, cacheDir]() {
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

void KaraokeModule::emitReady(const QString &filePath)
{
    const QString lrc  = cachedLrcPath(filePath);
    const QString voc  = cachedVocalsPath(filePath);
    const QString inst = cachedInstrumentalPath(filePath);
    emit ready(lrc, voc, inst);
    if (m_bus)
        emit m_bus->moduleEvent("karaoke", "ready", {
            {"songPath",         filePath},
            {"lrcPath",          lrc},
            {"vocalsPath",       voc},
            {"instrumentalPath", inst}
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Анализ имени файла (фоновый поток)
// ─────────────────────────────────────────────────────────────────────────────

void KaraokeModule::analyzeSong(const QString &filePath)
{
    auto toGui = [this](std::function<void()> fn) {
        QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
    };
    auto status = [&](const QString &msg) {
        toGui([this, msg]() { emit analysisStatus(msg); });
    };

    status(QStringLiteral("Читаю теги и имя файла…"));

    QString source;
    const SongMeta raw = readMetaRaw(filePath, &source);
    const ParsedName pn = parseFileName(filePath);

    const bool artistUnknown = raw.artist.isEmpty()
                               || raw.artist.compare(QLatin1String("Unknown"), Qt::CaseInsensitive) == 0;
    QString artist = artistUnknown ? QString() : raw.artist;
    QString title  = raw.title;

    // Нейросеть нужна, когда тегов нет и имя «нечистое» либо исполнитель не угадан
    const bool needLlm = source != QLatin1String("tags") && (artistUnknown || pn.dirty);

    if (needLlm && !m_cancelled.load() && m_resolver) {
        const SongNameResolver::Result ai = m_resolver->resolve(
            QFileInfo(filePath).fileName(),
            [&](const QString &tok) { toGui([this, tok]() { emit analysisToken(tok); }); },
            status);

        if (!ai.message.isEmpty())
            status(ai.message);

        if (!ai.title.isEmpty()) {
            artist = ai.artist;
            title  = ai.title;
            source = QStringLiteral("ai");
        } else if (!ai.artist.isEmpty() && artist.isEmpty()) {
            artist = ai.artist;                       // название из имени файла, исполнитель от нейросети
            source = QStringLiteral("ai");
        }
    }

    // Завершающий год («Апрель 2025») — не часть названия; числа внутри названия не трогаем
    title = SongNameResolver::stripTrailingYear(title);

    if (m_cancelled.load()) {
        toGui([this]() {
            m_analyzing = false;
            emit error(QStringLiteral("Обработка отменена"));
        });
        return;
    }

    if (title.trimmed().isEmpty()) {          // не удалось — пусть введёт вручную
        title  = pn.cleanStem;
        source = QStringLiteral("manual");
    }

    const QString fileName = QFileInfo(filePath).fileName();
    toGui([this, artist, title, source, fileName]() {
        m_analyzing = false;
        emit metaProposed(artist, title, source, fileName);
    });
}

void KaraokeModule::cancel()
{
    if (!m_processing && !m_analyzing) return;
    qDebug() << "[KaraokeModule] отмена";
    m_cancelled.store(true);
    if (m_analyzing && m_resolver)
        m_resolver->abort();
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
        // Раньше при отмене не отправлялось ничего — экран караоке навсегда
        // оставался в состоянии «обработка» с прогресс-баром.
        emit error(QStringLiteral("Обработка отменена"));
        if (m_bus)
            emit m_bus->moduleEvent("karaoke", "cancelled", {{"songPath", m_currentSongPath}});
        return;
    }

    // Проверяем результат — если файлы появились, значит успех
    if (isCached(m_currentSongPath)) {
        emitReady(m_currentSongPath);
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
    SongMeta meta = readMetaRaw(filePath, nullptr);
    const auto it = m_overrides.constFind(filePath);
    if (it != m_overrides.constEnd()) {
        meta.artist = it->first;
        meta.title  = it->second;
    }
    return meta;
}

SongMeta KaraokeModule::readMetaRaw(const QString &filePath, QString *source) const
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

    const bool fromTags = !meta.artist.isEmpty() && !meta.title.isEmpty();
    if (source) *source = fromTags ? QStringLiteral("tags") : QStringLiteral("filename");

    // Недостающее берём из имени файла «Артист - Название» (подчёркивания, числовой ID)
    if (!fromTags) {
        const ParsedName pn = parseFileName(filePath);
        if (meta.artist.isEmpty())
            meta.artist = pn.artist.isEmpty() ? QStringLiteral("Unknown") : pn.artist;
        if (meta.title.isEmpty())
            meta.title = pn.title;
    }
    return meta;
}

// ── Подтверждённые названия ───────────────────────────────────────────────────

QString KaraokeModule::overridesFile() const
{
    return QCoreApplication::applicationDirPath() + "/data/songs/meta_overrides.json";
}

void KaraokeModule::loadOverrides()
{
    m_overrides.clear();
    QFile f(overridesFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        if (!o.value("title").toString().isEmpty())
            m_overrides.insert(it.key(), { o.value("artist").toString(), o.value("title").toString() });
    }
}

void KaraokeModule::saveOverrides() const
{
    QJsonObject root;
    for (auto it = m_overrides.constBegin(); it != m_overrides.constEnd(); ++it)
        root.insert(it.key(), QJsonObject{ {"artist", it->first}, {"title", it->second} });
    QFile f(overridesFile());
    QDir().mkpath(QFileInfo(f).absolutePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString KaraokeModule::readTextFile(const QString &pathOrUrl) const
{
    const QString path = pathOrUrl.startsWith(QLatin1String("file:"))
                             ? QUrl(pathOrUrl).toLocalFile()
                             : pathOrUrl;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[KaraokeModule] не удалось открыть файл:" << path << f.errorString();
        return {};
    }
    return QString::fromUtf8(f.readAll());
}
