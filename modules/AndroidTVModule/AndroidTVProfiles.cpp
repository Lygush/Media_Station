// modules/AndroidTVModule/AndroidTVProfiles.cpp

#include "AndroidTVModule.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>
#include <QDebug>

// ══════════════════════════════════════════════════════════════════════════════
// AndroidTVProfileManager
// ══════════════════════════════════════════════════════════════════════════════

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

QVariantMap AndroidTVProfileManager::getProfile(const QString &id) const
{
    for (const auto &p : m_profiles) {
        if (p.id == id) {
            QVariantMap map;
            map["id"] = p.id;
            map["name"] = p.name;
            map["avdName"] = p.avdName;
            map["systemImagePath"] = p.systemImagePath;
            map["resolution"] = p.resolution;
            map["gpu"] = p.gpu;
            map["ramMb"] = p.ramMb;
            map["cores"] = p.cores;
            map["writableSystem"] = p.writableSystem;
            map["noBootAnim"] = p.noBootAnim;
            map["noSnapshot"] = p.noSnapshot;
            map["noSnapshotSave"] = p.noSnapshotSave;
            map["dnsServer"] = p.dnsServer;
            map["audioDriver"] = p.audioDriver;
            map["audioTimerMs"] = p.audioTimerMs;
            map["dsoundLatencyMs"] = p.dsoundLatencyMs;
            return map;
        }
    }
    return {};
}

// ── Reactive dictionary of the current profile ───────────────────────────
QVariantMap AndroidTVProfileManager::currentProfileData() const
{
    const AndroidTVProfile *p = activeProfile();
    if (!p) return {};

    QVariantMap map;
    map["id"]                   = p->id;
    map["name"]                 = p->name;
    map["avdName"]              = p->avdName;
    map["systemImagePath"]      = p->systemImagePath;
    map["displayName"]          = imageDisplayName(p->id);
    map["resolution"]           = p->resolution;
    map["gpu"]                  = p->gpu;
    map["ramMb"]                = p->ramMb;
    map["cores"]                = p->cores;
    map["writableSystem"]       = p->writableSystem;
    map["noBootAnim"]           = p->noBootAnim;
    map["noSnapshot"]           = p->noSnapshot;
    map["noSnapshotSave"]       = p->noSnapshotSave;
    map["dnsServer"]            = p->dnsServer;
    map["audioDriver"]          = p->audioDriver;
    map["audioTimerMs"]         = p->audioTimerMs;
    map["dsoundLatencyMs"]      = p->dsoundLatencyMs;
    return map;
}

bool AndroidTVProfileManager::setActiveProfile(const QString &id)
{
    if (id == m_activeProfileId) return true;
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
    int row = std::distance(m_profiles.begin(), it);
    beginRemoveRows(QModelIndex(), row, row);
    m_profiles.erase(it);
    endRemoveRows();
    save();
    emit countChanged();
    emit profilesChanged();
    return true;
}

