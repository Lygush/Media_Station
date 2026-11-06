// modules/GameMediaModule/SteamAppList.h
#pragma once

#include <atomic>

#include <QObject>
#include <QString>
#include <QHash>
#include <QMutex>
#include <QWaitCondition>
#include <QVariantList>

class SteamAppList : public QObject
{
    Q_OBJECT

public:
    struct LocalAppInfo {
        int     appid            = 0;
        QString shortDescription;
        double  metacriticScore  = -1.0;
        QString headerImage;
        QString background;
        QString releaseDate;
        QString type;

        bool isValid() const { return appid > 0; }
    };

    struct Candidate {
        int     appid;
        QString name;
        double  score;
    };

    static SteamAppList &instance();

    void loadIfNeeded();

    // Блокирует фоновый поток до завершения загрузки.
    void waitUntilLoaded();

    // Возвращает топ maxCount кандидатов (type=="game") отсортированных по score убыванию.
    // Вызывать только после загрузки (isLoaded() == true).
    QList<Candidate> findTopCandidates(const QString &normalizedTitle, int maxCount = 8) const;

    QString      findAppId(const QString &normalizedTitle) const;
    LocalAppInfo findAppInfo(const QString &normalizedTitle) const;
    LocalAppInfo appInfoById(int appid) const;
    QString      nameById(int appid) const;   // точное название в Steam, пусто если нет

    bool isLoaded() const { return m_loaded; }

    void cancel();

signals:
    void loaded();

private:
    explicit SteamAppList(QObject *parent = nullptr);

    void loadFromFile();

    QString localFilePath() const;
    QString normalizeKey(const QString &name) const;
    double  titleSimilarity(const QString &query, const QString &candidate) const;

    QHash<QString, int>         m_nameToId;  // normalizedKey -> appid
    QHash<int, QString>         m_appName;   // appid -> original name
    QHash<int, LocalAppInfo>    m_appInfo;

    // m_loaded и m_cancelled читаются из разных потоков без мьютекса
    // (isLoaded(), цикл разбора) — поэтому атомарные.
    std::atomic<bool> m_loaded    { false };
    bool              m_loading   = false;     // только под m_mutex
    std::atomic<bool> m_cancelled { false };

    mutable QMutex     m_mutex;
    QWaitCondition     m_loadedCond;
};
