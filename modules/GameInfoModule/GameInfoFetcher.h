// modules/GameInfoModule/GameInfoFetcher.h
#pragma once

#include <QObject>
#include <QString>

class GameInfoFetcher : public QObject
{
    Q_OBJECT
public:
    explicit GameInfoFetcher(QObject *parent = nullptr);

    struct FetchResult {
        QString sourceText;   // объединённый текст для LLM
        QString steamAppId;   // числовой ID в Steam (пустой если не найден)
    };

    // Вызывается из фонового потока.
    // Возвращает текст источников и steamAppId.
    // sourceText пустой — источники не найдены (LLM работает из своих знаний).
    // knownAppId — выбранный пользователем (или найденный точно) appId: тогда
    // и страница Steam, и поиск в Wikipedia идут по этой игре, а не по
    // собственному поиску сборщика по названию.
    FetchResult fetch(const QString &title, const QString &installFolder,
                      const QString &knownAppId = QString());

private:
    // Локальный список (первый источник — быстрее сети)
    QString fetchLocalAppInfo(const QString &normalizedTitle, QString &outAppId);

    // Основные источники
    QString fetchSteamPageById(const QString &appId, const QString &cc = "ru"); // страница по известному appId
    QString fetchWikipedia(const QString &normalizedTitle);
    QString fetchSteam(const QString &normalizedTitle, QString &outAppId); // storesearch + страница
    QString fetchSteamCommunity(const QString &appId);
    QString fetchSteamSpy(const QString &appId);

    // Фолбэки
    QString fetchWikipediaEn(const QString &normalizedTitle);
    QString fetchMetacritic(const QString &normalizedTitle, QString &outAppId);

    // Вспомогательные
    bool    verifySteamAppType(const QString &appId);   // true если type=="game"
    double  titleSimilarity(const QString &query, const QString &candidate);
    QString simplifyTitle(const QString &s);
};
