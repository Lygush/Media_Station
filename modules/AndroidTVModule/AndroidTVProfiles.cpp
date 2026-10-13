// modules/AndroidTVModule/AndroidTVProfiles.cpp

#include "AndroidTVModule.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QDebug>

// ══════════════════════════════════════════════════════════════════════════════
// AndroidTVProfileManager
// ══════════════════════════════════════════════════════════════════════════════

namespace {
constexpr auto kDefaultImagePath = "tools/android-sdk/system-images/android-34/android-tv/x86";

bool isAutoValue(const QVariant &v)
{
    return v.typeId() == QMetaType::QString && v.toString() == QLatin1String("auto");
}
} // namespace

AndroidTVProfileManager::AndroidTVProfileManager(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AndroidTVProfileManager::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_profiles.size();
}

QVariant AndroidTVProfileManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_profiles.size())
        return QVariant();

    const AndroidTVProfile &p = m_profiles.at(index.row());
    switch (role) {
    case ProfileIdRole: return p.id;
    case NameRole: return p.name;
    case AvdNameRole: return p.avdName;
    case SystemImagePathRole: return p.systemImagePath;
    case ResolutionRole: return p.resolution;
    case GpuRole: return p.gpu;
    case RamMbRole: return p.ramMb;
    case CoresRole: return p.cores;
    case WritableSystemRole: return p.writableSystem;
    case NoBootAnimRole: return p.noBootAnim;
    case NoSnapshotRole: return p.noSnapshot;
    case NoSnapshotSaveRole: return p.noSnapshotSave;
    case DnsServerRole: return p.dnsServer;
    case AudioDriverRole: return p.audioDriver;
    case AudioTimerMsRole: return p.audioTimerMs;
    case DsoundLatencyMsRole: return p.dsoundLatencyMs;
    case SizeMbRole: return profileSizeMb(p.id);
    default: return QVariant();
    }
}

QHash<int, QByteArray> AndroidTVProfileManager::roleNames() const
{
    return {
        {ProfileIdRole, "profileId"},
        {NameRole, "name"},
        {AvdNameRole, "avdName"},
        {SystemImagePathRole, "systemImagePath"},
        {ResolutionRole, "resolution"},
        {GpuRole, "gpu"},
        {RamMbRole, "ramMb"},
        {CoresRole, "cores"},
        {WritableSystemRole, "writableSystem"},
        {NoBootAnimRole, "noBootAnim"},
        {NoSnapshotRole, "noSnapshot"},
        {NoSnapshotSaveRole, "noSnapshotSave"},
        {DnsServerRole, "dnsServer"},
        {AudioDriverRole, "audioDriver"},
        {AudioTimerMsRole, "audioTimerMs"},
        {DsoundLatencyMsRole, "dsoundLatencyMs"},
        {SizeMbRole, "sizeMb"}
    };
}

QVariantMap AndroidTVProfileManager::profileToMap(const AndroidTVProfile &p) const
{
    QVariantMap map;
    map["id"]              = p.id;
    map["name"]            = p.name;
    map["avdName"]         = p.avdName;
    map["systemImagePath"] = p.systemImagePath;
    map["displayName"]     = imageDisplayName(p.id);   // раньше отсутствовало → в UI пустая строка
    map["imageExists"]     = QFile::exists(systemImageDir(p) + "/system.img");
    map["resolution"]      = p.resolution;
    map["gpu"]             = p.gpu;
    map["ramMb"]           = p.ramMb;
    map["cores"]           = p.cores;
    map["writableSystem"]  = p.writableSystem;
    map["noBootAnim"]      = p.noBootAnim;
    map["noSnapshot"]      = p.noSnapshot;
    map["noSnapshotSave"]  = p.noSnapshotSave;
    map["dnsServer"]       = p.dnsServer;
    map["audioDriver"]     = p.audioDriver;
    map["audioTimerMs"]    = p.audioTimerMs;
    map["dsoundLatencyMs"] = p.dsoundLatencyMs;
    return map;
}

QVariantMap AndroidTVProfileManager::getProfile(const QString &id) const
{
    for (const auto &p : m_profiles)
        if (p.id == id) return profileToMap(p);
    return {};
}

