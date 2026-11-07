// modules/AndroidTVModule/AdGuardHome.cpp
#include "AdGuardHome.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUdpSocket>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <shellapi.h>
#  include <objbase.h>
#endif

AdGuardHome::AdGuardHome(const QString &appDir, QObject *parent)
    : QObject(parent), m_appDir(appDir)
{
}

QString AdGuardHome::dir()            const { return m_appDir + "/tools/adguardhome"; }
QString AdGuardHome::exePath()        const { return dir() + "/AdGuardHome.exe"; }
QString AdGuardHome::configPath()     const { return dir() + "/AdGuardHome.yaml"; }
QString AdGuardHome::userFilterPath() const { return dir() + "/userfilters/mediastation.txt"; }
bool    AdGuardHome::isInstalled()    const { return QFileInfo::exists(exePath()); }

QString AdGuardHome::stateName(ServiceState s)
{
    switch (s) {
    case ServiceState::Missing:      return QStringLiteral("missing");
    case ServiceState::NotInstalled: return QStringLiteral("notInstalled");
    case ServiceState::Stopped:      return QStringLiteral("stopped");
    case ServiceState::Starting:     return QStringLiteral("starting");
    case ServiceState::Running:      return QStringLiteral("running");
    case ServiceState::Unknown:      break;
    }
    return QStringLiteral("unknown");
}

AdGuardHome::ServiceState AdGuardHome::serviceState() const
{
    if (!isInstalled()) return ServiceState::Missing;
#ifdef Q_OS_WIN
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return ServiceState::Unknown;
    SC_HANDLE svc = OpenServiceW(scm, L"AdGuardHome", SERVICE_QUERY_STATUS);
    if (!svc) {
        const DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return err == ERROR_SERVICE_DOES_NOT_EXIST ? ServiceState::NotInstalled : ServiceState::Unknown;
    }
    SERVICE_STATUS_PROCESS st = {};
    DWORD needed = 0;
    ServiceState res = ServiceState::Unknown;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&st),
                             sizeof(st), &needed)) {
        switch (st.dwCurrentState) {
        case SERVICE_RUNNING:       res = ServiceState::Running;  break;
        case SERVICE_START_PENDING:
        case SERVICE_CONTINUE_PENDING: res = ServiceState::Starting; break;
        default:                    res = ServiceState::Stopped;  break;
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return res;
#else
    return ServiceState::NotInstalled;
#endif
}

// ── Конфигурация ─────────────────────────────────────────────────────────────

QStringList AdGuardHome::blocklistToRules(const QString &text)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    static const QRegularExpression domain(QStringLiteral("^[A-Za-z0-9*]([A-Za-z0-9*-]*[A-Za-z0-9])?(\\.[A-Za-z0-9-]+)+$"));
    QStringList rules;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('!')))
            continue;
        // Уже правило AdGuard/Adblock — как есть
        if (line.startsWith(QLatin1String("||")) || line.startsWith(QLatin1String("@@"))) {
            rules << line;
            continue;
        }
        // hosts: «0.0.0.0 домен» / «127.0.0.1 домен»; иначе — просто домен
        const QStringList parts = line.split(ws, Qt::SkipEmptyParts);
        QString host = parts.value(parts.size() >= 2 ? 1 : 0).toLower();
        if (host == QLatin1String("localhost") || host.startsWith(QLatin1String("ip6-"))) continue;
        if (!domain.match(host).hasMatch()) continue;
        const QString rule = "||" + host + "^";
        if (!rules.contains(rule)) rules << rule;
    }
    return rules;
}

