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
        { G72Keys::kVolumeUp,  24  }, // KEYCODE_VOLUME_UP
        { G72Keys::kVolumeDown,25  }, // KEYCODE_VOLUME_DOWN
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
        const QString out = QString::fromUtf8(proc->readAll()).trimmed();
        if (!out.isEmpty()) logToEmulatorFile("[Input] shell: " + out.left(300));
    });
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        logToEmulatorFile("[Input] key shell finished, code " + QString::number(code));
        if (m_keyShell == proc) m_keyShell = nullptr;
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        logToEmulatorFile("[Input] key shell failed to start: " + proc->errorString());
        if (m_keyShell == proc) m_keyShell = nullptr;
        proc->deleteLater();
    });

    // Запись до фактического старта буферизуется QProcess и уйдёт после запуска
    proc->start();
    logToEmulatorFile("[Input] key shell started");
}

void AndroidTVModule::stopKeyShell()
{
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

    if (!m_keyShell) startKeyShell();
    if (m_keyShell && m_keyShell->state() != QProcess::NotRunning) {
        m_keyShell->write(QByteArray("input keyevent ") + QByteArray::number(keyCode) + '\n');
        return;
    }
    // Запасной путь — разовый вызов
    adb({"shell", "input", "keyevent", QString::number(keyCode)});
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