// ── Reactive dictionary of the current profile ───────────────────────────
QVariantMap AndroidTVProfileManager::currentProfileData() const
{
    const AndroidTVProfile *p = activeProfile();
    return p ? profileToMap(*p) : QVariantMap{};
}

void AndroidTVProfileManager::setLocked(bool locked)
{
    if (m_locked == locked) return;
    m_locked = locked;
    emit lockedChanged();
}

bool AndroidTVProfileManager::setActiveProfile(const QString &id)
{
    if (id == m_activeProfileId) return true;
    if (m_locked) {
        qWarning() << "[AndroidTV] setActiveProfile: emulator is running, profile switch refused";
        return false;
    }
    auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                           [&](const AndroidTVProfile &p){ return p.id == id; });
    if (it == m_profiles.end()) return false;
    m_activeProfileId = id;
    save();
    emit activeProfileIdChanged();
    emit profilesChanged();
    emit currentProfileDataChanged();
    return true;
}

bool AndroidTVProfileManager::removeProfile(const QString &id)
{
    if (id == m_activeProfileId) return false;
    auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                           [&](const AndroidTVProfile &p){ return p.id == id; });
    if (it == m_profiles.end()) return false;
    const int row = static_cast<int>(std::distance(m_profiles.begin(), it));
    beginRemoveRows(QModelIndex(), row, row);
    m_profiles.erase(it);
    endRemoveRows();
    save();
    emit countChanged();
    emit profilesChanged();
    return true;
}

// Имя AVD — только ASCII [A-Za-z0-9._-] (эмулятор не принимает кириллицу и
// пробелы) и уникальное среди профилей (иначе два профиля делили бы один диск).
QString AndroidTVProfileManager::makeUniqueAvdName(const QString &base) const
{
    QString clean;
    for (const QChar ch : base.simplified()) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')
            clean += ch;
        else if (ch.isSpace())
            clean += '_';
    }
    while (clean.startsWith('.') || clean.startsWith('_') || clean.startsWith('-'))
        clean.remove(0, 1);
    if (clean.isEmpty()) clean = QStringLiteral("MediaStation_TV");

    auto taken = [this](const QString &name) {
        for (const auto &p : m_profiles)
            if (p.avdName.compare(name, Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    QString candidate = clean;
    for (int n = 2; taken(candidate); ++n)
        candidate = clean + "_" + QString::number(n);
    return candidate;
}

// Путь из QML может быть: относительным путём из scanSystemImages(), абсолютным
// путём к папке, или (из FileDialog) путём/URL к файлу system.img.
// Храним путь к ПАПКЕ образа, относительный appDir, если он внутри приложения.
QString AndroidTVProfileManager::normalizeImagePath(const QString &path) const
{
    QString p = path.trimmed();
    if (p.startsWith(QLatin1String("file:"))) p = QUrl(p).toLocalFile();
    p = QDir::fromNativeSeparators(p);

    QString abs = QDir::isAbsolutePath(p) ? p : m_baseDir + "/" + p;
    QFileInfo fi(abs);
    if (fi.isFile()) abs = fi.absolutePath();    // выбрали system.img → папка образа
    abs = QDir::cleanPath(abs);

    const QString rel = QDir(m_baseDir).relativeFilePath(abs);
    if (!rel.startsWith(QLatin1String("..")) && !QDir::isAbsolutePath(rel))
        return rel;
    return abs;
}

QString AndroidTVProfileManager::systemImageDir(const AndroidTVProfile &p) const
{
    const QString path = QDir::fromNativeSeparators(p.systemImagePath);
    return QDir::cleanPath(QDir::isAbsolutePath(path) ? path : m_baseDir + "/" + path);
}

QString AndroidTVProfileManager::avdHome() const
{
    return m_avdHome.isEmpty() ? m_baseDir + "/tools/android-tv/avd" : m_avdHome;
}

QString AndroidTVProfileManager::addProfile(const QString &systemImagePath, const QString &baseName)
{
    AndroidTVProfile profile = makeDefaultProfile();
    profile.id = generateId();
    profile.systemImagePath = normalizeImagePath(systemImagePath);

    if (!baseName.trimmed().isEmpty()) {
        profile.name = baseName.trimmed();
    } else {
        // «Говорящая» часть пути: android-XX
        const QStringList parts = profile.systemImagePath.split('/', Qt::SkipEmptyParts);
        QString apiPart;
        for (const QString &part : parts)
            if (part.startsWith("android-", Qt::CaseInsensitive) && part.size() > 8 && part.at(8).isDigit()) {
                apiPart = part; break;
            }
        profile.name = apiPart.isEmpty() ? QStringLiteral("New Profile") : "Profile " + apiPart;
    }
    profile.avdName = makeUniqueAvdName(profile.name);

    const int row = m_profiles.size();
    beginInsertRows(QModelIndex(), row, row);
    m_profiles.append(profile);
    endInsertRows();
    save();
    emit countChanged();
    emit profilesChanged();
    return profile.id;
}

QStringList AndroidTVProfileManager::scanSystemImages() const
{
    QStringList found;
    const QString base = m_baseDir + "/tools/android-sdk/system-images";
    QDir dir(base);
    if (!dir.exists()) return found;
    const QStringList apiDirs = dir.entryList(QStringList() << "android-*", QDir::Dirs);
    for (const QString &apiDir : apiDirs) {
        QDir apiPath(base + "/" + apiDir);
        const QStringList platformDirs = apiPath.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &platform : platformDirs) {
            QDir platformPath(apiPath.path() + "/" + platform);
            const QStringList abiDirs = platformPath.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &abi : abiDirs) {
                const QString imgDir = platformPath.path() + "/" + abi;
                if (QFile::exists(imgDir + "/system.img")) {
                    QString rel = QDir(m_baseDir).relativeFilePath(imgDir);
                    found << rel.replace('\\', '/');
                }
            }
        }
    }
    return found;
}