QString AdGuardHome::defaultConfigText(const QString &dirPath)
{
    // Проверено на AdGuard Home v0.107.80: недостающие поля берутся по
    // умолчанию, schema_version обязателен (без него AdGuard Home пытается
    // «обновить» файл с версии 0 и не запускается).
    // Пути — в виде Windows (C:\...): AdGuard Home сверяет путь списка с
    // safe_fs_patterns посимвольно, а у себя пути приводит к родному виду
    const QString d = QDir::toNativeSeparators(QDir::cleanPath(dirPath));
    const QString sep = QDir::toNativeSeparators(QStringLiteral("/"));
    QString y;
    y += QStringLiteral(
        "# MediaStation: AdGuard Home только для Android TV (эмулятор получает\n"
        "# -dns-server 127.0.0.1). Файл создан MediaStation; правки сохраняются.\n"
        "# Панель: http://127.0.0.1:3080 (только с этого компьютера, без пароля).\n"
        "http:\n"
        "  address: 127.0.0.1:3080\n"
        "users: []\n"
        "dns:\n"
        "  bind_hosts:\n"
        "    - 127.0.0.1\n"
        "  port: 53\n"
        "  upstream_dns:\n"
        "    - https://dns.google/dns-query\n"
        "    - https://cloudflare-dns.com/dns-query\n"
        "    - https://common.dot.dns.yandex.net/dns-query\n"
        "  bootstrap_dns:\n"
        "    - 77.88.8.8\n"
        "    - 8.8.8.8\n"
        "    - 1.1.1.1\n"
        "  fallback_dns:\n"
        "    - 77.88.8.8\n"
        "    - 8.8.8.8\n"
        "  upstream_mode: parallel\n"
        "  cache_optimistic: true\n"
        "filters:\n"
        "  - enabled: true\n");
    y += QStringLiteral("    url: %1%2userfilters%2mediastation.txt\n").arg(d, sep);
    y += QStringLiteral(
        "    name: MediaStation (локальный)\n"
        "    id: 1\n"
        "  - enabled: true\n"
        "    url: https://adguardteam.github.io/HostlistsRegistry/assets/filter_1.txt\n"
        "    name: AdGuard DNS filter\n"
        "    id: 2\n"
        "# YouTube и Google Video не блокировать: рекламу внутри YouTube DNS всё\n"
        "# равно не убирает, а блокировка этих доменов ломает видео\n"
        "user_rules:\n"
        "  - '@@||youtube.com^'\n"
        "  - '@@||googlevideo.com^'\n"
        "  - '@@||ytimg.com^'\n"
        "  - '@@||youtubei.googleapis.com^'\n"
        "  - '@@||youtube-nocookie.com^'\n"
        "filtering:\n"
        "  safe_fs_patterns:\n");
    y += QStringLiteral("    - %1%2userfilters%2*\n").arg(d, sep);
    y += QStringLiteral(
        "querylog:\n"
        "  interval: 24h\n"
        "statistics:\n"
        "  interval: 1d\n"
        "schema_version: 34\n");
    return y;
}

