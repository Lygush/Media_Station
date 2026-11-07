// modules/AndroidTVModule/AndroidTVInput.cpp
//
// Управление вводом:
//   • Обработка клавиш пульта G72 (handleRemoteKey)
//   • Маппинг ключей G72 → Android keycode
//   • Отправка через консоль эмулятора (`event send`, без JVM в Android);
//     запасной путь — постоянный `adb shell input keyevent`
//   • Переключение режима клавиш (notifyKeyMode)
//   • Публичный Q_INVOKABLE sendRemoteKey для QML

#include "AndroidTVModule.h"
#include "EmulatorGrpcClient.h"
#include "LoggerModule.h"
#include "G72KeysModule.h"
#include "OverlayModule.h"
#include "core/EventBus.h"

#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QSettings>
#include "tools/LocalModelPaths.h"
#include <QDir>
#include <QHostAddress>
#include <QTcpServer>
#include <memory>
#include <QTimer>
#include <QHash>
#include <QRegularExpression>
#include <algorithm>

// ── Таблица маппинга G72-ключей → Android keycodes ───────────────────────────

/*static*/ int AndroidTVModule::remoteKeyToAndroidKeyCode(const QString &key)
{
    // https://developer.android.com/reference/android/view/KeyEvent
    static const struct { const char *name; int code; } kMap[] = {
        { G72Keys::kHome,      3   }, // KEYCODE_HOME
        { G72Keys::kBack,      4   }, // KEYCODE_BACK
        { G72Keys::kUp,        19  }, // KEYCODE_DPAD_UP
        { G72Keys::kDown,      20  }, // KEYCODE_DPAD_DOWN
        { G72Keys::kLeft,      21  }, // KEYCODE_DPAD_LEFT
        { G72Keys::kRight,     22  }, // KEYCODE_DPAD_RIGHT
        { G72Keys::kSelect,    23  }, // KEYCODE_DPAD_CENTER
        { G72Keys::kMenu,      82  }, // KEYCODE_MENU
        { G72Keys::kSearch,    84  }, // KEYCODE_SEARCH
        { G72Keys::kHomePage,  3   }, // KEYCODE_HOME (браузерная кнопка Home → Android Home)
        { G72Keys::kFavorites, 174 }, // KEYCODE_BOOKMARK
        // Громкость не передаём: она регулируется в Windows (G72KeysModule
        // пропускает эти клавиши в систему)
    };

    for (const auto &entry : kMap)
        if (key == QLatin1String(entry.name)) return entry.code;
    return -1;
}

// ── Обработка клавиши пульта ──────────────────────────────────────────────────

void AndroidTVModule::handleRemoteKey(const QString &key)
{
    // Клавиши нужны эмулятору только когда он показан пользователю
    if (!m_running || !m_emulatorShown) return;

    if (key == QLatin1String(G72Keys::kVolumeUp) || key == QLatin1String(G72Keys::kVolumeDown))
        return;   // громкость — системная, см. G72KeysModule::keyboardHookProc

    // Кнопка Menu — переключает оверлей
    if (key == QLatin1String(G72Keys::kMenu)) {
        if (m_bus) {
            if (m_overlayOpen) {
                emit m_bus->moduleEvent(
                    QLatin1String(Overlay::kModule),
                    QLatin1String(Overlay::kEventHide),
                    QVariantMap{});
            } else {
                emit m_bus->moduleEvent(
                    QLatin1String(Overlay::kModule),
                    QLatin1String(Overlay::kEventShow),
                    Overlay::makeShowEvent(
                        QLatin1String(Overlay::kContextAndroidTV)));
            }
        }
        return;
    }

    // Игнорируем клавиши, если оверлей открыт ИЛИ активен cooldown после закрытия
    if (m_overlayOpen || m_overlayCooldown) {
        logToEmulatorFile(QString("[Input] Key '%1' ignored (overlay open or cooldown)").arg(key));
        return;
    }

    const int keyCode = remoteKeyToAndroidKeyCode(key);
    if (keyCode < 0) {
        logToEmulatorFile(QString("[Input] Unknown remote key: %1").arg(key));
        return;
    }

    sendAndroidKey(keyCode);
}

// ── Отправка keyevent ────────────────────────────────────────────────────────
// Раньше на каждую клавишу запускался новый adb.exe (+ подключение к серверу),
// из-за чего навигация отставала на сотни миллисекунд, а при удержании кнопки
// копилась очередь процессов. Теперь держим один `adb shell` и пишем в его stdin.

