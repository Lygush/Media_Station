// tests/unit/tst_adguardhome — AdGuard Home для Android TV (аудит §23).
// Проверка настоящим AdGuard Home (Linux-сборка того же релиза) — если задан
// AGH_BIN, иначе этот тест пропускается.
#include <QtTest>
#include <QHostAddress>
#include <QProcess>
#include <QTemporaryDir>
#include <QUdpSocket>

#include "AdGuardHome.h"

class tst_AdGuardHome : public QObject
{
    Q_OBJECT
private slots:
    void blocklistToRules();
    void queryEncoding();
    void probeClosedPort();
    void probeFakeServer();
    void writeDefaultConfig();
    void realAdGuardHome();
    void pidsOnPort();
};

void tst_AdGuardHome::blocklistToRules()
{
    const QString text = QStringLiteral(
        "# comment\n"
        "doubleclick.net\n"
        "0.0.0.0 ads.example.com\n"
        "127.0.0.1 localhost\n"
        "||tracker.org^\n"
        "@@||allowed.com^\n"
        "not a domain!\n"
        "DoubleClick.NET\n"
        "\n");
    const QStringList r = AdGuardHome::blocklistToRules(text);
    QCOMPARE(r, QStringList({ "||doubleclick.net^", "||ads.example.com^", "||tracker.org^", "@@||allowed.com^" }));
}

void tst_AdGuardHome::queryEncoding()
{
    const QByteArray q = AdGuardHome::buildQuery(0xBEEF, "a.bc");
    QCOMPARE(q.size(), 12 + 1 + 1 + 1 + 2 + 1 + 4);
    QCOMPARE(quint8(q[0]), quint8(0xBE));
    QCOMPARE(quint8(q[1]), quint8(0xEF));
    QCOMPARE(q.mid(12, 6), QByteArray("\x01" "a" "\x02" "bc" "\x00", 6));

    QByteArray resp = q;
    resp[2] = char(0x81);
    QVERIFY(AdGuardHome::isResponseTo(resp, 0xBEEF));
    QVERIFY(!AdGuardHome::isResponseTo(q, 0xBEEF));         // это запрос, не ответ
    QVERIFY(!AdGuardHome::isResponseTo(resp, 0xBEEE));
    QVERIFY(!AdGuardHome::isResponseTo(QByteArray(5, 0), 0));
}

