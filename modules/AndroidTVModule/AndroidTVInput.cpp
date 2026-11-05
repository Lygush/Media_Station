// modules/AndroidTVModule/AndroidTVInput.cpp
//
// Управление вводом:
//   • Обработка клавиш пульта G72 (handleRemoteKey)
//   • Маппинг ключей G72 → Android keycode
//   • Отправка через постоянный `adb shell` (без запуска adb.exe на каждую клавишу)
//   • Переключение режима клавиш (notifyKeyMode)
//   • Публичный Q_INVOKABLE sendRemoteKey для QML

#include "AndroidTVModule.h"
#include "LoggerModule.h"
#include "G72KeysModule.h"
#include "OverlayModule.h"
#include "core/EventBus.h"

#include <QFile>

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