void AndroidTVModule::startKeyShell()
{
    if (m_keyShell) return;
    if (!QFile::exists(adbPath())) return;

    m_keyShell = new QProcess(this);
    QProcess *proc = m_keyShell;
    proc->setProgram(adbPath());
    proc->setArguments({"-s", adbSerial(), "shell"});
    proc->setProcessEnvironment(toolsEnvironment());
    proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(proc, &QProcess::readyRead, this, [this, proc]() {
        static const QString kMarker = QStringLiteral("__MS_KEYS_DONE__");
        m_keyShellBuf += QString::fromUtf8(proc->readAll());
        const int markerPos = m_keyShellBuf.lastIndexOf(kMarker);
        if (markerPos >= 0) {
            const QString before = m_keyShellBuf.left(markerPos);
            m_keyShellBuf = m_keyShellBuf.mid(markerPos + kMarker.size());
            const QString out = QString(before).remove(kMarker).trimmed();
            if (!out.isEmpty()) logToEmulatorFile("[Input] shell: " + out.left(300));
            m_keyBatchInFlight = false;
            flushPendingKeys();            // отправляем накопленное за это время
        } else if (m_keyShellBuf.size() > 4096) {
            logToEmulatorFile("[Input] shell: " + m_keyShellBuf.left(300).trimmed());
            m_keyShellBuf = m_keyShellBuf.right(kMarker.size());   // хвост может быть началом маркера
        }
    });
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        logToEmulatorFile("[Input] key shell finished, code " + QString::number(code));
        if (m_keyShell == proc) {
            m_keyShell = nullptr;
            m_keyBatchInFlight = false;
            m_keyShellBuf.clear();
        }
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        logToEmulatorFile("[Input] key shell failed to start: " + proc->errorString());
        if (m_keyShell == proc) {
            m_keyShell = nullptr;
            m_keyBatchInFlight = false;
            m_keyShellBuf.clear();
        }
        proc->deleteLater();
    });

    // Запись до фактического старта буферизуется QProcess и уйдёт после запуска
    proc->start();
    logToEmulatorFile("[Input] key shell started");
}

void AndroidTVModule::stopKeyShell()
{
    resetEvdev();
    if (m_grpcProbe) {
        QProcess *proc = m_grpcProbe;
        m_grpcProbe = nullptr;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) proc->kill();
    }
    m_grpcState = GrpcState::Off;
    m_grpcVerified = false;
    m_grpcProbeKeys.clear();
    m_grpcWaiting.clear();
    m_pendingKeys.clear();
    m_keyBatchInFlight = false;
    m_keyShellBuf.clear();
    if (!m_keyShell) return;
    QProcess *proc = m_keyShell;
    m_keyShell = nullptr;
    proc->disconnect(this);
    if (proc->state() == QProcess::NotRunning) { proc->deleteLater(); return; }
    proc->write("exit\n");
    proc->closeWriteChannel();
    if (m_shutdownDone) {
        // Выход из программы — ждём здесь, событийного цикла дальше не будет
        if (!proc->waitForFinished(500)) {
            proc->kill();
            proc->waitForFinished(1000);
        }
        proc->deleteLater();
        return;
    }
    // Выход из ТВ — без ожидания в GUI-потоке (раньше до 1,5 с «замирания»):
    // процесс удалится сам по завершении, не завершится за 1 с — убьём
    connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    QTimer::singleShot(1000, proc, [proc]() {
        if (proc->state() != QProcess::NotRunning) proc->kill();
    });
}

void AndroidTVModule::sendAndroidKey(int keyCode)
{
    // 0. gRPC эмулятора
    if (m_grpcState == GrpcState::Checking) {
        if (m_grpcWaiting.size() < kMaxPendingKeys) m_grpcWaiting.append(keyCode);
        return;
    }
    if (m_grpcState == GrpcState::Ready && sendKeyViaGrpc(keyCode))
        return;

    // Консоль уже проверялась с этой сборкой эмулятора и не работает — сразу adb
    if (m_consoleInput && m_consoleState != ConsoleState::Failed && consoleKnownBroken()) {
        m_consoleState = ConsoleState::Failed;
        logToEmulatorFile("[Input] emulator console: known not to deliver events with this "
                          "emulator build — using adb");
    }
    // 1. sendevent — если нашлось устройство клавиатуры с правом записи
    if (m_evdevState == EvdevState::Unknown) startEvdevProbe();
    if (m_evdevState == EvdevState::Probing) {
        if (m_evdevWaiting.size() < kMaxPendingKeys) m_evdevWaiting.append(keyCode);
        return;
    }
    if (m_evdevState == EvdevState::Ready) {
        const int linuxKey = evdevLinuxKey(keyCode);
        if (linuxKey > 0) { queueEvdevKey(linuxKey); return; }
    }
    // 2. консоль эмулятора (с самопроверкой), 3. adb input keyevent
    if (m_consoleInput && sendKeyViaConsole(keyCode))
        return;
    queueAdbKey(keyCode);
}

// ── gRPC эмулятора ───────────────────────────────────────────────────────────

/*static*/ int AndroidTVModule::grpcEvdevKey(int androidKeyCode)
{
    // Коды evdev, которые эмулятор переводит в клавиши Android (Generic.kl).
    // OK — KEY_ENTER: для Android это «подтверждение» наравне с DPAD_CENTER, а
    // KEY_REPLY может отсутствовать в таблице перевода эмулятора.
    switch (androidKeyCode) {
    case 19: return 103;   // DPAD_UP    ← KEY_UP
    case 20: return 108;   // DPAD_DOWN  ← KEY_DOWN
    case 21: return 105;   // DPAD_LEFT  ← KEY_LEFT
    case 22: return 106;   // DPAD_RIGHT ← KEY_RIGHT
    case 23: return 28;    // DPAD_CENTER← KEY_ENTER
    case 4:  return 158;   // BACK       ← KEY_BACK
    case 3:  return 172;   // HOME       ← KEY_HOMEPAGE
    default: return -1;    // Меню, Поиск… — старым путём
    }
}

