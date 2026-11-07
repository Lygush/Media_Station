// Ввод клавиш в Android TV через консоль эмулятора: самопроверка getevent и
// переход на adb. Консоль эмулятора — поддельный TCP-сервер с тем же
// протоколом (приветствие, OK/KO), adb — скрипт, записывающий вызовы.
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

#define private public
#define protected public
#include "AndroidTVModule.h"
#undef private
#undef protected

class FakeConsole : public QObject {
    Q_OBJECT
public:
    QTcpServer server;
    QByteArrayList commands;
    QString flagFile;            // создаётся при получении event send (для «getevent»)
    bool rejectSyn = true;       // как эмулятор 36.4: EV_SYN — «invalid event code»
    bool start() {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *s = server.nextPendingConnection();
            s->write("Android Console: type 'help' for a list of commands\r\nOK\r\n");
            connect(s, &QTcpSocket::readyRead, this, [this, s]() {
                while (s->canReadLine()) {
                    const QByteArray line = s->readLine().trimmed();
                    commands << line;
                    if (line.startsWith("event send")) {
                        if (rejectSyn && line.contains("EV_SYN")) {
                            s->write("KO: invalid event code in 'EV_SYN:SYN_REPORT:0'\r\n");
                            continue;
                        }
                        QFile f(flagFile); f.open(QIODevice::WriteOnly); f.close();
                    }
                    s->write("OK\r\n");
                }
            });
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
};

class TstAndroidTvConsole : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString adbLog, flag;

    // Вывод `getevent -pl` как у эмулятора: сенсорный экран + клавиатура
    static QString geteventListing() {
        return QStringLiteral(
            "add device 1: /dev/input/event2\n"
            "  bus:      0006\n"
            "  name:     \"virtio_input_multi_touch_1\"\n"
            "  events:\n"
            "    ABS (0003): ABS_MT_SLOT           : value 0, min 0, max 9\n"
            "  input props:\n"
            "    INPUT_PROP_DIRECT\n"
            "add device 2: /dev/input/event1\n"
            "  bus:      0006\n"
            "  name:     \"virtio_input_keyboard\"\n"
            "  events:\n"
            "    KEY (0001): KEY_ESC               KEY_1                 KEY_2\n"
            "                KEY_UP                KEY_LEFT              KEY_RIGHT\n"
            "                KEY_DOWN              KEY_ENTER             KEY_BACK\n"
            "    MSC (0004): MSC_SCAN\n"
            "  input props:\n"
            "    <none>\n");
    }

    // evdevWritable: есть ли у «shell» право записи в устройство клавиатуры
    void writeFakeAdb(bool eventsReachAndroid, bool evdevWritable = false, bool suOk = false) {
        const QString adbDir = dir.path() + "/tools/android-sdk/platform-tools";
        QDir().mkpath(adbDir);
        const QString script = adbDir + "/adb.exe";
        QFile f(script);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        // getevent: если события доходят — ждём флаг от консоли и печатаем EV_KEY
        QString s = "#!/bin/bash\n"
                    "echo \"$*\" >> '" + adbLog + "'\n"
                    "if [[ \"$*\" == *'getevent -pl'* ]]; then\n"
                    "  cat <<'LISTING'\n" + QString(suOk ? "SU_OK\n" : "") + geteventListing() + "LISTING\n";
        if (evdevWritable) s += "  echo WRITABLE /dev/input/event1\n";
        s += "  exit 0\nfi\n"
             "if [[ \"$*\" == *getevent* ]]; then\n";
        if (eventsReachAndroid)
            s += "  for i in $(seq 1 40); do [ -f '" + flag + "' ] && { echo '/dev/input/event1: EV_KEY KEY_DOWN DOWN'; sleep 5; exit 0; }; sleep 0.05; done\n";
        s += "  sleep 5; exit 0\nfi\n"
             "if [[ \"$*\" == *' shell' ]]; then\n"
             "  while read -r line; do echo \"STDIN $line\" >> '" + adbLog + "';"
             " echo __MS_KEYS_DONE__; done\nfi\n";
        f.write(s.toUtf8()); f.close();
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
    }