qint64 AndroidTVProfileManager::profileSizeMb(const QString &id) const
{
    for (const auto &p : m_profiles) {
        if (p.id == id) {
            QFileInfo fi(avdHome() + "/" + p.avdName + ".avd/userdata-qemu.img.qcow2");
            return fi.exists() ? fi.size() / 1024 / 1024 : -1;
        }
    }
    return -1;
}

bool AndroidTVProfileManager::clearProfileData(const QString &id)
{
    if (m_locked && id == m_activeProfileId) {
        qWarning() << "[AndroidTV] clearProfileData: emulator is running";
        return false;
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        const AndroidTVProfile &p = m_profiles.at(i);
        if (p.id != id) continue;

        const QString avdDir = avdHome() + "/" + p.avdName + ".avd";
        QDir d(avdDir);
        if (!d.exists()) return false;
        const QStringList files = d.entryList({"userdata*.img", "userdata*.img.qcow2"}, QDir::Files);
        bool ok = true;
        for (const QString &f : files)
            if (!d.remove(f)) ok = false;
        QDir snapDir(avdDir + "/snapshots/default_boot");
        if (snapDir.exists()) snapDir.removeRecursively();

        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx, {SizeMbRole});
        emit profilesChanged();
        return ok;
    }
    return false;
}

const AndroidTVProfile* AndroidTVProfileManager::activeProfile() const
{
    for (const auto &p : m_profiles)
        if (p.id == m_activeProfileId) return &p;
    return nullptr;
}

// ── Field update ─────────────────────────────────────────────────────────────
bool AndroidTVProfileManager::updateProfileField(const QString &id, const QString &field, const QVariant &value)
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id != id) continue;

        AndroidTVProfile &p = m_profiles[i];
        if (field == "name") {
            const QString n = value.toString().trimmed();
            if (n.isEmpty()) return false;
            p.name = n;          // avdName не меняем — к нему привязаны данные
        }
        else if (field == "resolution") p.resolution = value.toString().trimmed();
        else if (field == "gpu")        p.gpu = value.toString();
        else if (field == "ramMb")      p.ramMb = qBound(512, value.toInt(), 16384);
        else if (field == "cores")      p.cores = qBound(1, value.toInt(), 16);
        else if (field == "writableSystem") p.writableSystem = value.toBool();
        else if (field == "noBootAnim")     p.noBootAnim = value.toBool();
        else if (field == "noSnapshot")     p.noSnapshot = value.toBool();
        else if (field == "noSnapshotSave") p.noSnapshotSave = value.toBool();
        else if (field == "dnsServer")      p.dnsServer = value.toString();
        else if (field == "audioDriver")    p.audioDriver = value.toString();
        else if (field == "audioTimerMs" || field == "dsoundLatencyMs") {
            QVariant &target = (field == "audioTimerMs") ? p.audioTimerMs : p.dsoundLatencyMs;
            if (isAutoValue(value)) {
                target = QVariant(QStringLiteral("auto"));
            } else {
                bool ok = false;
                const int v = value.toInt(&ok);
                if (!ok || v <= 0) return false;
                target = v;
            }
        }
        else return false;

        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx, {});
        emit profilesChanged();
        if (id == m_activeProfileId) emit currentProfileDataChanged();
        save();
        return true;
    }
    return false;
}

