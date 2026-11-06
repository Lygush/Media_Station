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
#include "LoggerModule.h"
#include "G72KeysModule.h"
#include "OverlayModule.h"
#include "core/EventBus.h"

#include <QFile>
#include <QDir>
#include <QHostAddress>
#include <QTimer>

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
    m_pendingKeys.clear();
    m_keyBatchInFlight = false;
    m_keyShellBuf.clear();
    if (!m_keyShell) return;
    QProcess *proc = m_keyShell;
    m_keyShell = nullptr;
    proc->disconnect(this);
    if (proc->state() != QProcess::NotRunning) {
        proc->write("exit\n");
        proc->closeWriteChannel();
        if (!proc->waitForFinished(500)) {
            proc->kill();
            proc->waitForFinished(1000);
        }
    }
    proc->deleteLater();
}

void AndroidTVModule::sendAndroidKey(int keyCode)
{
    if (m_consoleInput && sendKeyViaConsole(keyCode))
        return;
    queueAdbKey(keyCode);
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
    if (m_pendingKeys.isEmpty()) return;

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

    if (!m_keyShell) startKeyShell();
    if (m_keyShell && m_keyShell->state() != QProcess::NotRunning) {
        // Несколько кодов в одной команде — одна JVM на всю пачку; маркер
        // сообщает, что команда выполнена и можно отправлять следующую.
        m_keyShell->write("input keyevent " + codes.join(QLatin1Char(' ')).toLatin1()
                          // кавычки разбивают маркер в тексте команды (на случай эха
                          // терминала), а в выводе echo он склеивается целиком
                          + "; echo __MS_KEYS_\"\"DONE__\n");
        m_keyBatchInFlight = true;
        m_keyBatchSent.start();
        return;
    }
    // Запасной путь — разовый вызов
    adb(QStringList{"shell", "input", "keyevent"} + codes);
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

    const QByteArray code = QByteArray::number(linuxKey);
    m_console->write("event send EV_KEY:" + code + ":1 EV_KEY:" + code + ":0\n");
    return true;
}

void AndroidTVModule::startConsole()
{
    stopConsole();
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
    if (!m_bus) return;
    emit m_bus->moduleEvent(
        QLatin1String(G72Keys::kModule),
        QLatin1String(G72Keys::kEventMode),
        G72Keys::makeModeEvent(emulatorActive));
    logToEmulatorFile(
        QString("[Input] G72Keys mode → %1")
            .arg(emulatorActive ? "EmulatorMode" : "Normal"));
}

// ── Q_INVOKABLE для QML ───────────────────────────────────────────────────────

void AndroidTVModule::sendRemoteKey(const QString &key)
{
    handleRemoteKey(key);
}
