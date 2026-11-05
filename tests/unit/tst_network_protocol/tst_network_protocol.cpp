// tst_network_protocol — проверка протокола приёма установок NetworkModule.
// Поднимает настоящий TCP-сервер на 127.0.0.1:12345 и проходит сценарии
// клиентом: согласование, приём, распаковка, обрывы, отмена, нехватка места.
// Нужен 7zr/7z: tools/7zr.exe рядом с тестом или в PATH. Порт 12345 должен
// быть свободен (закройте MediaStation перед запуском).
#include <QCoreApplication>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <cstdio>

#define private public
#include "NetworkModule.h"
#undef private
#include "core/EventBus.h"
#include "SettingsManager.h"
#include "tools/SevenZip.h"
#include "InstallUtils.h"

static QString g_installPath;
QString SettingsManager::installPath() const { return g_installPath; }

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) std::printf("  PASS  %s\n", msg); \
    else { std::printf("  FAIL  %s\n", msg); ++g_fail; } } while (0)

static QByteArray line(const QJsonObject &o)
{ return QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n"; }

// Ждём строку от сервера
static QByteArray waitLine(QTcpSocket &c, QByteArray &buf, int ms = 3000)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        int nl = buf.indexOf('\n');
        if (nl >= 0) { QByteArray l = buf.left(nl).trimmed(); buf = buf.mid(nl + 1); return l; }
        if (c.state() != QAbstractSocket::ConnectedState && c.bytesAvailable() == 0) break;
        c.waitForReadyRead(50);
        QCoreApplication::processEvents();
        buf += c.readAll();
    }
    int nl = buf.indexOf('\n');
    if (nl >= 0) { QByteArray l = buf.left(nl).trimmed(); buf = buf.mid(nl + 1); return l; }
    return {};
}

static bool waitDisconnected(QTcpSocket &c, int ms = 3000)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        if (c.state() == QAbstractSocket::UnconnectedState) return true;
        c.waitForReadyRead(50);
        QCoreApplication::processEvents();
    }
    return c.state() == QAbstractSocket::UnconnectedState;
}