// ── imageDisplayName (friendly name for QML) ──────────────────────────────
QString AndroidTVProfileManager::imageDisplayName(const QString &id) const
{
    for (const auto &p : m_profiles) {
        if (p.id != id) continue;
        const QString path = QDir::fromNativeSeparators(p.systemImagePath);
        const QStringList parts = path.split('/', Qt::SkipEmptyParts);

        QString api;
        for (const QString &part : parts)
            if (part.startsWith("android-", Qt::CaseInsensitive) && part.size() > 8 && part.at(8).isDigit())
                api = part.mid(8);

        QString kind;
        if (path.contains("google_atv", Qt::CaseInsensitive) || path.contains("google-tv", Qt::CaseInsensitive))
            kind = "Google TV";
        else if (path.contains("android-tv", Qt::CaseInsensitive))
            kind = "Android TV";

        if (!kind.isEmpty())
            return api.isEmpty() ? kind : QString("%1 (API %2)").arg(kind, api);
        return parts.isEmpty() ? path : parts.last();
    }
    return {};
}

AndroidTVProfile AndroidTVProfileManager::makeDefaultProfile() const
{
    AndroidTVProfile def;
    def.id = "default";
    def.name = "MediaStation TV";
    def.avdName = "MediaStation_TV";
    def.systemImagePath = QString::fromLatin1(kDefaultImagePath);
    def.resolution = "1920x1080";
    def.gpu = "auto";
    def.ramMb = 2048;
    def.cores = 4;
    def.writableSystem = false;
    def.noBootAnim = true;
    def.noSnapshot = false;
    def.noSnapshotSave = true;
    def.dnsServer = "8.8.8.8,1.1.1.1";
    def.audioDriver = "dsound";
    def.audioTimerMs = QVariant("auto");
    def.dsoundLatencyMs = QVariant("auto");
    return def;
}