    AndroidTVModule *makeModule(FakeConsole &c) {
        auto *m = new AndroidTVModule();
        m->m_appDir = dir.path();
        m->m_running = true;
        m->m_emulatorShown = true;
        m->m_consoleInput = true;
        m->m_consolePort = c.server.serverPort();
        return m;
    }
    QString log() { QFile f(adbLog); f.open(QIODevice::ReadOnly); return QString::fromUtf8(f.readAll()); }

private slots:
    void grpcTokenFromDiscovery();
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        QSettings("MyCompany", "MediaStation").remove("androidtv/consoleInputBrokenFor");
        QFile::remove(dir.path() + "/tools/android-sdk/emulator/emulator.exe");
        adbLog = dir.path() + "/adb.log"; flag = dir.path() + "/event.flag";
        QFile::remove(adbLog); QFile::remove(flag);
    }

    // События доходят: проверка проходит, первое нажатие НЕ теряется, без EV_SYN
    void consoleWorks() {
        writeFakeAdb(true);
        FakeConsole c; c.flagFile = flag; QVERIFY(c.start());
        AndroidTVModule *m = makeModule(c);
        m->handleRemoteKey("down");
        QTRY_VERIFY_WITH_TIMEOUT(m->m_consoleVerified, 4000);
        bool sentDown = false;
        for (const QByteArray &cmd : c.commands) {
            QVERIFY2(!cmd.contains("EV_SYN"), cmd.constData());
            if (cmd == "event send EV_KEY:KEY_DOWN:1 EV_KEY:KEY_DOWN:0") sentDown = true;
        }
        QVERIFY2(sentDown, "first key was lost");
        QVERIFY(m->m_consoleState == AndroidTVModule::ConsoleState::Ready);
        // следующее нажатие — сразу в консоль
        m->handleRemoteKey("right");
        QTRY_VERIFY(c.commands.contains("event send EV_KEY:KEY_RIGHT:1 EV_KEY:KEY_RIGHT:0"));
        QVERIFY(!log().contains("input keyevent"));
        delete m;
    }

    // События не доходят: переход на adb, нажатие повторяется через adb
    // Разбор вывода getevent: устройство клавиатуры, нужные клавиши
    void pickEvdevDeviceParsesListing() {
        QSet<int> keys; QString name;
        const QString out = geteventListing() + "WRITABLE /dev/input/event1\n";
        QCOMPARE(AndroidTVModule::pickEvdevDevice(out, &keys, &name), QString("/dev/input/event1"));
        QCOMPARE(name, QString("virtio_input_keyboard"));
        QCOMPARE(keys, (QSet<int>{103, 105, 106, 108, 28, 158}));
        // без права записи — не годится
        QVERIFY(AndroidTVModule::pickEvdevDevice(geteventListing(), &keys, &name).isEmpty());
    }

    // Есть доступная клавиатура: стрелки/OK/Назад — sendevent, без консоли и JVM
    void evdevPreferred() {
        writeFakeAdb(false, true);
        FakeConsole c; c.flagFile = flag; QVERIFY(c.start());
        AndroidTVModule *m = makeModule(c);
        m->handleRemoteKey("down");      // пришло во время проверки устройства
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("STDIN sendevent /dev/input/event1 1 108 1;"
                                                "sendevent /dev/input/event1 0 0 0;"
                                                "sendevent /dev/input/event1 1 108 0;"
                                                "sendevent /dev/input/event1 0 0 0;"), 4000);
        m->handleRemoteKey("select");    // KEY_REPLY нет — KEY_ENTER
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("sendevent /dev/input/event1 1 28 1"), 3000);
        m->handleRemoteKey("back");
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("sendevent /dev/input/event1 1 158 1"), 3000);
        m->handleRemoteKey("home");      // нет в sendevent — input keyevent
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("input keyevent 3"), 3000);
        QVERIFY2(c.commands.isEmpty(), "console must not be used when sendevent works");
        delete m;
    }

    // Права записи нет, но есть su (образ без Google Play): sendevent через su
    void evdevViaSu() {
        QSet<int> keys; QString name; bool viaSu = false;
        QCOMPARE(AndroidTVModule::pickEvdevDevice("SU_OK\n" + geteventListing(), &keys, &name, &viaSu),
                 QString("/dev/input/event1"));
        QVERIFY(viaSu);

        writeFakeAdb(false, false, true);
        FakeConsole c; c.flagFile = flag; QVERIFY(c.start());
        AndroidTVModule *m = makeModule(c);
        m->handleRemoteKey("up");
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("STDIN su 0 sh -c 'sendevent /dev/input/event1 1 103 1;"), 4000);
        QVERIFY(c.commands.isEmpty());
        delete m;
    }

    // Консоль не работает: запоминается для этой сборки эмулятора, при
    // следующем запуске проверка не повторяется — сразу adb
    void consoleFailureRemembered() {
        writeFakeAdb(false);
        QDir().mkpath(dir.path() + "/tools/android-sdk/emulator");
        { QFile e(dir.path() + "/tools/android-sdk/emulator/emulator.exe"); QVERIFY(e.open(QIODevice::WriteOnly)); e.write("x"); }
        FakeConsole c; c.flagFile = flag; QVERIFY(c.start());
        AndroidTVModule *m = makeModule(c);
        m->handleRemoteKey("down");
        QTRY_VERIFY_WITH_TIMEOUT(m->m_consoleState == AndroidTVModule::ConsoleState::Failed, 4000);
        delete m;

        QFile::remove(adbLog);
        FakeConsole c2; c2.flagFile = flag; QVERIFY(c2.start());
        AndroidTVModule *m2 = makeModule(c2);
        m2->handleRemoteKey("up");
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("STDIN input keyevent 19"), 3000);
        QVERIFY2(c2.commands.isEmpty(), "console must be skipped when known broken");
        QVERIFY(!log().contains("getevent -lq"));
        delete m2;
    }

    // gRPC эмулятора (настоящий сервер grpcio из tst_emulator_grpc): нажатия
    // идут через sendKey, проверка getevent проходит, консоль и adb не нужны
    void grpcPreferred() {
        QProcess probe;
        probe.start("python3", {"-c", "import grpc, grpc_tools"});
        if (!probe.waitForFinished(20000) || probe.exitCode() != 0)
            QSKIP("python3 with grpcio not available");
        writeFakeAdb(true);                       // getevent видит EV_KEY, когда появится flag
        QTcpServer t; QVERIFY(t.listen(QHostAddress::LocalHost, 0)); const int port = t.serverPort(); t.close();
        QProcess server;
        server.setProcessChannelMode(QProcess::MergedChannels);
        // Сервер пишет нажатия в flag — для поддельного getevent это «событие дошло»
        server.start("python3", {QString(GRPC_SERVER_DIR) + "/fake_emulator_grpc.py",
                                 QString::number(port), "-", flag});
        QVERIFY(server.waitForStarted(10000));
        QByteArray out;
        QTRY_VERIFY_WITH_TIMEOUT((out += server.readAll()).contains("READY"), 30000);

        FakeConsole c; c.flagFile = dir.path() + "/console.flag"; QVERIFY(c.start());
        AndroidTVModule *m = makeModule(c);
        m->m_grpcPort = port;
        m->startGrpcCheck();
        m->handleRemoteKey("down");
        QTRY_VERIFY_WITH_TIMEOUT(m->m_grpcVerified, 6000);
        m->handleRemoteKey("select");
        m->handleRemoteKey("back");
        auto lines = [this]() { QFile f(flag); f.open(QIODevice::ReadOnly);
                                return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts); };
        QTRY_COMPARE_WITH_TIMEOUT(lines(), (QStringList{"key 1 2 108", "key 1 2 28", "key 1 2 158"}), 4000);
        QVERIFY2(c.commands.isEmpty(), "console must not be used");
        QVERIFY(!log().contains("input keyevent"));
        QVERIFY(m->m_grpcState == AndroidTVModule::GrpcState::Ready);
        delete m;
        server.kill(); server.waitForFinished();
    }

    // gRPC не отвечает — после повторов остальные пути, нажатие не теряется
    void grpcUnavailableFallsBack() {
        writeFakeAdb(false);
        QTcpServer t; QVERIFY(t.listen(QHostAddress::LocalHost, 0)); const int dead = t.serverPort(); t.close();
        FakeConsole c; c.flagFile = flag; QVERIFY(c.start());
        AndroidTVModule *m = makeModule(c);
        m->m_grpcPort = dead;
        m->startGrpcCheck();
        m->handleRemoteKey("left");
        QTRY_VERIFY_WITH_TIMEOUT(m->m_grpcState == AndroidTVModule::GrpcState::Failed, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("STDIN input keyevent 21"), 6000);
        delete m;
    }

    // Удаление профиля: с данными — папка .avd и .ini удалены; без — остались;
    // общий с другим профилем AVD не трогается
    void removeProfileData() {
        QTemporaryDir base;
        AndroidTVProfileManager pm;
        pm.load(base.path());
        pm.setAvdHome(base.path() + "/avd");
        const QString a = pm.addProfile("system-images/android-34/android-tv/x86", "TV A");
        const QString b = pm.addProfile("system-images/android-34/android-tv/x86", "TV B");
        const QString c = pm.addProfile("system-images/android-34/android-tv/x86", "TV C");
        pm.setActiveProfile(c);
        auto avdOf = [&](const QString &id) { return pm.getProfile(id).value("avdName").toString(); };
        auto makeAvd = [&](const QString &name) {
            QDir().mkpath(base.path() + "/avd/" + name + ".avd/snapshots");
            QFile f(base.path() + "/avd/" + name + ".avd/userdata-qemu.img.qcow2"); f.open(QIODevice::WriteOnly); f.write("x");
            QFile ini(base.path() + "/avd/" + name + ".ini"); ini.open(QIODevice::WriteOnly); ini.write("path=");
        };
        const QString avdA = avdOf(a), avdB = avdOf(b);
        QVERIFY(!avdA.isEmpty() && avdA != avdB);
        makeAvd(avdA); makeAvd(avdB);

        QVERIFY(pm.removeProfile(a, true));
        QVERIFY(!QDir(base.path() + "/avd/" + avdA + ".avd").exists());
        QVERIFY(!QFile::exists(base.path() + "/avd/" + avdA + ".ini"));

        QVERIFY(pm.removeProfile(b, false));
        QVERIFY(QDir(base.path() + "/avd/" + avdB + ".avd").exists());
        QVERIFY(QFile::exists(base.path() + "/avd/" + avdB + ".ini"));

        QVERIFY(!pm.removeProfile(c, true));     // активный не удаляется
        QVERIFY(pm.getProfile(a).isEmpty() && pm.getProfile(b).isEmpty());
        QVERIFY(!pm.getProfile(c).isEmpty());
    }

    void consoleSilentFallsBackToAdb() {
        writeFakeAdb(false);
        FakeConsole c; c.flagFile = flag; QVERIFY(c.start());
        AndroidTVModule *m = makeModule(c);
        m->handleRemoteKey("down");
        QTRY_VERIFY_WITH_TIMEOUT(m->m_consoleState == AndroidTVModule::ConsoleState::Failed, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("STDIN input keyevent 20"), 3000);
        m->handleRemoteKey("left");
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("STDIN input keyevent 21"), 3000);
        delete m;
    }
};

// Токен gRPC — из файла обнаружения эмулятора (pid_<PID>.ini), не токен консоли
void TstAndroidTvConsole::grpcTokenFromDiscovery()
{
    const QByteArray ini =
        "port.serial=5554\r\nport.adb=5555\r\navd.name=MediaStation_TV\r\n"
        "grpc.port=8554\r\ngrpc.token=Zx9+abc/DEF==\r\ncmdline=\"emulator.exe\" -grpc 8554\r\n";
    QCOMPARE(AndroidTVModule::grpcTokenFromDiscovery(ini, 8554), QByteArray("Zx9+abc/DEF=="));
    QCOMPARE(AndroidTVModule::grpcTokenFromDiscovery(ini, 0),    QByteArray("Zx9+abc/DEF=="));
    QVERIFY(AndroidTVModule::grpcTokenFromDiscovery(ini, 8555).isEmpty());   // чужой эмулятор
    QVERIFY(AndroidTVModule::grpcTokenFromDiscovery("port.serial=5554\n", 8554).isEmpty());
}

QTEST_MAIN(TstAndroidTvConsole)
#include "tst_androidtv_console.moc"