int AndroidTVModule::pickFreeGrpcPort() const
{
    // 8554 — порт gRPC эмулятора по умолчанию; со сдвигом как у консоли
    for (int port = 8554 + (m_consolePort - 5554); port < 8654; ++port) {
        QTcpServer t;
        if (t.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) return port;
    }
    return 0;
}

bool AndroidTVModule::grpcDisabled() const
{
    const QString key = emulatorBuildKey();
    return !key.isEmpty()
        && LocalModels::appSettings().value("androidtv/grpcUnsupportedFor").toString() == key;
}

bool AndroidTVModule::grpcKnownBroken() const
{
    const QString key = emulatorBuildKey();
    return !key.isEmpty()
        && LocalModels::appSettings().value("androidtv/grpcInputBrokenFor").toString() == key;
}

void AndroidTVModule::rememberGrpcBroken()
{
    const QString key = emulatorBuildKey();
    if (key.isEmpty()) return;
    QSettings st = LocalModels::appSettings();
    st.setValue("androidtv/grpcInputBrokenFor", key);
}

void AndroidTVModule::startGrpcCheck()
{
    if (m_grpcPort <= 0 || m_grpcState != GrpcState::Off) return;
    if (grpcKnownBroken()) {
        m_grpcState = GrpcState::Failed;
        logToEmulatorFile("[Input] gRPC: known not to deliver keys with this emulator build");
        return;
    }
    if (!m_grpc) {
        m_grpc = new EmulatorGrpcClient(this);
        connect(m_grpc, &EmulatorGrpcClient::callFailed, this, [this](const QString &method, const QString &err) {
            if (m_grpcState == GrpcState::Ready)
                failGrpc(method + ": " + err);
        });
    }
    QString tokenSource;
    const QByteArray token = readGrpcToken(&tokenSource);
    logToEmulatorFile("[Input] gRPC token: " + tokenSource);
    m_grpc->configure(m_grpcPort, token);
    m_grpcState = GrpcState::Checking;
    m_grpcStatusTries = 0;

    auto attempt = std::make_shared<std::function<void()>>();
    *attempt = [this, attempt]() {
        if (m_grpcState != GrpcState::Checking) return;
        ++m_grpcStatusTries;
        m_grpc->getStatus([this, attempt](const EmulatorGrpcClient::Status &st) {
            if (m_grpcState != GrpcState::Checking) return;
            if (st.ok) {
                m_grpcState = GrpcState::Ready;
                logToEmulatorFile(QString("[Input] gRPC :%1 ready (emulator %2, booted=%3)")
                                      .arg(m_grpcPort).arg(st.version).arg(st.booted));
            } else if (m_grpcStatusTries < kGrpcStatusRetries) {
                QTimer::singleShot(400, this, [attempt]() { (*attempt)(); });
                return;
            } else {
                m_grpcState = GrpcState::Failed;
                logToEmulatorFile("[Input] gRPC unavailable (" + st.error + ") — using other input paths");
            }
            const QList<int> waiting = m_grpcWaiting;
            m_grpcWaiting.clear();
            for (int k : waiting) sendAndroidKey(k);
        });
    };
    (*attempt)();
}

bool AndroidTVModule::sendKeyViaGrpc(int androidKeyCode)
{
    const int code = grpcEvdevKey(androidKeyCode);
    if (code < 0 || !m_grpc) return false;
    if (!m_grpcVerified && !m_grpcProbe) {
        startGrpcProbe({androidKeyCode});
        return true;
    }
    if (m_grpcProbe) m_grpcProbeKeys.append(androidKeyCode);
    m_grpc->sendKeyPress(code);
    return true;
}

// Как у консоли: «OK» от эмулятора ещё не значит, что нажатие дошло до
// Android. Слушаем `getevent -lq`, отправляем, ждём EV_KEY.
void AndroidTVModule::startGrpcProbe(const QList<int> &androidKeys)
{
    m_grpcProbeSawKey = false;
    m_grpcProbeKeys.clear();
    m_grpcProbe = new QProcess(this);
    QProcess *proc = m_grpcProbe;
    proc->setProgram(adbPath());
    proc->setArguments({"-s", adbSerial(), "shell", "getevent", "-lq"});
    proc->setProcessEnvironment(toolsEnvironment());
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyRead, this, [this, proc]() {
        if (m_grpcProbe != proc) return;
        if (proc->readAll().contains("EV_KEY")) {
            m_grpcProbeSawKey = true;
            finishGrpcProbe(true);
        }
    });
    connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    proc->start();
    logToEmulatorFile("[Input] gRPC probe: listening with getevent");

    QTimer::singleShot(300, this, [this, androidKeys]() {
        if (m_grpcState != GrpcState::Ready || !m_grpcProbe) return;
        for (int k : androidKeys) sendKeyViaGrpc(k);
    });
    QTimer::singleShot(300 + kGrpcProbeMs, this, [this, proc]() {
        if (m_grpcProbe == proc) finishGrpcProbe(m_grpcProbeSawKey);
    });
}

void AndroidTVModule::finishGrpcProbe(bool sawKey)
{
    QProcess *proc = m_grpcProbe;
    m_grpcProbe = nullptr;
    if (proc && proc->state() != QProcess::NotRunning) proc->kill();
    const QList<int> sent = m_grpcProbeKeys;
    m_grpcProbeKeys.clear();
    if (sawKey) {
        m_grpcVerified = true;
        logToEmulatorFile("[Input] gRPC probe: OK, keys reach Android — fast input (no JVM)");
        return;
    }
    rememberGrpcBroken();
    failGrpc(QStringLiteral("keys do not reach Android (getevent saw nothing)"), sent);
}