QString AndroidTVProfileManager::addProfile(const QString &systemImagePath, const QString &baseName)
{
    AndroidTVProfile profile;
    profile.id = generateId();
    // [7] Используем baseName если задан, иначе генерируем из пути
    if (!baseName.isEmpty()) {
        profile.name = baseName;
    } else {
        // Берём последнюю папку пути как имя профиля
        QStringList parts = systemImagePath.split('/', Qt::SkipEmptyParts);
        // Ищем «говорящую» часть: android-XX
        QString apiPart;
        for (const QString &part : parts) {
            if (part.startsWith("android-", Qt::CaseInsensitive)) { apiPart = part; break; }
        }
        profile.name = apiPart.isEmpty() ? "New Profile" : "Profile " + apiPart;
    }
    QString avdBase = profile.name.simplified().replace(' ', '_');
    profile.avdName = avdBase.isEmpty() ? "CustomAVD" : avdBase;
    profile.systemImagePath = systemImagePath;
    profile.resolution = "1920x1080";
    profile.gpu = "auto";
    profile.ramMb = 2048;
    profile.cores = 4;
    profile.writableSystem = false;
    profile.noBootAnim = true;
    profile.noSnapshot = false;
    profile.noSnapshotSave = true;
    profile.dnsServer = "8.8.8.8,1.1.1.1";
    profile.audioDriver = "dsound";
    profile.audioTimerMs = QVariant("auto");
    profile.dsoundLatencyMs = QVariant("auto");

    int row = m_profiles.size();
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
    QString base = m_baseDir + "/tools/android-sdk/system-images";
    QDir dir(base);
    if (!dir.exists()) return found;
    QStringList apiDirs = dir.entryList(QStringList() << "android-*", QDir::Dirs);
    for (const QString &apiDir : apiDirs) {
        QDir apiPath(base + "/" + apiDir);
        QStringList platformDirs = apiPath.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &platform : platformDirs) {
            QDir platformPath(apiPath.path() + "/" + platform);
            QStringList abiDirs = platformPath.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &abi : abiDirs) {
                QString imgDir = platformPath.path() + "/" + abi;
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
            QString qcow2Path = m_baseDir + "/tools/android-tv/avd/" + p.avdName + ".avd/userdata-qemu.img.qcow2";
            QFileInfo fi(qcow2Path);
            if (fi.exists())
                return fi.size() / 1024 / 1024;
            return -1;
        }
    }
    return -1;
}

bool AndroidTVProfileManager::clearProfileData(const QString &id)
{
    for (const auto &p : m_profiles) {
        if (p.id == id) {
            QString avdDir = m_baseDir + "/tools/android-tv/avd/" + p.avdName + ".avd";
            QDir d(avdDir);
            if (!d.exists()) return false;
            QStringList filters = {"userdata*.img", "userdata*.img.qcow2"};
            QStringList files = d.entryList(filters, QDir::Files);
            bool ok = true;
            for (const QString &f : files) {
                if (!d.remove(f)) ok = false;
            }
            QDir snapDir(avdDir + "/snapshots/default_boot");
            if (snapDir.exists()) snapDir.removeRecursively();
            return ok;
        }
    }
    return false;
}

const AndroidTVProfile* AndroidTVProfileManager::activeProfile() const
{
    for (const auto &p : m_profiles) {
        if (p.id == m_activeProfileId) return &p;
    }
    return nullptr;
}

// ── Field update (without immediate save) ────────────────────────────────
bool AndroidTVProfileManager::updateProfileField(const QString &id, const QString &field, const QVariant &value)
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == id) {
            AndroidTVProfile &p = m_profiles[i];
            if (field == "resolution") p.resolution = value.toString();
            else if (field == "gpu") p.gpu = value.toString();
            else if (field == "ramMb") p.ramMb = value.toInt();
            else if (field == "cores") p.cores = value.toInt();
            else if (field == "writableSystem") p.writableSystem = value.toBool();
            else if (field == "noBootAnim") p.noBootAnim = value.toBool();
            else if (field == "noSnapshot") p.noSnapshot = value.toBool();
            else if (field == "noSnapshotSave") p.noSnapshotSave = value.toBool();
            else if (field == "dnsServer") p.dnsServer = value.toString();
            else if (field == "audioDriver") p.audioDriver = value.toString();
            else if (field == "audioTimerMs") {
                if (value.type() == QVariant::String && value.toString() == "auto")
                    p.audioTimerMs = QVariant("auto");
                else if (value.canConvert<int>())
                    p.audioTimerMs = value.toInt();
                else return false;
            }
            else if (field == "dsoundLatencyMs") {
                if (value.type() == QVariant::String && value.toString() == "auto")
                    p.dsoundLatencyMs = QVariant("auto");
                else if (value.canConvert<int>())
                    p.dsoundLatencyMs = value.toInt();
                else return false;
            }
            else return false;

            QModelIndex idx = index(i);
            emit dataChanged(idx, idx, {});
            emit profilesChanged();
            emit currentProfileDataChanged();   // reactivity for QML
            save();   // [1] сохраняем сразу при любом изменении настроек
            return true;
        }
    }
    return false;
}