bool AdGuardHome::writeDefaultConfig(const QString &blocklist, QString *error) const
{
    QDir().mkpath(dir() + "/userfilters");

    if (!QFileInfo::exists(userFilterPath())) {
        QStringList rules;
        QFile old(blocklist);
        if (old.open(QIODevice::ReadOnly | QIODevice::Text))
            rules = blocklistToRules(QString::fromUtf8(old.readAll()));
        QSaveFile f(userFilterPath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (error) *error = f.errorString();
            return false;
        }
        f.write("! MediaStation — свои правила блокировки (по одному в строке: ||домен^)\n");
        for (const QString &r : rules) f.write((r + "\n").toUtf8());
        if (!f.commit()) { if (error) *error = f.errorString(); return false; }
    }

    if (QFileInfo::exists(configPath())) return true;   // правки пользователя не трогаем
    QSaveFile f(configPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(defaultConfigText(dir()).toUtf8());
    if (!f.commit()) { if (error) *error = f.errorString(); return false; }
    return true;
}

// ── Загрузка ─────────────────────────────────────────────────────────────────

void AdGuardHome::download(std::function<void(qreal)> progress, std::function<void(bool, const QString &)> done)
{
    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    const QString toolsDir = m_appDir + "/tools";
    QDir().mkpath(toolsDir);
    const QString zipPath = toolsDir + "/AdGuardHome_windows_amd64.zip";

    QNetworkRequest req{QUrl(QString::fromLatin1(kDownloadUrl))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    QNetworkReply *reply = m_nam->get(req);

    auto file = std::make_shared<QFile>(zipPath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort(); reply->deleteLater();
        done(false, "не удалось создать " + QDir::toNativeSeparators(zipPath));
        return;
    }
    connect(reply, &QNetworkReply::readyRead, this, [reply, file]() { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, this, [progress](qint64 got, qint64 total) {
        if (total > 0 && progress) progress(qreal(got) / total);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, zipPath, toolsDir, done]() {
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(zipPath);
            done(false, "загрузка не удалась: " + reply->errorString());
            return;
        }
        // Архив содержит папку AdGuardHome/ → tools/AdGuardHome (= tools/adguardhome:
        // в Windows регистр имён не важен)
        auto *tar = new QProcess(this);
        tar->setProgram(QStringLiteral("tar"));
        tar->setArguments({ QStringLiteral("-xf"), QDir::toNativeSeparators(zipPath),
                            QStringLiteral("-C"), QDir::toNativeSeparators(toolsDir) });
        tar->setProcessChannelMode(QProcess::MergedChannels);
        connect(tar, &QProcess::finished, this, [this, tar, zipPath, done](int code, QProcess::ExitStatus) {
            const QString out = QString::fromLocal8Bit(tar->readAll()).trimmed();
            tar->deleteLater();
            QFile::remove(zipPath);
            if (code == 0 && isInstalled()) done(true, {});
            else done(false, QStringLiteral("не удалось распаковать архив (tar %1) %2").arg(code).arg(out));
        });
        connect(tar, &QProcess::errorOccurred, this, [tar, zipPath, done](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            tar->deleteLater();
            done(false, QStringLiteral("не найден tar.exe (нужна Windows 10 1803+) — распакуйте ")
                            + QDir::toNativeSeparators(zipPath) + QStringLiteral(" в tools вручную"));
        });
        tar->start();
    });
}

// ── Диагностика ──────────────────────────────────────────────────────────────

QList<int> AdGuardHome::pidsOnPort(const QString &netstat, int port)
{
    // «  UDP    127.0.0.1:53     *:*        1234»
    // «  TCP    0.0.0.0:53       0.0.0.0:0  LISTENING  1234»
    QList<int> pids;
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    for (const QString &raw : netstat.split(QLatin1Char('\n'))) {
        const QStringList f = raw.trimmed().split(ws, Qt::SkipEmptyParts);
        if (f.size() < 4) continue;
        const QString proto = f[0].toUpper();
        if (proto != QLatin1String("UDP") && proto != QLatin1String("TCP")) continue;
        const QString local = f[1];
        const int colon = local.lastIndexOf(QLatin1Char(':'));
        if (colon < 0 || local.mid(colon + 1).toInt() != port) continue;
        const QString host = local.left(colon);
        if (host != QLatin1String("127.0.0.1") && host != QLatin1String("0.0.0.0")
            && host != QLatin1String("[::]") && host != QLatin1String("[::1]")) continue;
        bool ok = false;
        const int pid = f.last().toInt(&ok);
        if (ok && pid > 0 && !pids.contains(pid)) pids << pid;
    }
    return pids;
}

void AdGuardHome::diagnose(std::function<void(const QString &)> done)
{
    if (!m_nam) m_nam = new QNetworkAccessManager(this);
    auto report = std::make_shared<QStringList>();
    auto pending = std::make_shared<int>(2);
    auto finishOne = [report, pending, done]() {
        if (--*pending == 0) done(report->join(QStringLiteral("; ")));
    };

    // 1. Панель AdGuard Home: запущен ли, какие адреса DNS
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/control/status").arg(kWebPort)));
    req.setTransferTimeout(2000);
    QNetworkReply *r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, report, finishOne]() {
        r->deleteLater();
        if (r->error() == QNetworkReply::NoError) {
            *report << "panel status: " + QString::fromUtf8(r->readAll()).simplified().left(300);
            finishOne();
            return;
        }
        *report << "panel :3080 unreachable (" + r->errorString() + ")";
        // Мастер первого запуска на :3000 = служба не нашла наш AdGuardHome.yaml
        QNetworkRequest w(QUrl(QStringLiteral("http://127.0.0.1:3000/")));
        w.setTransferTimeout(2000);
        QNetworkReply *r2 = m_nam->get(w);
        connect(r2, &QNetworkReply::finished, this, [r2, report, finishOne]() {
            r2->deleteLater();
            if (r2->error() == QNetworkReply::NoError
                || r2->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() > 0)
                *report << "setup wizard answers on :3000 — the service did not load AdGuardHome.yaml";
            finishOne();
        });
    });

    // 2. Кто держит порт 53
#ifdef Q_OS_WIN
    auto *ns = new QProcess(this);
    connect(ns, &QProcess::finished, this, [this, ns, report, finishOne](int, QProcess::ExitStatus) {
        const QString out = QString::fromLocal8Bit(ns->readAllStandardOutput());
        ns->deleteLater();
        const QList<int> pids = pidsOnPort(out, kDnsPort);
        if (pids.isEmpty()) { *report << "port 53: nobody listens on 127.0.0.1/0.0.0.0"; finishOne(); return; }
        QStringList names;
        for (int pid : pids) {
            QProcess tl;
            tl.start(QStringLiteral("tasklist"), { "/FI", QString("PID eq %1").arg(pid), "/FO", "CSV", "/NH" });
            tl.waitForFinished(3000);
            const QString line = QString::fromLocal8Bit(tl.readAllStandardOutput()).trimmed();
            const QString name = line.section(QLatin1Char(','), 0, 0).remove(QLatin1Char('"'));
            names << QString("%1 (pid %2)").arg(name.isEmpty() ? QStringLiteral("?") : name).arg(pid);
        }
        *report << "port 53 used by: " + names.join(QStringLiteral(", "));
        finishOne();
    });
    ns->start(QStringLiteral("netstat"), { QStringLiteral("-ano") });
#else
    *report << "port check: Windows only";
    finishOne();
#endif
}