void AndroidTVModule::failGrpc(const QString &reason, const QList<int> &resend)
{
    if (m_grpcState == GrpcState::Failed) return;
    m_grpcState = GrpcState::Failed;
    logToEmulatorFile("[Input] gRPC input disabled (" + reason + ") — using other input paths");
    for (int k : resend) sendAndroidKey(k);
}

// ── Память о неработающей консоли ────────────────────────────────────────────

QString AndroidTVModule::emulatorBuildKey() const
{
    const QFileInfo fi(emulatorPath());
    if (!fi.exists()) return {};
    return QString::number(fi.size()) + "_" + QString::number(fi.lastModified().toSecsSinceEpoch());
}

bool AndroidTVModule::consoleKnownBroken() const
{
    const QString key = emulatorBuildKey();
    return !key.isEmpty()
        && LocalModels::appSettings().value("androidtv/consoleInputBrokenFor").toString() == key;
}

void AndroidTVModule::rememberConsoleBroken()
{
    const QString key = emulatorBuildKey();
    if (key.isEmpty()) return;
    QSettings st = LocalModels::appSettings();
    st.setValue("androidtv/consoleInputBrokenFor", key);
}

// ── sendevent ────────────────────────────────────────────────────────────────

int AndroidTVModule::evdevLinuxKey(int androidKeyCode) const
{
    auto has = [this](int k) { return m_evdevKeys.contains(k); };
    switch (androidKeyCode) {
    case 19: return has(103) ? 103 : -1;                  // DPAD_UP    ← KEY_UP
    case 20: return has(108) ? 108 : -1;                  // DPAD_DOWN  ← KEY_DOWN
    case 21: return has(105) ? 105 : -1;                  // DPAD_LEFT  ← KEY_LEFT
    case 22: return has(106) ? 106 : -1;                  // DPAD_RIGHT ← KEY_RIGHT
    case 23: return has(232) ? 232 : has(28) ? 28 : -1;   // DPAD_CENTER ← KEY_REPLY / ENTER
    case 4:  return has(158) ? 158 : -1;                  // BACK       ← KEY_BACK
    default: return -1;                                   // Home, Menu… — через input keyevent
    }
}

/*static*/ QString AndroidTVModule::pickEvdevDevice(const QString &out, QSet<int> *keys, QString *name,
                                                    bool *viaSu)
{
    bool suOk = false;
    static const QHash<QString, int> kNeeded = {
        { "KEY_UP", 103 }, { "KEY_DOWN", 108 }, { "KEY_LEFT", 105 }, { "KEY_RIGHT", 106 },
        { "KEY_ENTER", 28 }, { "KEY_REPLY", 232 }, { "KEY_BACK", 158 },
    };
    struct Dev { QString path, name; QSet<int> keys; };
    QList<Dev> devs;
    QSet<QString> writable;
    bool inKeys = false;
    static const QRegularExpression rxAdd(QStringLiteral("^add device \\d+:\\s*(\\S+)"));
    static const QRegularExpression rxSection(QStringLiteral("^\\s*[A-Z_]+ \\([0-9a-fA-F]{4}\\):"));
    static const QRegularExpression rxName(QStringLiteral("^\\s*name:\\s*\"(.*)\""));

    for (const QString &raw : out.split(QLatin1Char('\n'))) {
        const QString line = QString(raw).remove(QLatin1Char('\r'));
        if (line.startsWith(QLatin1String("WRITABLE "))) { writable.insert(line.mid(9).trimmed()); continue; }
        if (line.trimmed() == QLatin1String("SU_OK")) { suOk = true; continue; }
        const auto mAdd = rxAdd.match(line);
        if (mAdd.hasMatch()) { devs.append({ mAdd.captured(1), {}, {} }); inKeys = false; continue; }
        if (devs.isEmpty()) continue;
        const auto mName = rxName.match(line);
        if (mName.hasMatch()) { devs.last().name = mName.captured(1); continue; }
        if (rxSection.match(line).hasMatch())
            inKeys = line.trimmed().startsWith(QLatin1String("KEY ("));
        else if (line.trimmed().startsWith(QLatin1String("input props")))
            inKeys = false;
        if (!inKeys) continue;
        for (const QString &tok : line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
            const auto it = kNeeded.constFind(tok);
            if (it != kNeeded.constEnd()) devs.last().keys.insert(it.value());
        }
    }

    // Нужны стрелки и право записи (своё или через su); из подходящих — где
    // больше нужных клавиш, при равенстве — доступное без su
    const Dev *best = nullptr;
    bool bestSu = false;
    for (const Dev &d : devs) {
        const bool direct = writable.contains(d.path);
        if (!direct && !suOk) continue;
        if (!d.keys.contains(103) || !d.keys.contains(108)) continue;
        const bool better = !best || d.keys.size() > best->keys.size()
                            || (d.keys.size() == best->keys.size() && direct && bestSu);
        if (better) { best = &d; bestSu = !direct; }
    }
    if (!best) return {};
    if (keys)  *keys  = best->keys;
    if (name)  *name  = best->name;
    if (viaSu) *viaSu = bestSu;
    return best->path;
}

