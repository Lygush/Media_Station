// modules/G72Module/G72KeysModule.cpp
#include "G72KeysModule.h"
#include "core/EventBus.h"

#include <QMetaObject>
#include <QDebug>
#include <QGuiApplication>
#include <QCursor>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

#ifdef Q_OS_WIN
HHOOK G72KeysModule::s_keyboardHook = nullptr;
HHOOK G72KeysModule::s_mouseHook    = nullptr;
#endif
G72KeysModule *G72KeysModule::s_instance = nullptr;

static constexpr struct { int vk; const char *name; } kVkMap[] = {
    { VK_UP,     G72Keys::kUp        },
    { VK_DOWN,   G72Keys::kDown      },
    { VK_LEFT,   G72Keys::kLeft      },
    { VK_RIGHT,  G72Keys::kRight     },
    { VK_RETURN, G72Keys::kSelect    },
    { VK_APPS,   G72Keys::kMenu      },
    { 0xAC,      G72Keys::kHome      },
    { 0xAA,      G72Keys::kSearch    },
    { 0xA6,      G72Keys::kBack      },
    { 0xAB,      G72Keys::kFavorites },
    { 0xA7,      G72Keys::kHomePage  },
    { 0xAF,      G72Keys::kVolumeUp   },
    { 0xAE,      G72Keys::kVolumeDown },
    };
static constexpr int kVkMapSize = static_cast<int>(sizeof(kVkMap) / sizeof(kVkMap[0]));

static constexpr int kOverlayClosingCooldownMs = 300;

/*static*/ const char *G72KeysModule::vkToKeyName(int vkCode)
{
    for (int i = 0; i < kVkMapSize; ++i)
        if (kVkMap[i].vk == vkCode) return kVkMap[i].name;
    return nullptr;
}

G72KeysModule::G72KeysModule(QObject *parent) : QObject(parent) {}
G72KeysModule::~G72KeysModule() { shutdown(); }

void G72KeysModule::initialize(EventBus *bus, SettingsManager *)
{
    m_bus = bus;

    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data)
            {
                if (mod == QLatin1String(G72Keys::kModule) &&
                    ev  == QLatin1String(G72Keys::kEventMode))
                {
                    const bool active = data.value(QStringLiteral("emulator"), false).toBool();
                    if (m_emulatorMode == active) return;
                    m_emulatorMode = active;
                    emit emulatorModeChanged(active);
                    applyCursorState();
                    qDebug() << "[G72Keys] EmulatorMode:" << (active ? "ON" : "OFF");
                    return;
                }

                if (mod == QLatin1String("overlay") &&
                    ev  == QLatin1String("visibilityChanged"))
                {
                    const bool visible = data.value(QStringLiteral("visible"), false).toBool();
                    if (m_overlayOpen == visible) return;
                    m_overlayOpen = visible;

                    if (!visible) {
                        // Оверлей закрылся — запускаем cooldown.
                        m_overlayClosing = true;
                        if (m_overlayClosingTimer) {
                            m_overlayClosingTimer->stop();
                            m_overlayClosingTimer->deleteLater();
                        }
                        m_overlayClosingTimer = new QTimer(this);
                        m_overlayClosingTimer->setSingleShot(true);
                        m_overlayClosingTimer->setInterval(kOverlayClosingCooldownMs);
                        connect(m_overlayClosingTimer, &QTimer::timeout, this, [this]() {
                            m_overlayClosing = false;
                            m_overlayClosingTimer->deleteLater();
                            m_overlayClosingTimer = nullptr;
                            qDebug() << "[G72Keys] Overlay cooldown ended, input unblocked";
                        });
                        m_overlayClosingTimer->start();
                        qDebug() << "[G72Keys] OverlayOpen: NO (cooldown started)";
                    } else {
                        // Оверлей открылся — немедленно отменяем cooldown если был
                        if (m_overlayClosingTimer) {
                            m_overlayClosingTimer->stop();
                            m_overlayClosingTimer->deleteLater();
                            m_overlayClosingTimer = nullptr;
                        }
                        m_overlayClosing = false;
                        qDebug() << "[G72Keys] OverlayOpen: YES";
                    }

                    applyCursorState();
                }
            });

#ifdef Q_OS_WIN
    s_instance = this;

    s_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboardHookProc, nullptr, 0);
    if (!s_keyboardHook)
        qWarning() << "[G72Keys] WH_KEYBOARD_LL failed, error:" << GetLastError();
    else
        qDebug() << "[G72Keys] Keyboard hook installed";

    s_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseHookProc, nullptr, 0);
    if (!s_mouseHook)
        qWarning() << "[G72Keys] WH_MOUSE_LL failed, error:" << GetLastError();
    else
        qDebug() << "[G72Keys] Mouse hook installed";
#endif
}

void G72KeysModule::shutdown()
{
    if (m_shutdownDone) return;
    m_shutdownDone = true;

    if (m_overlayClosingTimer) {
        m_overlayClosingTimer->stop();
        m_overlayClosingTimer->deleteLater();
        m_overlayClosingTimer = nullptr;
    }

    if (m_cursorHidden) {
        m_cursorHidden = false;
        QGuiApplication::restoreOverrideCursor();
        qDebug() << "[G72Keys] Cursor restored on shutdown";
    }

#ifdef Q_OS_WIN
    if (s_keyboardHook) {
        UnhookWindowsHookEx(s_keyboardHook);
        s_keyboardHook = nullptr;
    }
    if (s_mouseHook) {
        UnhookWindowsHookEx(s_mouseHook);
        s_mouseHook = nullptr;
    }
    s_instance = nullptr;
#endif
}

