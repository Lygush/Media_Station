// modules/AndroidTVModule/AdGuardHome.h
// ─────────────────────────────────────────────────────────────────────────────
// Блокировка рекламы в Android TV через AdGuard Home (аудит §23) вместо
// dnsproxy, который эмулятором не использовался.
//
//   tools/adguardhome/AdGuardHome.exe   — компонент (кладётся установщиком или
//                                          setup_android_sdk.bat)
//   tools/adguardhome/AdGuardHome.yaml  — пишет MediaStation (writeDefaultConfig):
//        DNS только 127.0.0.1:53, панель только 127.0.0.1:3080, DoH-серверы,
//        локальный список userfilters/mediastation.txt + AdGuard DNS filter,
//        белый список YouTube (агрессивные списки ломают видео).
//
// AdGuard Home работает **службой Windows** (ставится один раз, с запросом
// прав администратора). MediaStation её не запускает и не останавливает:
// перед запуском эмулятора проверяет, что 127.0.0.1:53 отвечает на DNS, и
// тогда передаёт эмулятору `-dns-server 127.0.0.1`. Не отвечает — DNS из
// профиля, как раньше. Весь Windows на 127.0.0.1 не перенаправляется.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

class AdGuardHome : public QObject
{
    Q_OBJECT
public:
    explicit AdGuardHome(const QString &appDir, QObject *parent = nullptr);

    static constexpr auto kServiceName = "AdGuardHome";
    static constexpr auto kDnsHost     = "127.0.0.1";
    static constexpr int  kDnsPort     = 53;
    static constexpr int  kWebPort     = 3080;

    QString dir()        const;
    QString exePath()    const;
    QString configPath() const;
    QString userFilterPath() const;
    bool    isInstalled() const;     // exe на месте

    enum class ServiceState { Missing, NotInstalled, Stopped, Starting, Running, Unknown };
    ServiceState serviceState() const;
    static QString stateName(ServiceState s);

    /// Конфигурация по умолчанию (если её ещё нет). blocklist — старый
    /// tools/android-tv/blocklists/default.txt: домены переносятся в локальный
    /// список фильтров. false — не удалось записать (*error).
    bool writeDefaultConfig(const QString &blocklist, QString *error = nullptr) const;
    static QString defaultConfigText(const QString &dir);
    /// Строки hosts / «домен в строке» → правила AdGuard (||домен^)
    static QStringList blocklistToRules(const QString &text);

    /// Скачать AdGuard Home (последний релиз с GitHub) и распаковать в
    /// tools/adguardhome — встроенным в Windows 10/11 tar.exe.
    /// progress(0..1) во время загрузки; done(ok, сообщение).
    static constexpr auto kDownloadUrl =
        "https://github.com/AdguardTeam/AdGuardHome/releases/latest/download/AdGuardHome_windows_amd64.zip";
    void download(std::function<void(qreal)> progress, std::function<void(bool, const QString &)> done);

    /// Поставить / удалить службу. Запрашивает права администратора (UAC);
    /// done(ok, сообщение) — после завершения AdGuardHome.exe.
    void installService(std::function<void(bool, const QString &)> done);
    void uninstallService(std::function<void(bool, const QString &)> done);

    /// Почему DNS не отвечает: ответ панели (/control/status) и кто занял
    /// порт 53 (netstat + tasklist). Для журнала.
    void diagnose(std::function<void(const QString &report)> done);
    /// Строки netstat -ano → PID, слушающие UDP/TCP порт port на 127.0.0.1/0.0.0.0
    static QList<int> pidsOnPort(const QString &netstat, int port);

    /// Отвечает ли DNS-сервер (любой ответ на запрос A — значит работает)
    static void probeDns(const QString &host, quint16 port, int timeoutMs, QObject *context,
                         std::function<void(bool ok, int ms)> done);

    // ── DNS — открыто для тестов ─────────────────────────────────────────────
    static QByteArray buildQuery(quint16 id, const QString &name);
    static bool isResponseTo(const QByteArray &packet, quint16 id);

private:
    void runElevated(const QStringList &args, std::function<void(bool, const QString &)> done);

    QString m_appDir;
    class QNetworkAccessManager *m_nam = nullptr;
};