void AndroidTVModule::startEvdevProbe()
{
    if (m_evdevState != EvdevState::Unknown || !m_running) return;
    if (!QFile::exists(adbPath())) { m_evdevState = EvdevState::Unavailable; return; }
    m_evdevState = EvdevState::Probing;

    auto *proc = new QProcess(this);
    m_evdevProbe = proc;
    proc->setProgram(adbPath());
    // Доступен ли su (образы без Google Play), устройства с клавишами
    // (через su — если без него getevent не видит устройства) и какие из них
    // доступны на запись пользователю shell
    proc->setArguments({"-s", adbSerial(), "shell",
                        "if su 0 id -u 2>/dev/null | grep -qx 0; then echo SU_OK; "
                        "su 0 getevent -pl 2>/dev/null; else getevent -pl 2>/dev/null; fi; "
                        "for f in /dev/input/event*; do [ -w $f ] && echo WRITABLE $f; done"});
    proc->setProcessEnvironment(toolsEnvironment());
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::finished, this, [this, proc](int, QProcess::ExitStatus) {
        if (m_evdevProbe != proc) { proc->deleteLater(); return; }
        m_evdevProbe = nullptr;
        onEvdevProbeOutput(QString::fromUtf8(proc->readAll()));
        proc->deleteLater();
    });
    QTimer::singleShot(kEvdevProbeTimeoutMs, proc, [this, proc]() {
        if (m_evdevProbe != proc) return;
        m_evdevProbe = nullptr;
        proc->disconnect(this);
        proc->kill();
        proc->deleteLater();
        onEvdevProbeOutput(QString());
    });
    proc->start();
    logToEmulatorFile("[Input] evdev: looking for a writable keyboard device");
}

void AndroidTVModule::onEvdevProbeOutput(const QString &out)
{
    QString name;
    m_evdevViaSu = false;
    m_evdevPath = pickEvdevDevice(out, &m_evdevKeys, &name, &m_evdevViaSu);
    if (m_evdevPath.isEmpty()) {
        m_evdevState = EvdevState::Unavailable;
        m_evdevKeys.clear();
        if (out.isEmpty()) {
            logToEmulatorFile(QStringLiteral("[Input] evdev: probe timed out — using console/adb"));
        } else {
            // Подробности — чтобы по журналу было видно, чего не хватило
            const int devices  = int(out.count(QLatin1String("add device")));
            const int writable = int(out.count(QLatin1String("WRITABLE ")));
            const bool su      = out.contains(QLatin1String("SU_OK"));
            logToEmulatorFile(QString("[Input] evdev: no usable keyboard device (devices seen %1, "
                                      "writable %2, su %3) — using console/adb")
                                  .arg(devices).arg(writable).arg(su ? "yes" : "no"));
        }
    } else {
        m_evdevState = EvdevState::Ready;
        QStringList k;
        for (int code : std::as_const(m_evdevKeys)) k << QString::number(code);
        std::sort(k.begin(), k.end());
        logToEmulatorFile(QString("[Input] evdev: %1 '%2', keys %3 — sendevent%4 (no JVM)")
                              .arg(m_evdevPath, name, k.join(','),
                                   m_evdevViaSu ? " via su" : ""));
    }
    const QList<int> waiting = m_evdevWaiting;
    m_evdevWaiting.clear();
    for (int key : waiting) sendAndroidKey(key);
}

void AndroidTVModule::queueEvdevKey(int linuxKey)
{
    if (m_pendingEvdev.size() >= kMaxPendingKeys) m_pendingEvdev.removeFirst();
    m_pendingEvdev.append(linuxKey);
    flushPendingKeys();
}

void AndroidTVModule::resetEvdev()
{
    if (m_evdevProbe) {
        QProcess *proc = m_evdevProbe;
        m_evdevProbe = nullptr;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) proc->kill();
        proc->deleteLater();
    }
    m_evdevState = EvdevState::Unknown;
    m_evdevViaSu = false;
    m_evdevPath.clear();
    m_evdevKeys.clear();
    m_evdevWaiting.clear();
    m_pendingEvdev.clear();
}

void AndroidTVModule::queueAdbKey(int keyCode)
{
    logToEmulatorFile(QString("[Input] keyevent %1").arg(keyCode));

    if (m_pendingKeys.size() >= kMaxPendingKeys) {
        // Удержание кнопки быстрее, чем Android успевает обрабатывать:
        // отбрасываем самые старые нажатия, а не копим «хвост».
        m_pendingKeys.removeFirst();
    }
    m_pendingKeys.append(keyCode);
    flushPendingKeys();
}