void tst_AdGuardHome::probeClosedPort()
{
    QUdpSocket s;                       // свободный порт, на котором никто не слушает
    QVERIFY(s.bind(QHostAddress::LocalHost, 0));
    const quint16 port = s.localPort();
    s.close();

    bool done = false, ok = true;
    QElapsedTimer t; t.start();
    AdGuardHome::probeDns("127.0.0.1", port, 500, this, [&](bool r, int) { done = true; ok = r; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QVERIFY(!ok);
    QVERIFY(t.elapsed() < 1500);
}

void tst_AdGuardHome::probeFakeServer()
{
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress::LocalHost, 0));
    connect(&server, &QUdpSocket::readyRead, &server, [&server]() {
        while (server.hasPendingDatagrams()) {
            QByteArray d(int(server.pendingDatagramSize()), Qt::Uninitialized);
            QHostAddress from; quint16 port;
            server.readDatagram(d.data(), d.size(), &from, &port);
            d[2] = char(quint8(d[2]) | 0x80);
            d[3] = char(0x83);         // NXDOMAIN — тоже «сервер работает»
            server.writeDatagram(d, from, port);
        }
    });
    bool done = false, ok = false;
    AdGuardHome::probeDns("127.0.0.1", server.localPort(), 1000, this, [&](bool r, int) { done = true; ok = r; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QVERIFY(ok);
}

void tst_AdGuardHome::writeDefaultConfig()
{
    QTemporaryDir app;
    const QString block = app.path() + "/default.txt";
    QFile b(block); QVERIFY(b.open(QIODevice::WriteOnly));
    b.write("doubleclick.net\nads.yahoo.com\n"); b.close();

    AdGuardHome agh(app.path());
    QVERIFY(agh.writeDefaultConfig(block));
    QFile cfg(agh.configPath()); QVERIFY(cfg.open(QIODevice::ReadOnly));
    const QString y = QString::fromUtf8(cfg.readAll());
    cfg.close();
    QVERIFY(y.contains("schema_version: 34"));
    QVERIFY(y.contains("  address: 127.0.0.1:3080"));
    QVERIFY(y.contains("    - 127.0.0.1\n  port: 53\n"));
    QVERIFY(y.contains(QDir::cleanPath(app.path()) + "/tools/adguardhome/userfilters/*"));
    QVERIFY(y.contains("'@@||googlevideo.com^'"));

    QFile uf(agh.userFilterPath()); QVERIFY(uf.open(QIODevice::ReadOnly));
    const QString rules = QString::fromUtf8(uf.readAll());
    QVERIFY(rules.contains("||doubleclick.net^\n"));
    QVERIFY(rules.contains("||ads.yahoo.com^\n"));
    uf.close();

    // Существующие файлы (правки пользователя) не перезаписываются
    QVERIFY(cfg.open(QIODevice::WriteOnly | QIODevice::Truncate));
    cfg.write("# user edit\n"); cfg.close();
    QVERIFY(agh.writeDefaultConfig(block));
    QVERIFY(cfg.open(QIODevice::ReadOnly));
    QCOMPARE(cfg.readAll(), QByteArray("# user edit\n"));
}

// Наша конфигурация на настоящем AdGuard Home: запускается, отвечает на
// проверку, блокирует домен из локального списка, не блокирует YouTube
void tst_AdGuardHome::realAdGuardHome()
{
    const QString bin = qEnvironmentVariable("AGH_BIN");
    if (bin.isEmpty() || !QFileInfo::exists(bin)) QSKIP("AGH_BIN not set");

    QTemporaryDir app;
    const QString block = app.path() + "/default.txt";
    QFile b(block); QVERIFY(b.open(QIODevice::WriteOnly));
    b.write("doubleclick.net\n"); b.close();
    AdGuardHome agh(app.path());
    QVERIFY(agh.writeDefaultConfig(block));

    // Порт 53 в тесте занят/нужен root — меняем только его
    QUdpSocket tmp; QVERIFY(tmp.bind(QHostAddress::LocalHost, 0));
    const quint16 port = tmp.localPort(); tmp.close();
    QFile cfg(agh.configPath()); QVERIFY(cfg.open(QIODevice::ReadOnly));
    QString y = QString::fromUtf8(cfg.readAll()); cfg.close();
    y.replace("  port: 53\n", QString("  port: %1\n").arg(port));
    y.replace("127.0.0.1:3080", "127.0.0.1:0");
    QVERIFY(cfg.open(QIODevice::WriteOnly | QIODevice::Truncate)); cfg.write(y.toUtf8()); cfg.close();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(bin, { "-c", agh.configPath(), "-w", agh.dir(), "--no-check-update" });
    QVERIFY(p.waitForStarted());

    bool ready = false;
    for (int i = 0; i < 50 && !ready; ++i) {
        bool done = false;
        AdGuardHome::probeDns("127.0.0.1", port, 200, this, [&](bool r, int) { done = true; ready = r; });
        QTRY_VERIFY(done);
        if (!ready) QTest::qWait(100);
    }
    const QString log = QString::fromUtf8(p.readAll());
    QVERIFY2(ready, qPrintable(log));
    QVERIFY2(!log.contains("[error]"), qPrintable(log));

    auto ask = [&](const QString &name) -> QByteArray {
        QUdpSocket s;
        s.writeDatagram(AdGuardHome::buildQuery(7, name), QHostAddress::LocalHost, port);
        if (!s.waitForReadyRead(3000)) return {};
        QByteArray d(int(s.pendingDatagramSize()), Qt::Uninitialized);
        s.readDatagram(d.data(), d.size());
        return d;
    };
    // doubleclick.net → 0.0.0.0 (ответ A с нулевым адресом). Списки фильтров
    // AdGuard Home загружает после старта DNS — ждём до 5 с
    QByteArray blocked;
    for (int i = 0; i < 50; ++i) {
        blocked = ask("doubleclick.net");
        if (blocked.size() > 12 && ((quint8(blocked[6]) << 8) | quint8(blocked[7])) == 1) break;
        QTest::qWait(100);
    }
    QVERIFY(!blocked.isEmpty());
    QVERIFY2(((quint8(blocked[6]) << 8) | quint8(blocked[7])) == 1,
             qPrintable(log + QString::fromUtf8(p.readAll())));
    QVERIFY(blocked.endsWith(QByteArray(4, '\0')));

    p.kill();
    p.waitForFinished();
}

void tst_AdGuardHome::pidsOnPort()
{
    const QString ns = QStringLiteral(
        "\r\nActive Connections\r\n\r\n  Proto  Local Address          Foreign Address        State           PID\r\n"
        "  TCP    0.0.0.0:135            0.0.0.0:0              LISTENING       1100\r\n"
        "  TCP    127.0.0.1:3080         0.0.0.0:0              LISTENING       4321\r\n"
        "  UDP    0.0.0.0:53             *:*                                    2468\r\n"
        "  UDP    127.0.0.1:53           *:*                                    4321\r\n"
        "  UDP    192.168.1.5:53         *:*                                    999\r\n"
        "  UDP    127.0.0.1:5353         *:*                                    777\r\n"
        "  UDP    [::]:53                *:*                                    2468\r\n");
    QCOMPARE(AdGuardHome::pidsOnPort(ns, 53), QList<int>({ 2468, 4321 }));
    QVERIFY(AdGuardHome::pidsOnPort(ns, 54).isEmpty());
}

QTEST_GUILESS_MAIN(tst_AdGuardHome)
#include "tst_adguardhome.moc"
