// EmulatorGrpcClient против настоящего gRPC-сервера (grpcio, Python) по
// emulator_controller.proto из пакета эмулятора: кодирование сообщений,
// HTTP/2 без TLS, авторизация, порядок нажатий. Нужны python3 и grpcio
// (pip install grpcio grpcio-tools) — без них тест пропускается.
#include <QtTest>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include "EmulatorGrpcClient.h"

class TstEmulatorGrpc : public QObject {
    Q_OBJECT
    QProcess server;
    QTemporaryDir dir;
    QString log;
    int port = 0;
    const QByteArray token = "s3cret-token";

    QStringList logLines() {
        QFile f(log); if (!f.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    }

private slots:
    void statusMerged();
    void initTestCase() {
        QProcess probe;
        probe.start("python3", {"-c", "import grpc, grpc_tools"});
        if (!probe.waitForFinished(20000) || probe.exitCode() != 0)
            QSKIP("python3 with grpcio/grpcio-tools not available");
        QTcpServer t; QVERIFY(t.listen(QHostAddress::LocalHost, 0)); port = t.serverPort(); t.close();
        log = dir.filePath("keys.log");
        server.setProcessChannelMode(QProcess::MergedChannels);
        server.start("python3", {QString(FAKE_SERVER_DIR) + "/fake_emulator_grpc.py",
                                 QString::number(port), QString::fromLatin1(token), log});
        QVERIFY2(server.waitForStarted(10000), "python3 failed to start");
        QByteArray out;
        QTRY_VERIFY_WITH_TIMEOUT((out += server.readAll()).contains("READY")
                                 || server.state() == QProcess::NotRunning, 30000);
        QVERIFY2(server.state() == QProcess::Running, out.constData());
    }
    void cleanupTestCase() { server.kill(); server.waitForFinished(); }

    // Кодирование — сверка с эталонными байтами protobuf
    void encoding() {
        QCOMPARE(EmulatorGrpcClient::encodeKeyPress(108).toHex(), QByteArray("08011002186c"));
        QCOMPARE(EmulatorGrpcClient::encodeKeyPress(232).toHex(), QByteArray("0801100218e801"));
        QCOMPARE(EmulatorGrpcClient::frame("ab").toHex(), QByteArray("00000000026162"));
        QByteArray m; QVERIFY(EmulatorGrpcClient::unframe(QByteArray::fromHex("00000000026162"), &m));
        QCOMPARE(m, QByteArray("ab"));
        QVERIFY(!EmulatorGrpcClient::unframe(QByteArray::fromHex("0000000009"), &m));
    }

    void statusWithToken() {
        EmulatorGrpcClient c; c.configure(port, token);
        bool done = false; EmulatorGrpcClient::Status st;
        c.getStatus([&](const EmulatorGrpcClient::Status &s) { st = s; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY2(st.ok, qPrintable(st.error));
        QVERIFY(st.booted);
        QCOMPARE(st.version, QString("36.4.10.0 (fake)"));
    }

    void wrongTokenRejected() {
        EmulatorGrpcClient c; c.configure(port, "wrong");
        bool done = false; EmulatorGrpcClient::Status st;
        c.getStatus([&](const EmulatorGrpcClient::Status &s) { st = s; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!st.ok);
        QVERIFY2(st.error.contains("16") || st.error.contains("bad token"), qPrintable(st.error));
    }

    void noServer() {
        QTcpServer t; QVERIFY(t.listen(QHostAddress::LocalHost, 0)); const int dead = t.serverPort(); t.close();
        EmulatorGrpcClient c; c.configure(dead, token);
        bool done = false; EmulatorGrpcClient::Status st;
        c.getStatus([&](const EmulatorGrpcClient::Status &s) { st = s; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!st.ok);
    }

    // Порядок нажатий сохраняется; текст доходит
    void keysInOrderAndText() {
        QFile::remove(log);
        EmulatorGrpcClient c; c.configure(port, token);
        QStringList failures;
        connect(&c, &EmulatorGrpcClient::callFailed, this, [&](const QString &m, const QString &e) { failures << m + ": " + e; });
        const QList<int> keys = {108, 108, 103, 105, 106, 232, 158};
        for (int k : keys) c.sendKeyPress(k);
        c.sendText("hello 123");
        QTRY_COMPARE_WITH_TIMEOUT(logLines().size(), keys.size() + 1, 8000);
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ")));
        const QStringList lines = logLines();
        for (int i = 0; i < keys.size(); ++i)
            QCOMPARE(lines.at(i), QString("key 1 2 %1").arg(keys.at(i)));
        QCOMPARE(lines.last(), QString("text hello 123"));
    }
};

void TstEmulatorGrpc::statusMerged()
{
    QVERIFY(EmulatorGrpcClient::statusIsOk(""));
    QVERIFY(EmulatorGrpcClient::statusIsOk("0"));
    QVERIFY(EmulatorGrpcClient::statusIsOk("0, 0"));
    QVERIFY(!EmulatorGrpcClient::statusIsOk("16"));
    QVERIFY(!EmulatorGrpcClient::statusIsOk("0, 16"));
}

QTEST_MAIN(TstEmulatorGrpc)
#include "tst_emulator_grpc.moc"