void AndroidTVModule::flushPendingKeys()
{
    if (m_pendingKeys.isEmpty() && m_pendingEvdev.isEmpty()) return;

    if (m_keyBatchInFlight) {
        if (m_keyBatchSent.isValid() && m_keyBatchSent.elapsed() < kKeyBatchStuckMs)
            return;                         // предыдущая команда ещё выполняется
        logToEmulatorFile("[Input] previous key batch did not confirm, sending anyway");
        m_keyBatchInFlight = false;
    }

    QStringList codes;
    for (int k : std::as_const(m_pendingKeys))
        codes << QString::number(k);
    m_pendingKeys.clear();

    // sendevent: нажатие + синхронизация, отпускание + синхронизация
    QByteArray evdevCmd;
    const QByteArray dev = m_evdevPath.toLatin1();
    for (int k : std::as_const(m_pendingEvdev)) {
        const QByteArray code = QByteArray::number(k);
        evdevCmd += "sendevent " + dev + " 1 " + code + " 1;sendevent " + dev + " 0 0 0;"
                    "sendevent " + dev + " 1 " + code + " 0;sendevent " + dev + " 0 0 0;";
    }
    m_pendingEvdev.clear();

    // Через su — одна оболочка root на всю пачку (su — нативная утилита, не JVM)
    QByteArray cmd = (!evdevCmd.isEmpty() && m_evdevViaSu)
                         ? "su 0 sh -c '" + evdevCmd + "';"
                         : evdevCmd;
    if (!codes.isEmpty())
        cmd += "input keyevent " + codes.join(QLatin1Char(' ')).toLatin1() + ";";

    if (!m_keyShell) startKeyShell();
    if (m_keyShell && m_keyShell->state() != QProcess::NotRunning) {
        // Одна команда на пачку; маркер сообщает, что она выполнена и можно
        // отправлять следующую. Кавычки разбивают маркер в тексте команды (на
        // случай эха терминала), а в выводе echo он склеивается целиком.
        m_keyShell->write(cmd + " echo __MS_KEYS_\"\"DONE__\n");
        m_keyBatchInFlight = true;
        m_keyBatchSent.start();
        return;
    }
    // Запасной путь — разовый вызов
    adb(QStringList{"shell", QString::fromLatin1(cmd)});
}

// ── Консоль эмулятора ────────────────────────────────────────────────────────
// `input keyevent` в Android — это запуск app_process (JVM): 200–500 мс CPU
// гостя на каждую пачку нажатий, отсюда рывки звука при листании.
// Консоль эмулятора (TCP 127.0.0.1:<consolePort>) принимает
// `event send EV_KEY:<код>:1 EV_KEY:<код>:0` и кладёт событие прямо в
// виртуальную клавиатуру — как будто нажали клавишу в окне эмулятора.
//
// Через консоль идут только частые клавиши (стрелки, OK, Назад) с
// однозначной раскладкой (Generic.kl / qwerty.kl). Остальные — старым путём.
// Любой сбой консоли (нет соединения, неверный токен, KO на команды) —
// переключение на adb до следующего запуска ТВ, с записью в журнал.

/*static*/ int AndroidTVModule::androidKeyToLinuxKey(int androidKeyCode)
{
    switch (androidKeyCode) {
    case 19: return 103;   // DPAD_UP     ← KEY_UP
    case 20: return 108;   // DPAD_DOWN   ← KEY_DOWN
    case 21: return 105;   // DPAD_LEFT   ← KEY_LEFT
    case 22: return 106;   // DPAD_RIGHT  ← KEY_RIGHT
    case 23: return 232;   // DPAD_CENTER ← KEY_REPLY (в Generic.kl и qwerty.kl = DPAD_CENTER)
    case 4:  return 158;   // BACK        ← KEY_BACK
    default: return -1;
    }
}