// ── Loading profiles from JSON ────────────────────────────────────────────
void AndroidTVProfileManager::load(const QString &baseDir)
{
    m_baseDir = baseDir;
    const QString path = profilesFilePath();

    QList<AndroidTVProfile> loaded;
    QString activeId;

    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        f.close();
        if (!doc.isObject()) {
            qWarning() << "[AndroidTV] profiles.json is corrupted:" << err.errorString()
                       << "— recreating default profile";
            QFile::rename(path, path + ".bak");
        } else {
            const QJsonObject root = doc.object();
            activeId = root["activeProfileId"].toString();
            const QJsonArray arr = root["profiles"].toArray();
            for (const QJsonValue &val : arr) {
                const QJsonObject obj = val.toObject();
                AndroidTVProfile p;
                p.id = obj["id"].toString();
                if (p.id.isEmpty()) p.id = generateId();
                p.name = obj["name"].toString("Profile");
                p.avdName = obj["avdName"].toString();
                p.systemImagePath = obj["systemImagePath"].toString(QString::fromLatin1(kDefaultImagePath));
                p.resolution = obj["resolution"].toString("auto");
                p.gpu = obj["gpu"].toString("auto");
                p.ramMb = obj["ramMb"].toInt(2048);
                p.cores = obj["cores"].toInt(4);
                p.writableSystem = obj["writableSystem"].toBool(false);
                p.noBootAnim = obj["noBootAnim"].toBool(true);
                p.noSnapshot = obj["noSnapshot"].toBool(false);
                p.noSnapshotSave = obj["noSnapshotSave"].toBool(true);
                p.dnsServer = obj["dnsServer"].toString("8.8.8.8,1.1.1.1");
                p.audioDriver = obj["audioDriver"].toString("dsound");

                // Audio timer: обратная совместимость со старым форматом
                if (obj.contains("useDefaultAudioTimer")) {
                    p.audioTimerMs = obj["useDefaultAudioTimer"].toBool(true)
                                         ? QVariant("auto") : QVariant(obj["audioTimerMs"].toInt(100));
                } else {
                    const QJsonValue av = obj["audioTimerMs"];
                    p.audioTimerMs = av.isDouble() ? QVariant(av.toInt()) : QVariant("auto");
                }
                if (obj.contains("useDefaultDsoundLatency")) {
                    p.dsoundLatencyMs = obj["useDefaultDsoundLatency"].toBool(true)
                                            ? QVariant("auto") : QVariant(obj["dsoundLatencyMs"].toInt(200));
                } else {
                    const QJsonValue dv = obj["dsoundLatencyMs"];
                    p.dsoundLatencyMs = dv.isDouble() ? QVariant(dv.toInt()) : QVariant("auto");
                }
                loaded.append(p);
            }
        }
    }

    beginResetModel();
    m_profiles.clear();
    // avdName: пустые / недопустимые / дублирующиеся исправляем
    for (AndroidTVProfile p : loaded) {
        const QString wanted = p.avdName.isEmpty() ? p.name : p.avdName;
        p.avdName = makeUniqueAvdName(wanted);
        m_profiles.append(p);
    }
    bool changed = false;
    if (m_profiles.isEmpty()) {
        AndroidTVProfile def = makeDefaultProfile();
        // Если стандартного образа нет — берём первый найденный
        if (!QFile::exists(systemImageDir(def) + "/system.img")) {
            const QStringList images = scanSystemImages();
            for (const QString &img : images)
                if (img.contains("tv", Qt::CaseInsensitive)) { def.systemImagePath = img; break; }
            if (!QFile::exists(systemImageDir(def) + "/system.img") && !images.isEmpty())
                def.systemImagePath = images.first();
        }
        m_profiles.append(def);
        changed = true;
    }
    endResetModel();

    m_activeProfileId = activeId;
    if (!activeProfile()) {
        m_activeProfileId = m_profiles.first().id;
        changed = true;
    }
    if (changed) save();

    emit countChanged();
    emit activeProfileIdChanged();
    emit profilesChanged();
    emit currentProfileDataChanged();
}

// ── Save ──────────────────────────────────────────────────────────────────
void AndroidTVProfileManager::save()
{
    QJsonObject root;
    QJsonArray arr;
    for (const auto &p : m_profiles) {
        QJsonObject obj;
        obj["id"] = p.id;
        obj["name"] = p.name;
        obj["avdName"] = p.avdName;
        obj["systemImagePath"] = p.systemImagePath;
        obj["resolution"] = p.resolution;
        obj["gpu"] = p.gpu;
        obj["ramMb"] = p.ramMb;
        obj["cores"] = p.cores;
        obj["writableSystem"] = p.writableSystem;
        obj["noBootAnim"] = p.noBootAnim;
        obj["noSnapshot"] = p.noSnapshot;
        obj["noSnapshotSave"] = p.noSnapshotSave;
        obj["dnsServer"] = p.dnsServer;
        obj["audioDriver"] = p.audioDriver;
        obj["audioTimerMs"] = QJsonValue::fromVariant(p.audioTimerMs);
        obj["dsoundLatencyMs"] = QJsonValue::fromVariant(p.dsoundLatencyMs);
        arr.append(obj);
    }
    root["profiles"] = arr;
    root["activeProfileId"] = m_activeProfileId;

    const QString path = profilesFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    // QSaveFile: при сбое во время записи старый profiles.json не теряется
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson());
        if (!f.commit())
            qWarning() << "[AndroidTV] cannot save profiles:" << f.errorString();
    } else {
        qWarning() << "[AndroidTV] cannot open profiles for writing:" << path;
    }
}

// ── Helpers ────────────────────────────────────────────────────────────────
QString AndroidTVProfileManager::generateId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString AndroidTVProfileManager::profilesFilePath() const
{
    return m_baseDir + "/data/AndroidTV/profiles.json";
}