// ── Курсор ────────────────────────────────────────────────────────────────────
// Используем Qt-уровневый механизм вместо Win32-хаков (SetSystemCursor,
// ShowCursor). QGuiApplication::setOverrideCursor(Qt::BlankCursor) работает
// через Qt's cursor stack — не трогает системные курсоры, не создаёт race
// condition с WM_SETCURSOR, корректно восстанавливается через restoreOverrideCursor.

void G72KeysModule::applyCursorState()
{
    const bool shouldHide = m_emulatorMode;
    if (shouldHide == m_cursorHidden) return;

    if (shouldHide) {
        // Кладём BlankCursor поверх текущего курсора Qt.
        // Если Qt уже имеет override cursor (кастомный курсор из main.cpp) —
        // наш BlankCursor ляжет поверх него и будет показан вместо него.
        QGuiApplication::setOverrideCursor(Qt::BlankCursor);
        m_cursorHidden = true;
        qDebug() << "[G72Keys] Cursor hidden (Qt::BlankCursor)";
    } else {
        // Снимаем наш BlankCursor — Qt автоматически вернёт предыдущий курсор
        // (кастомный из main.cpp или системный по умолчанию).
        QGuiApplication::restoreOverrideCursor();
        m_cursorHidden = false;
        qDebug() << "[G72Keys] Cursor restored (restoreOverrideCursor)";
    }
}

// ── Win32 хуки ───────────────────────────────────────────────────────────────

#ifdef Q_OS_WIN
/*static*/ LRESULT CALLBACK G72KeysModule::keyboardHookProc(
    int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0 || !s_instance)
        return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);

    const KBDLLHOOKSTRUCT *kb = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
    const int  vk     = static_cast<int>(kb->vkCode);
    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    const char *keyName = vkToKeyName(vk);
    if (!keyName)
        return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);

    if (isDown) {
        QMetaObject::invokeMethod(s_instance, "processKey",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, vk),
                                  Q_ARG(bool, true));
    }

    if (s_instance->isInputBlocked())
        return 1;

    if (s_instance->m_emulatorMode || vk == 0xAC || vk == 0xAA)
        return 1;

    return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);
}

/*static*/ LRESULT CALLBACK G72KeysModule::mouseHookProc(
    int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0 || !s_instance)
        return CallNextHookEx(s_mouseHook, nCode, wParam, lParam);

    if (s_instance->m_emulatorMode) {
        switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            if (wParam == WM_LBUTTONDOWN) {
                // Левый клик мыши в режиме airmouse = кнопка ОК.
                // Передаём как select даже когда оверлей открыт —
                // иначе клик на кнопках оверлея не проходит через dispatchKey.
                // Только m_overlayClosing (cooldown) исключаем: там ввод не нужен.
                if (!s_instance->m_overlayClosing) {
                    QMetaObject::invokeMethod(s_instance, "processMouseAsSelect",
                                              Qt::QueuedConnection);
                }
            }
            return 1;
        default:
            break;
        }
    }
    return CallNextHookEx(s_mouseHook, nCode, wParam, lParam);
}
#endif

// ── Обработка ввода ───────────────────────────────────────────────────────────

void G72KeysModule::processKey(int vkCode, bool isDown)
{
    if (!isDown) return;
    const char *keyName = vkToKeyName(vkCode);
    if (!keyName) return;
    dispatchKey(keyName);
}

void G72KeysModule::processMouseAsSelect()
{
    dispatchKey(G72Keys::kSelect);
}

void G72KeysModule::dispatchKey(const char *keyName)
{
    if (m_overlayOpen) {
        if (m_bus) {
            emit m_bus->moduleEvent(
                QLatin1String(G72Keys::kModule),
                QLatin1String(G72Keys::kEventKey),
                G72Keys::makeKeyEvent(QString::fromLatin1(keyName)));
        }
        return;
    }

    if (m_overlayClosing) {
        qDebug() << "[G72Keys] Key absorbed during overlay cooldown:" << keyName;
        return;
    }

    if (m_emulatorMode) {
        if (m_bus) {
            emit m_bus->moduleEvent(
                QLatin1String(G72Keys::kModule),
                QLatin1String(G72Keys::kEventKey),
                G72Keys::makeKeyEvent(QString::fromLatin1(keyName)));
        }
        return;
    }

    // Normal режим — QML-сигналы
    const QLatin1String key(keyName);
    if      (key == G72Keys::kHome)       emit homePressed();
    else if (key == G72Keys::kBack)       emit backPressed();
    else if (key == G72Keys::kSearch)     emit searchPressed();
    else if (key == G72Keys::kMenu)       emit menuPressed();
    else if (key == G72Keys::kUp)         emit upPressed();
    else if (key == G72Keys::kDown)       emit downPressed();
    else if (key == G72Keys::kLeft)       emit leftPressed();
    else if (key == G72Keys::kRight)      emit rightPressed();
    else if (key == G72Keys::kSelect)     emit selectPressed();
    else if (key == G72Keys::kVolumeUp)   emit volumeUpPressed();
    else if (key == G72Keys::kVolumeDown) emit volumeDownPressed();
}