// Токен лежит в %USERPROFILE%\.emulator_console_auth_token (эмулятор создаёт
// его сам при первом запуске). Пустой файл = авторизация выключена.
/*static*/ QByteArray AndroidTVModule::readConsoleAuthToken()
{
    QFile f(QDir::homePath() + QStringLiteral("/.emulator_console_auth_token"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll().trimmed();
}

// pid_<PID>.ini: строки «ключ=значение»; берём grpc.token, если grpc.port
// совпадает с нашим (port <= 0 — не сверять)
/*static*/ QByteArray AndroidTVModule::grpcTokenFromDiscovery(const QByteArray &ini, int port)
{
    QByteArray token;
    int filePort = -1;
    for (QByteArray line : ini.split('\n')) {
        line = line.trimmed();
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QByteArray k = line.left(eq).trimmed(), v = line.mid(eq + 1).trimmed();
        if (k == "grpc.token")     token = v;
        else if (k == "grpc.port") filePort = v.toInt();
    }
    if (port > 0 && filePort > 0 && filePort != port) return {};
    return token;
}

QByteArray AndroidTVModule::readGrpcToken(QString *source) const
{
    QStringList candidates;
    if (!m_emuDiscoveryFile.isEmpty()) candidates << m_emuDiscoveryFile;
    const QString runDir = QDir::tempPath() + QStringLiteral("/avd/running");
    if (m_emulatorPid > 0) candidates << runDir + QString("/pid_%1.ini").arg(m_emulatorPid);
    // Запасной путь: любой файл обнаружения с нашим портом gRPC
    const QFileInfoList all = QDir(runDir).entryInfoList({ QStringLiteral("pid_*.ini") }, QDir::Files, QDir::Time);
    for (const QFileInfo &fi : all) candidates << fi.absoluteFilePath();
    candidates.removeDuplicates();

    for (const QString &path : candidates) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray tok = grpcTokenFromDiscovery(f.readAll(), m_grpcPort);
        if (!tok.isEmpty()) {
            if (source) *source = "grpc.token from " + QDir::toNativeSeparators(path);
            return tok;
        }
    }
    if (source) *source = QStringLiteral("no grpc.token in discovery files — using console auth token");
    return readConsoleAuthToken();
}

bool AndroidTVModule::sendKeyViaConsole(int androidKeyCode)
{
    if (m_consoleState == ConsoleState::Failed) return false;
    const int linuxKey = androidKeyToLinuxKey(androidKeyCode);
    if (linuxKey < 0) return false;

    if (m_consoleState != ConsoleState::Ready) {
        if (m_consolePending.size() >= kMaxPendingKeys)
            m_consolePending.removeFirst();
        m_consolePending.append(androidKeyCode);
        if (m_consoleState == ConsoleState::Off) startConsole();
        return true;
    }

    // Первое нажатие в готовую, но не проверенную консоль — через проверку
    // (startConsoleProbe сам отправит его, когда getevent будет слушать)
    if (!m_consoleVerified && !m_consoleProbe) {
        startConsoleProbe({androidKeyCode});
        return true;
    }

    // Символьные имена клавиш. EV_SYN эта версия эмулятора не принимает
    // («invalid event code» — журнал 07.11), синхронизацию добавляет сама.
    const QByteArray name = linuxKeyName(linuxKey);
    m_console->write("event send EV_KEY:" + name + ":1 EV_KEY:" + name + ":0\n");
    if (m_consoleProbe) m_consoleProbeKeys.append(androidKeyCode);
    return true;
}

/*static*/ QByteArray AndroidTVModule::linuxKeyName(int linuxKey)
{
    switch (linuxKey) {
    case 103: return "KEY_UP";
    case 108: return "KEY_DOWN";
    case 105: return "KEY_LEFT";
    case 106: return "KEY_RIGHT";
    case 232: return "KEY_REPLY";
    case 158: return "KEY_BACK";
    default:  return QByteArray::number(linuxKey);
    }
}

// Проверка: `getevent -lq` в госте печатает события устройств ввода. Запускаем
// его, через 300 мс (успеть открыть устройства) отправляем накопленные
// нажатия через консоль и ждём строку с EV_KEY не дольше kConsoleProbeMs.
void AndroidTVModule::startConsoleProbe(const QList<int> &keys)
{
    m_consoleProbeSawKey = false;
    m_consoleProbeKeys.clear();
    m_consoleProbe = new QProcess(this);
    QProcess *proc = m_consoleProbe;
    proc->setProgram(adbPath());
    proc->setArguments({"-s", adbSerial(), "shell", "getevent", "-lq"});
    proc->setProcessEnvironment(toolsEnvironment());
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyRead, this, [this, proc]() {
        if (m_consoleProbe != proc) return;
        const QByteArray out = proc->readAll();
        if (out.contains("EV_KEY")) {
            m_consoleProbeSawKey = true;
            finishConsoleProbe(true);
        }
    });
    connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    proc->start();
    logToEmulatorFile("[Input] console probe: listening with getevent");

    QTimer::singleShot(300, this, [this, keys]() {
        if (m_consoleState != ConsoleState::Ready) return;
        for (int k : keys) sendKeyViaConsole(k);
    });
    QTimer::singleShot(300 + kConsoleProbeMs, this, [this, proc]() {
        if (m_consoleProbe == proc) finishConsoleProbe(m_consoleProbeSawKey);
    });
}

void AndroidTVModule::finishConsoleProbe(bool sawKey)
{
    QProcess *proc = m_consoleProbe;
    m_consoleProbe = nullptr;
    if (proc && proc->state() != QProcess::NotRunning) proc->kill();
    const QList<int> sent = m_consoleProbeKeys;
    m_consoleProbeKeys.clear();

    if (sawKey) {
        m_consoleVerified = true;
        logToEmulatorFile("[Input] console probe: OK, events reach Android");
        return;
    }
    // Событий нет — консоль принимает команды, но они не доходят. Дальше —
    // adb, отправленное во время проверки повторяем через него. Запоминаем
    // для этой сборки эмулятора.
    rememberConsoleBroken();
    failConsole(QStringLiteral("events do not reach Android (getevent saw nothing)"));
    for (int k : sent) queueAdbKey(k);
}

void AndroidTVModule::startConsole()
{
    // stopConsole() очищает очередь — а в ней уже лежит нажатие, ради
    // которого консоль запускается. Раньше первое нажатие терялось всегда.
    const QList<int> keep = m_consolePending;
    stopConsole();
    m_consolePending = keep;
    m_consoleState = ConsoleState::Connecting;
    m_consoleBuf.clear();
    m_consoleAuthNeeded  = false;
    m_consoleKoCount     = 0;
    m_consoleReadyLogged = false;

    m_console = new QTcpSocket(this);
    QTcpSocket *sock = m_console;

    connect(sock, &QTcpSocket::connected, this, [this, sock]() {
        if (m_console != sock) return;
        m_consoleState = ConsoleState::Banner;
    });
    connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
        if (m_console == sock) onConsoleReadyRead();
    });
    connect(sock, &QTcpSocket::errorOccurred, this, [this, sock](QAbstractSocket::SocketError) {
        if (m_console != sock) return;
        if (m_consoleState == ConsoleState::Ready
            && sock->error() == QAbstractSocket::RemoteHostClosedError) {
            // Эмулятор закрыл соединение (перезапуск) — переподключимся
            // при следующем нажатии.
            logToEmulatorFile("[Input] console closed by emulator");
            m_console = nullptr;
            m_consoleState = ConsoleState::Off;
            sock->deleteLater();
            return;
        }
        failConsole(QStringLiteral("socket error: ") + sock->errorString());
    });

    // Соединение и рукопожатие — не дольше 3 с, иначе работаем через adb
    QTimer::singleShot(3000, sock, [this, sock]() {
        if (m_console == sock && m_consoleState != ConsoleState::Ready)
            failConsole(QStringLiteral("handshake timeout"));
    });

    logToEmulatorFile(QString("[Input] connecting to emulator console :%1").arg(m_consolePort));
    sock->connectToHost(QHostAddress::LocalHost, quint16(m_consolePort));
}

