// modules/AndroidTVModule/AndroidTVInput.cpp
//
// Управление вводом:
//   • Обработка клавиш пульта G72 (handleRemoteKey)
//   • Маппинг ключей G72 → Android keycode
//   • Переключение режима клавиш (notifyKeyMode)
//   • Публичный Q_INVOKABLE sendRemoteKey для QML

#include "AndroidTVModule.h"
#include "LoggerModule.h"
#include "G72KeysModule.h"
#include "OverlayModule.h"
#include "core/EventBus.h"

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
        { G72Keys::kFavorites, 268 }, // KEYCODE_BOOKMARK
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
    if (!m_running) return;

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
        logToEmulatorFile(
            QString("[Input] Unknown remote key: %1").arg(key));
        return;
    }

    logToEmulatorFile(
        QString("[Input] Remote key '%1' → ADB keyevent %2")
            .arg(key).arg(keyCode));
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