// ── Служба ───────────────────────────────────────────────────────────────────

void AdGuardHome::installService(std::function<void(bool, const QString &)> done)
{
    if (!isInstalled()) { done(false, QStringLiteral("AdGuardHome.exe не найден: ") + exePath()); return; }
    // Служба запускается с рабочей папкой и конфигурацией рядом с exe
    runElevated({ QStringLiteral("-s"), QStringLiteral("install") }, std::move(done));
}

void AdGuardHome::uninstallService(std::function<void(bool, const QString &)> done)
{
    if (!isInstalled()) { done(false, QStringLiteral("AdGuardHome.exe не найден")); return; }
    runElevated({ QStringLiteral("-s"), QStringLiteral("uninstall") }, std::move(done));
}

void AdGuardHome::runElevated(const QStringList &args, std::function<void(bool, const QString &)> done)
{
#ifdef Q_OS_WIN
    const QString exe = QDir::toNativeSeparators(exePath());
    const QString wd  = QDir::toNativeSeparators(dir());
    const QString params = args.join(QLatin1Char(' '));
    QPointer<AdGuardHome> self(this);
    // ShellExecuteEx(runas) показывает окно UAC и ждёт ответа — в фоне, чтобы
    // интерфейс не замирал
    (void)QtConcurrent::run([self, exe, wd, params, done]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const std::wstring wExe = exe.toStdWString(), wWd = wd.toStdWString(), wPar = params.toStdWString();
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize       = sizeof(sei);
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb       = L"runas";
        sei.lpFile       = wExe.c_str();
        sei.lpParameters = wPar.c_str();
        sei.lpDirectory  = wWd.c_str();
        sei.nShow        = SW_HIDE;
        bool ok = false;
        QString msg;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 60000);
            DWORD code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            ok  = (code == 0);
            msg = ok ? QString() : QStringLiteral("AdGuardHome.exe завершился с кодом %1").arg(code);
        } else {
            const DWORD err = GetLastError();
            msg = err == ERROR_CANCELLED ? QStringLiteral("отменено (нет прав администратора)")
                                         : QStringLiteral("ошибка запуска %1").arg(err);
        }
        CoUninitialize();
        if (self) QMetaObject::invokeMethod(self.data(), [done, ok, msg]() { done(ok, msg); },
                                            Qt::QueuedConnection);
    });