static QByteArray makeArchive(const QString &dir, int payloadMb)
{
    QDir().mkpath(dir + "/src");
    QFile f(dir + "/src/Game.exe");
    if (!f.open(QIODevice::WriteOnly)) return {};
    QByteArray block(1024 * 1024, 0);
    for (int i = 0; i < payloadMb; ++i) {
        for (int j = 0; j < block.size(); j += 4)
            *reinterpret_cast<quint32 *>(block.data() + j) = QRandomGenerator::global()->generate();
        f.write(block);
    }
    f.close();
    if (SevenZip::compress(dir + "/src", QStringLiteral("Game.exe"), dir + "/a.7z",
                           nullptr, {}) != SevenZip::Ok)
        return {};
    QFile a(dir + "/a.7z");
    if (!a.open(QIODevice::ReadOnly)) return {};
    return a.readAll();
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    g_installPath = tmp.path() + "/games";

    EventBus bus;
    NetworkModule net;
    net.m_bus         = &bus;
    net.m_installPath = g_installPath;
    net.initServer();
    QTest::qWait(100);

    QSignalSpy cancelled(&net, &NetworkModule::serverCancelledInstall);
    QSignalSpy extracted(&bus, &EventBus::networkExtractFinished);

    // Диалог установки «пользователь нажал Установить»: сразу accept с заданным id
    QString nextTileId;
    QObject::connect(&net, &NetworkModule::showInstallDialog, &net,
        [&](const QString &, const QString &, const QString &, const QString &, QTcpSocket *s) {
            if (!nextTileId.isEmpty()) net.acceptInstall(s, nextTileId);
        });

    const QByteArray archive = makeArchive(tmp.path() + "/arc", 8);
    std::printf("archive: %lld bytes\n", (long long)archive.size());
    if (archive.isEmpty()) {
        std::printf("SKIP: 7zr/7z не найден\n");
        return 1;
    }

    // ── T1: JSON согласования и PACK в одном сегменте; полная установка ──
    {
        std::printf("T1 negotiation+PACK in one segment, full install\n");
        nextTileId = "net_t1";
        QTcpSocket c; c.connectToHost("127.0.0.1", 12345);
        if (!c.waitForConnected(2000)) { std::printf("SKIP: порт 12345 недоступен\n"); return 1; }
        QByteArray buf;
        c.write(line({{"title", "My Game."}, {"kind", "app"}, {"exeRelativePath", "Game.exe"}}) + "PACK:5\n");
        CHECK(waitLine(c, buf) == "OK", "server accepted, PACK line did not break connection");
        c.write(line({{"archiveSize", archive.size()}}));
        CHECK(waitLine(c, buf) == "READY", "READY received");
        c.write(archive); c.flush();
        QByteArray l = waitLine(c, buf, 15000);
        CHECK(l.startsWith("OK: installation started"), "installation started");
        CHECK(extracted.size() == 1 && extracted.at(0).at(0).toString() == "net_t1", "networkExtractFinished emitted");
        const QString root = extracted.size() ? extracted.at(0).at(2).toString() : QString();
        CHECK(QFile::exists(root + "/game/Game.exe"), "game extracted");
        CHECK(QDir(root).dirName() == "My Game", "trailing dot stripped from folder name");
        CHECK(cancelled.isEmpty(), "no cancel signal on success");
        waitDisconnected(c);
        QTest::qWait(100);
        CHECK(cancelled.isEmpty(), "no cancel signal after successful disconnect");
        CHECK(net.m_clients.isEmpty(), "client state released");
    }

    // ── T2: отправитель отключился на середине приёма ──
    {
        std::printf("T2 disconnect while receiving\n");
        cancelled.clear();
        nextTileId = "net_t2";
        QTcpSocket c; c.connectToHost("127.0.0.1", 12345); c.waitForConnected(2000);
        QByteArray buf;
        c.write(line({{"title", "Two"}, {"kind", "app"}}));
        waitLine(c, buf);
        c.write(line({{"archiveSize", archive.size()}}));
        waitLine(c, buf);
        c.write(archive.left(archive.size() / 3)); c.flush();
        QTest::qWait(200);
        CHECK(net.hasActiveInstall("net_t2"), "hasActiveInstall during receive");
        const QString tmpArchive = net.m_clients.isEmpty() ? QString() : net.m_clients.first().tempArchivePath;
        c.abort();
        QTest::qWaitFor([&] { return !cancelled.isEmpty(); }, 3000);
        CHECK(cancelled.size() == 1 && cancelled.at(0).at(0).toString() == "net_t2",
              "serverCancelledInstall emitted on disconnect (tile removed from UI)");
        CHECK(!tmpArchive.isEmpty() && !QFile::exists(tmpArchive), "partial temp archive removed");
        CHECK(!net.hasActiveInstall("net_t2"), "no active install after disconnect");
    }

    // ── T3: CANCEL от отправителя в фазе упаковки ──
    {
        std::printf("T3 CANCEL during packing\n");
        cancelled.clear();
        nextTileId = "net_t3";
        QTcpSocket c; c.connectToHost("127.0.0.1", 12345); c.waitForConnected(2000);
        QByteArray buf;
        c.write(line({{"title", "Three"}, {"kind", "app"}}));
        waitLine(c, buf);
        c.write("PACK:40\nCANCEL\n");
        CHECK(waitDisconnected(c), "server closed the connection after CANCEL");
        CHECK(cancelled.size() == 1, "exactly one cancel signal");
    }

    // ── T4: нехватка места ──
    {
        std::printf("T4 not enough disk space\n");
        cancelled.clear();
        nextTileId = "net_t4";
        QTcpSocket c; c.connectToHost("127.0.0.1", 12345); c.waitForConnected(2000);
        QByteArray buf;
        c.write(line({{"title", "Four"}, {"kind", "app"}}));
        waitLine(c, buf);
        c.write(line({{"archiveSize", (qint64)1 << 52}}));   // 4 ПБ
        CHECK(waitLine(c, buf).startsWith("FAIL: not enough disk space"), "FAIL before transfer");
        waitDisconnected(c);
        QTest::qWait(100);
        CHECK(cancelled.size() == 1, "tile removed from UI");
    }

    // ── T5: отправитель отключился сразу после передачи (идёт распаковка) ──
    {
        std::printf("T5 disconnect during extraction\n");
        cancelled.clear(); extracted.clear();
        nextTileId = "net_t5";
        QTcpSocket c; c.connectToHost("127.0.0.1", 12345); c.waitForConnected(2000);
        QByteArray buf;
        c.write(line({{"title", "Five"}, {"kind", "app"}}));
        waitLine(c, buf);
        c.write(line({{"archiveSize", archive.size()}}));
        waitLine(c, buf);
        c.write(archive);
        QTest::qWaitFor([&] { return c.bytesToWrite() == 0; }, 10000);
        // ждём, пока сервер перейдёт в распаковку, и обрываем
        QTest::qWaitFor([&] {
            for (const auto &st : net.m_clients) if (st.phase == NetworkModule::PhaseExtracting) return true;
            return false; }, 5000);
        c.abort();
        QTest::qWaitFor([&] { return !extracted.isEmpty(); }, 15000);
        CHECK(extracted.size() == 1, "installation completed despite sender disconnect");
        CHECK(cancelled.isEmpty(), "no cancel signal");
    }

    // ── T6: имена папок ──
    {
        std::printf("T6 InstallUtils::sanitizeFolderName\n");
        CHECK(InstallUtils::sanitizeFolderName("", "abcdef123456") == "Game_abcdef12", "empty title");
        CHECK(InstallUtils::sanitizeFolderName("CON", "x") == "_CON", "reserved name");
        CHECK(InstallUtils::sanitizeFolderName("a:b?c", "x") == "a_b_c", "invalid chars");
        CHECK(InstallUtils::sanitizeFolderName("Name. . ", "x") == "Name", "trailing dots/spaces");
        CHECK(InstallUtils::sanitizeFolderName("Ведьмак 3", "x") == "Ведьмак 3", "cyrillic kept");
    }

    net.shutdown();
    std::printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
