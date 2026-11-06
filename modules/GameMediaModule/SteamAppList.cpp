// modules/GameMediaModule/SteamAppList.cpp
#include "SteamAppList.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QThreadPool>
#include <QDebug>

SteamAppList &SteamAppList::instance()
{
    static SteamAppList s_instance;
    return s_instance;
}

SteamAppList::SteamAppList(QObject *parent) : QObject(parent) {}

QString SteamAppList::localFilePath() const
{
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/tools/applicationslist/applications.json");
}

QString SteamAppList::normalizeKey(const QString &name) const
{
    QString key = name.toLower();
    for (QChar &ch : key) {
        if (ch.isSpace()) ch = QLatin1Char(' ');
    }
    static const QRegularExpression kNonKeyRe(QStringLiteral("[^a-z0-9 ]"));
    key.remove(kNonKeyRe);
    return key.simplified();
}

// ── Публичный API ─────────────────────────────────────────────────────────────

void SteamAppList::loadIfNeeded()
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_loaded || m_loading) return;
        m_loading   = true;
        m_cancelled = false;
    }

    QThreadPool::globalInstance()->start([this]() { loadFromFile(); });
}

// Блокирует вызывающий поток до завершения загрузки.
// Вызывать ТОЛЬКО из фонового потока — не из главного.
void SteamAppList::waitUntilLoaded()
{
    // Ждём только пока загрузка реально идёт. Раньше условие не учитывало
    // неудачную загрузку (нет tools/applicationslist/applications.json или
    // битый JSON) — поток ждал вечно, и описание игры «висело» на этапе БД.
    QMutexLocker locker(&m_mutex);
    while (m_loading && !m_loaded && !m_cancelled)
        m_loadedCond.wait(&m_mutex);
}

void SteamAppList::cancel()
{
    {
        QMutexLocker locker(&m_mutex);
        m_cancelled = true;
    }
    m_loadedCond.wakeAll();
}

// ── Фоновое чтение файла ─────────────────────────────────────────────────────

void SteamAppList::loadFromFile()
{
    const QString path = localFilePath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "SteamAppList: cannot open" << path;
        QMutexLocker locker(&m_mutex);
        m_loading = false;
        m_loadedCond.wakeAll();
        return;
    }

    const QByteArray raw = f.readAll();
    f.close();

    if (m_cancelled) {
        QMutexLocker locker(&m_mutex);
        m_loading = false;
        m_loadedCond.wakeAll();
        return;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        qWarning() << "SteamAppList: JSON parse error:" << err.errorString();
        QMutexLocker locker(&m_mutex);
        m_loading = false;
        m_loadedCond.wakeAll();
        return;
    }

    const QJsonArray arr = doc.array();

    QHash<QString, int>      nameToId;
    QHash<int, QString>      appName;
    QHash<int, LocalAppInfo> appInfo;
    nameToId.reserve(arr.size());
    appName.reserve(arr.size());
    appInfo.reserve(arr.size());

    for (const QJsonValue &val : arr) {
        if (m_cancelled) break;

        const QJsonObject obj = val.toObject();
        const int     appid = obj["appid"].toInt();
        const QString name  = obj["name"].toString().trimmed();
        const QString type  = obj["type"].toString();

        if (appid <= 0 || name.isEmpty()) continue;

        if (type == QLatin1String("game")) {
            const QString key = normalizeKey(name);
            if (!key.isEmpty()) {
                nameToId.insert(key, appid);
                appName.insert(appid, name);  // сохраняем оригинальное имя
            }
        }

        LocalAppInfo info;
        info.appid            = appid;
        info.type             = type;
        info.shortDescription = obj["short_description"].toString();
        info.releaseDate      = obj["release_date"].toString();
        info.headerImage      = obj["header_image"].toString();
        info.background       = obj["background"].toString();

        const QJsonValue mcVal = obj["metacritic_score"];
        info.metacriticScore  = mcVal.isNull() ? -1.0 : mcVal.toDouble(-1.0);

        appInfo.insert(appid, info);
    }

    qDebug() << "SteamAppList: parsed" << nameToId.size() << "games,"
             << appInfo.size() << "total entries from" << path;

    // Заполняем данные прямо в фоновом потоке под мьютексом,
    // затем будим waitUntilLoaded() и уведомляем главный поток через сигнал.
    // QueuedConnection для emit loaded() — чтобы слоты в главном потоке
    // сработали когда он освободится, без блокировки.
    {
        QMutexLocker locker(&m_mutex);
        if (!m_cancelled) {
            m_nameToId = std::move(nameToId);
            m_appName  = std::move(appName);
            m_appInfo  = std::move(appInfo);
            m_loaded   = true;
        }
        m_loading = false;
        m_loadedCond.wakeAll();
    }

    if (m_loaded) {
        qDebug() << "SteamAppList: ready," << m_nameToId.size() << "games indexed";
        emit loaded();
    }
}