#else
    Q_UNUSED(args)
    done(false, QStringLiteral("только Windows"));
#endif
}

// ── DNS ──────────────────────────────────────────────────────────────────────

QByteArray AdGuardHome::buildQuery(quint16 id, const QString &name)
{
    QByteArray q;
    q.append(char(id >> 8)); q.append(char(id & 0xFF));
    q.append(char(0x01)); q.append(char(0x00));      // RD
    q.append(char(0)); q.append(char(1));            // QDCOUNT = 1
    q.append(4, char(0));                            // AN, NS
    q.append(2, char(0));                            // AR
    for (const QString &label : name.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        const QByteArray l = label.toLatin1().left(63);
        q.append(char(l.size()));
        q.append(l);
    }
    q.append(char(0));
    q.append(char(0)); q.append(char(1));            // QTYPE A
    q.append(char(0)); q.append(char(1));            // QCLASS IN
    return q;
}

bool AdGuardHome::isResponseTo(const QByteArray &p, quint16 id)
{
    if (p.size() < 12) return false;
    const quint16 rid = quint16((quint8(p[0]) << 8) | quint8(p[1]));
    return rid == id && (quint8(p[2]) & 0x80);       // QR = ответ
}

void AdGuardHome::probeDns(const QString &host, quint16 port, int timeoutMs, QObject *context,
                           std::function<void(bool, int)> done)
{
    auto *sock  = new QUdpSocket(context);
    auto *timer = new QTimer(sock);
    auto  t     = std::make_shared<QElapsedTimer>();
    auto  fired = std::make_shared<bool>(false);
    const quint16 id = quint16(QRandomGenerator::global()->bounded(1, 0xFFFF));
    t->start();

    auto finish = [sock, fired, t, done](bool ok) {
        if (*fired) return;
        *fired = true;
        const int ms = int(t->elapsed());
        sock->deleteLater();
        done(ok, ms);
    };
    QObject::connect(sock, &QUdpSocket::readyRead, sock, [sock, id, finish]() {
        while (sock->hasPendingDatagrams()) {
            QByteArray d(int(sock->pendingDatagramSize()), Qt::Uninitialized);
            sock->readDatagram(d.data(), d.size());
            if (isResponseTo(d, id)) { finish(true); return; }
        }
    });
    QObject::connect(sock, &QUdpSocket::errorOccurred, sock, [finish](QAbstractSocket::SocketError) {
        finish(false);   // на Windows «порт закрыт» приходит как ошибка сокета
    });
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, sock, [finish]() { finish(false); });
    timer->start(timeoutMs);

    // Имя, которого точно нет в списках блокировки — проверяем сам сервер
    if (sock->writeDatagram(buildQuery(id, QStringLiteral("dns-check.mediastation.invalid")),
                            QHostAddress(host), port) < 0)
        QTimer::singleShot(0, sock, [finish]() { finish(false); });
}