void AndroidTVModule::onConsoleReadyRead()
{
    m_consoleBuf += m_console->readAll();

    // Протокол: строки текста; каждый ответ заканчивается строкой «OK» или
    // «KO: …». Приветствие тоже заканчивается «OK».
    for (;;) {
        const int nl = m_consoleBuf.indexOf('\n');
        if (nl < 0) break;
        const QByteArray line = m_consoleBuf.left(nl).trimmed();
        m_consoleBuf.remove(0, nl + 1);
        if (line.isEmpty()) continue;

        const bool ok = (line == "OK");
        const bool ko = line.startsWith("KO");

        switch (m_consoleState) {
        case ConsoleState::Banner:
            if (line.contains("Authentication required")) {
                m_consoleAuthNeeded = true;
            } else if (ok && m_consoleAuthNeeded) {
                const QByteArray token = readConsoleAuthToken();
                if (token.isEmpty()) {
                    failConsole(QStringLiteral("auth token not found in %1").arg(QDir::homePath()));
                    return;
                }
                m_console->write("auth " + token + "\n");
                m_consoleState = ConsoleState::Auth;
            } else if (ok) {
                m_consoleState = ConsoleState::Ready;
            }
            break;
        case ConsoleState::Auth:
            if (ok) {
                m_consoleState = ConsoleState::Ready;
            } else if (ko) {
                failConsole(QStringLiteral("auth rejected: ") + QString::fromUtf8(line));
                return;
            }
            break;
        case ConsoleState::Ready:
            if (ko) {
                logToEmulatorFile("[Input] console: " + QString::fromUtf8(line.left(200)));
                // Несколько отказов подряд — эта версия эмулятора не принимает
                // события так: дальше через adb.
                if (++m_consoleKoCount >= 3) {
                    failConsole(QStringLiteral("event send rejected"));
                    return;
                }
            } else if (ok) {
                m_consoleKoCount = 0;
            }
            break;
        default:
            break;
        }

        if (m_consoleState == ConsoleState::Ready && !m_consoleReadyLogged) {
            m_consoleReadyLogged = true;
            logToEmulatorFile("[Input] emulator console ready");
        }
        if (m_consoleState == ConsoleState::Ready && !m_consolePending.isEmpty()) {
            const QList<int> pending = m_consolePending;
            m_consolePending.clear();
            if (!m_consoleVerified && !m_consoleProbe) {
                startConsoleProbe(pending);     // первые нажатия — с проверкой
                return;
            }
            for (int k : pending) sendKeyViaConsole(k);
        }
    }
}

void AndroidTVModule::failConsole(const QString &reason)
{
    logToEmulatorFile("[Input] emulator console unavailable (" + reason
                      + "), falling back to adb input keyevent");
    if (m_console) {
        QTcpSocket *sock = m_console;
        m_console = nullptr;
        sock->disconnect(this);
        sock->abort();
        sock->deleteLater();
    }
    m_consoleState = ConsoleState::Failed;
    const QList<int> pending = m_consolePending;
    m_consolePending.clear();
    for (int k : pending) queueAdbKey(k);
}

void AndroidTVModule::stopConsole()
{
    if (m_consoleProbe) {
        QProcess *proc = m_consoleProbe;
        m_consoleProbe = nullptr;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) proc->kill();
    }
    m_consoleProbeKeys.clear();
    m_consoleVerified = false;
    m_consolePending.clear();
    m_consoleBuf.clear();
    m_consoleState = ConsoleState::Off;
    if (!m_console) return;
    QTcpSocket *sock = m_console;
    m_console = nullptr;
    sock->disconnect(this);
    sock->abort();
    sock->deleteLater();
}

// ── Уведомление G72KeysModule о смене режима ─────────────────────────────────

void AndroidTVModule::notifyKeyMode(bool emulatorActive)
{
    // Устройство для sendevent ищем сразу при показе ТВ — к первому нажатию
    // всё готово
    if (emulatorActive) { startGrpcCheck(); startEvdevProbe(); }
    if (!m_bus) return;
    emit m_bus->moduleEvent(
        QLatin1String(G72Keys::kModule),
        QLatin1String(G72Keys::kEventMode),
        G72Keys::makeModeEvent(emulatorActive));
    logToEmulatorFile(
        QString("[Input] G72Keys mode → %1")
            .arg(emulatorActive ? "EmulatorMode" : "Normal"));
}

void AndroidTVModule::requestKeyHookReinstall()
{
    if (!m_bus) return;
    emit m_bus->moduleEvent(QLatin1String(G72Keys::kModule),
                            QLatin1String(G72Keys::kEventReinstallHook), QVariantMap{});
}

// ── Q_INVOKABLE для QML ───────────────────────────────────────────────────────

void AndroidTVModule::sendRemoteKey(const QString &key)
{
    handleRemoteKey(key);
}
