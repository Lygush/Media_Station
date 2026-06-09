// modules/G72Module/G72KeysModule.h
#pragma once

#include "core/IModule.h"
#include "core/EventBus.h"

#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QString>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace G72Keys {
constexpr auto kModule    = "g72keys";
constexpr auto kEventKey  = "keyPressed";
constexpr auto kEventMode = "modeChanged";

constexpr auto kHome      = "home";
constexpr auto kBack      = "back";
constexpr auto kSearch    = "search";
constexpr auto kMenu      = "menu";
constexpr auto kHomePage  = "homepage";
constexpr auto kFavorites = "favorites";
constexpr auto kUp        = "up";
constexpr auto kDown      = "down";
constexpr auto kLeft      = "left";
constexpr auto kRight     = "right";
constexpr auto kSelect    = "select";
constexpr auto kVolumeUp   = "volup";
constexpr auto kVolumeDown = "voldown";

inline QVariantMap makeKeyEvent(const QString &key) {
    return QVariantMap{{ QStringLiteral("key"), key }};
}
inline QVariantMap makeModeEvent(bool emulatorActive) {
    return QVariantMap{{ QStringLiteral("emulator"), emulatorActive }};
}
}

class G72KeysModule : public QObject, public IModule
{
    Q_OBJECT
    Q_PROPERTY(bool emulatorMode READ emulatorMode NOTIFY emulatorModeChanged)

public:
    explicit G72KeysModule(QObject *parent = nullptr);
    ~G72KeysModule() override;

    QString  moduleId()  const override { return QStringLiteral("g72keys"); }
    void     initialize(EventBus *bus, SettingsManager *) override;
    void     shutdown()  override;
    QObject *qmlObject() override       { return this; }
    QString  qmlName()   const override { return QStringLiteral("G72Keys"); }

    bool emulatorMode() const { return m_emulatorMode; }

signals:
    void homePressed();
    void backPressed();
    void searchPressed();
    void menuPressed();
    void upPressed();
    void downPressed();
    void leftPressed();
    void rightPressed();
    void selectPressed();
    void volumeUpPressed();
    void volumeDownPressed();
    void emulatorModeChanged(bool active);

private slots:
    void processKey(int vkCode, bool isDown);
    void processMouseAsSelect();
    void applyCursorState();

private:
    void dispatchKey(const char *keyName);
    static const char *vkToKeyName(int vkCode);

    // Возвращает true если ввод должен быть заблокирован для эмулятора:
    // оверлей открыт ИЛИ только что закрылся (cooldown 300 мс).
    inline bool isInputBlocked() const {
        return m_overlayOpen || m_overlayClosing;
    }

#ifdef Q_OS_WIN
    static LRESULT CALLBACK keyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    static HHOOK s_keyboardHook;
    static HHOOK s_mouseHook;
#endif

    static G72KeysModule *s_instance;

    bool      m_emulatorMode   = false;
    bool      m_overlayOpen    = false;
    bool      m_overlayClosing = false; // cooldown после закрытия оверлея
    bool      m_cursorHidden   = false;
    bool      m_shutdownDone   = false;
    EventBus *m_bus            = nullptr;
    QTimer   *m_overlayClosingTimer = nullptr;
};