// ── imageDisplayName (friendly name for QML) ──────────────────────────────
QString AndroidTVProfileManager::imageDisplayName(const QString &id) const
{
    for (const auto &p : m_profiles) {
        if (p.id == id) {
            QString path = p.systemImagePath;
            if (path.contains("android-tv", Qt::CaseInsensitive))
                return "Android TV";
            if (path.contains("google-tv", Qt::CaseInsensitive))
                return "Google TV";
            QStringList parts = path.split('/', Qt::SkipEmptyParts);
            if (!parts.isEmpty())
                return parts.last();
            return path;
        }
    }
    return {};
}

// ── Loading profiles from JSON ────────────────────────────────────────────
void AndroidTVProfileManager::load(const QString &baseDir)
{
    m_baseDir = baseDir;
    QString path = profilesFilePath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        // create default profile
        AndroidTVProfile def;
        def.id = "default";
        def.name = "MediaStation TV (Android 14)";
        def.avdName = "MediaStation_TV";
        def.systemImagePath = "tools/android-sdk/system-images/android-34/android-tv/x86";
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

        beginInsertRows(QModelIndex(), 0, 0);
        m_profiles.append(def);
        endInsertRows();
        m_activeProfileId = def.id;
        save();
        emit currentProfileDataChanged();
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return;

    QJsonObject root = doc.object();
    QJsonArray arr = root["profiles"].toArray();
    beginResetModel();
    m_profiles.clear();
    for (const QJsonValue &val : arr) {
        QJsonObject obj = val.toObject();
        AndroidTVProfile p;
        p.id = obj["id"].toString();
        p.name = obj["name"].toString();
        p.avdName = obj["avdName"].toString();
        p.systemImagePath = obj["systemImagePath"].toString();
        p.resolution = obj["resolution"].toString("1920x1080");
        p.gpu = obj["gpu"].toString("auto");
        p.ramMb = obj["ramMb"].toInt(2048);
        p.cores = obj["cores"].toInt(4);
        p.writableSystem = obj["writableSystem"].toBool(false);
        p.noBootAnim = obj["noBootAnim"].toBool(true);
        p.noSnapshot = obj["noSnapshot"].toBool(false);
        p.noSnapshotSave = obj["noSnapshotSave"].toBool(true);
        p.dnsServer = obj["dnsServer"].toString("8.8.8.8,1.1.1.1");
        p.audioDriver = obj["audioDriver"].toString("dsound");

        // Audio timer: backward-compatible parsing
        if (obj.contains("useDefaultAudioTimer")) {
            if (obj["useDefaultAudioTimer"].toBool(true))
                p.audioTimerMs = QVariant("auto");
            else
                p.audioTimerMs = obj["audioTimerMs"].toInt(100);
        } else {
            QJsonValue av = obj["audioTimerMs"];
            if (av.isString() && av.toString() == "auto")
                p.audioTimerMs = QVariant("auto");
            else if (av.isDouble())
                p.audioTimerMs = av.toInt();
            else
                p.audioTimerMs = QVariant("auto");
        }

        // Dsound latency: backward-compatible parsing
        if (obj.contains("useDefaultDsoundLatency")) {
            if (obj["useDefaultDsoundLatency"].toBool(true))
                p.dsoundLatencyMs = QVariant("auto");
            else
                p.dsoundLatencyMs = obj["dsoundLatencyMs"].toInt(200);
        } else {
            QJsonValue dv = obj["dsoundLatencyMs"];
            if (dv.isString() && dv.toString() == "auto")
                p.dsoundLatencyMs = QVariant("auto");
            else if (dv.isDouble())
                p.dsoundLatencyMs = dv.toInt();
            else
                p.dsoundLatencyMs = QVariant("auto");
        }

        m_profiles.append(p);
    }
    endResetModel();
    m_activeProfileId = root["activeProfileId"].toString();
    if (m_activeProfileId.isEmpty() && !m_profiles.isEmpty())
        m_activeProfileId = m_profiles.first().id;
    emit currentProfileDataChanged();
}

// ── Save (public method) ──────────────────────────────────────────────────
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

    QString path = profilesFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson());
        f.close();
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