// ── Поиск ────────────────────────────────────────────────────────────────────

double SteamAppList::titleSimilarity(const QString &query, const QString &candidate) const
{
    if (candidate == query) return 1.0;

    // Кандидат начинается с запроса — высший приоритет после точного совпадения
    // ("the witcher 3: wild hunt" startsWith "the witcher 3")
    if (candidate.startsWith(query)) return 0.95;

    static const QSet<QString> kStopWords = {
        "the", "a", "an", "of", "and", "in", "to", "for", "on", "at"
    };

    const QStringList qWords = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QStringList cWords = candidate.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    // Значимые слова запроса: не стоп-слова; цифры значимы всегда (даже однозначные).
    QStringList sigWords;
    for (const QString &w : qWords) {
        if (kStopWords.contains(w)) continue;
        const bool isNumber = std::all_of(w.begin(), w.end(), [](QChar c){ return c.isDigit(); });
        if (isNumber || w.length() >= 2)
            sigWords.append(w);
    }

    if (sigWords.isEmpty()) return 0.0;

    int hits = 0;
    for (const QString &w : sigWords)
        if (cWords.contains(w)) ++hits;

    const double coverage = double(hits) / sigWords.size();

    // Если не все значимые слова совпали — низкий score
    if (hits < sigWords.size()) return coverage * 0.35;

    // Все слова совпали — штраф за лишние слова у кандидата
    const int extra = qMax(0, cWords.size() - qWords.size());
    return 1.0 / (1.0 + extra * 0.3);
}

// Возвращает топ maxCount кандидатов среди игр (type=="game"),
// отсортированных по убыванию схожести с запросом.
// Имена — оригинальные (не нормализованные), для показа пользователю.
QList<SteamAppList::Candidate> SteamAppList::findTopCandidates(
        const QString &normalizedTitle, int maxCount) const
{
    if (!m_loaded) return {};

    const QString query = normalizeKey(normalizedTitle);
    QList<Candidate> result;
    result.reserve(512);

    // Итерируем по nameToId — ключ уже нормализован, value = appid
    for (auto it = m_nameToId.cbegin(); it != m_nameToId.cend(); ++it) {
        const double score = titleSimilarity(query, it.key());
        if (score < 0.15) continue;

        const int appid = it.value();
        const QString originalName = m_appName.value(appid, it.key());
        result.append({ appid, originalName, score });
    }

    std::sort(result.begin(), result.end(),
              [](const Candidate &a, const Candidate &b){ return a.score > b.score; });

    if (result.size() > maxCount)
        result.resize(maxCount);

    return result;
}

QString SteamAppList::findAppId(const QString &normalizedTitle) const
{
    if (!m_loaded) return {};

    const QString key = normalizeKey(normalizedTitle);

    auto it = m_nameToId.find(key);
    if (it != m_nameToId.end()) {
        qDebug() << "SteamAppList: exact match" << normalizedTitle << "->" << it.value();
        return QString::number(it.value());
    }

    if (key.length() < 4) return {};

    int bestId  = -1;
    int bestLen = INT_MAX;
    for (auto jt = m_nameToId.cbegin(); jt != m_nameToId.cend(); ++jt) {
        const QString &candidate = jt.key();
        if (candidate.contains(key) && candidate.length() < bestLen) {
            bestLen = candidate.length();
            bestId  = jt.value();
        }
    }

    if (bestId > 0) {
        qDebug() << "SteamAppList: partial match" << normalizedTitle << "->" << bestId;
        return QString::number(bestId);
    }

    qDebug() << "SteamAppList: no match for" << normalizedTitle;
    return {};
}

SteamAppList::LocalAppInfo SteamAppList::findAppInfo(const QString &normalizedTitle) const
{
    if (!m_loaded) return {};

    const QString idStr = findAppId(normalizedTitle);
    if (idStr.isEmpty()) return {};

    return m_appInfo.value(idStr.toInt());
}

SteamAppList::LocalAppInfo SteamAppList::appInfoById(int appid) const
{
    return m_appInfo.value(appid);
}

QString SteamAppList::nameById(int appid) const
{
    return m_appName.value(appid);
}
