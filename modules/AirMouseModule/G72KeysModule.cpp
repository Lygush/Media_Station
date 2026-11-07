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
std::atomic<G72KeysModule *> G72KeysModule::s_instance { nullptr };

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
#ifdef Q_OS_WIN
                    m_loggedTvKey = false;
                    if (active) requestHookReinstall();
#endif
                    emit emulatorModeChanged(active);
                    applyCursorState();
                    qDebug() << "[G72Keys] EmulatorMode:" << (active ? "ON" : "OFF");
                    return;
                }

                if (mod == QLatin1String(G72Keys::kModule) &&
                    ev  == QLatin1String(G72Keys::kEventReinstallHook))
                {
#ifdef Q_OS_WIN
                    requestHookReinstall();
#endif
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
    startHookThread();
#endif
}

#ifdef Q_OS_WIN
void G72KeysModule::startHookThread()
{
    if (m_hookThread.joinable()) return;
    m_hookThread = std::thread([this]() { hookThreadMain(); });
}

void G72KeysModule::stopHookThread()
{
    if (!m_hookThread.joinable()) return;
    // Поток мог ещё не успеть создать очередь сообщений — ждём его id
    for (int i = 0; i < 200 && m_hookThreadId.load() == 0; ++i)
        Sleep(5);
    if (const DWORD id = m_hookThreadId.load())
        PostThreadMessageW(id, WM_QUIT, 0, 0);
    m_hookThread.join();
    m_hookThreadId = 0;
}

void G72KeysModule::hookThreadMain()
{
    // Создаём очередь сообщений потока до публикации id (иначе
    // PostThreadMessage из stopHookThread может уйти в пустоту)
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    m_hookThreadId = GetCurrentThreadId();

    // Обработчик хука — несколько сравнений и PostMessage; высокий приоритет
    // гарантирует, что Windows не посчитает хук «зависшим» даже при полной
    // загрузке процессора эмулятором.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    s_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, hInst, 0);
    if (!s_keyboardHook)
        qWarning() << "[G72Keys] WH_KEYBOARD_LL failed, error:" << GetLastError();
    else
        qDebug() << "[G72Keys] Keyboard hook installed (own thread)";

    s_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, hInst, 0);
    if (!s_mouseHook)
        qWarning() << "[G72Keys] WH_MOUSE_LL failed, error:" << GetLastError();
    else
        qDebug() << "[G72Keys] Mouse hook installed (own thread)";

    // Таймер потока (hwnd = nullptr) — WM_TIMER приходит в эту очередь
    SetTimer(nullptr, 0, kHookRefreshMs, nullptr);

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == nullptr && msg.message == kMsgReinstallHook) {
            reinstallKeyboardHook();
            qDebug() << "[G72Keys] Keyboard hook reinstalled on request";
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_TIMER) {
            G72KeysModule *self = s_instance.load();
            if (self && self->m_emulatorMode.load()) reinstallKeyboardHook();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (s_keyboardHook) { UnhookWindowsHookEx(s_keyboardHook); s_keyboardHook = nullptr; }
    if (s_mouseHook)    { UnhookWindowsHookEx(s_mouseHook);    s_mouseHook    = nullptr; }
    qDebug() << "[G72Keys] Hook thread finished";
}

void G72KeysModule::requestHookReinstall()
{
    if (const DWORD id = m_hookThreadId.load())
        PostThreadMessageW(id, kMsgReinstallHook, 0, 0);
}

// Новый хук ставится ДО снятия старого — без «дыры», в которой клавиши
// прошли бы мимо. Последний установленный LL-хук вызывается первым.
void G72KeysModule::reinstallKeyboardHook()
{
    HHOOK fresh = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandleW(nullptr), 0);
    if (!fresh) {
        qWarning() << "[G72Keys] keyboard hook reinstall failed, error:" << GetLastError();
        return;
    }
    HHOOK old = s_keyboardHook;
    s_keyboardHook = fresh;
    if (old) UnhookWindowsHookEx(old);
}
#endif

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
    stopHookThread();
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
    G72KeysModule *self = s_instance.load();
    if (nCode < 0 || !self)
        return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);

    const KBDLLHOOKSTRUCT *kb = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
    const int  vk     = static_cast<int>(kb->vkCode);
    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    // Автоповтор: Windows шлёт WM_KEYDOWN, пока клавиша удержана. Стрелкам
    // повтор нужен (листание), а «Меню»/Esc, OK, «Назад», «Домой» от повтора
    // срабатывали по нескольку раз: оверлей открывался-закрывался подряд
    // («выезжает несколько раз»). Вызывается только из потока хуков.
    static bool s_down[256] = {};
    bool isRepeat = false;
    if (vk >= 0 && vk < 256) {
        isRepeat  = isDown && s_down[vk];
        s_down[vk] = isDown;
    }
    const bool repeatAllowed = (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT);

    // Esc с обычной клавиатуры в режиме ТВ = кнопка «Меню» пульта (оверлей с
    // выходом из Android TV). Раньше Esc ловила только горячая клавиша в QML —
    // она срабатывает, лишь когда фокус у окна MediaStation, а при показе ТВ
    // фокус может оказаться у эмулятора. Вне режима ТВ Esc не трогаем.
    const bool escAsMenu = (vk == VK_ESCAPE) && self->m_emulatorMode.load();
    const char *keyName = escAsMenu ? G72Keys::kMenu : vkToKeyName(vk);
    if (!keyName)
        return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);

    // Диагностика для журнала: доходят ли клавиши до хука в режиме ТВ
    if (isDown && self->m_emulatorMode.load() && !self->m_loggedTvKey.exchange(true)) {
        HWND fg = GetForegroundWindow();
        wchar_t cls[64] = {};
        if (fg) GetClassNameW(fg, cls, 63);
        const QString info = QString("vk=0x%1, foreground class='%2'")
                                 .arg(vk, 2, 16, QLatin1Char('0')).arg(QString::fromWCharArray(cls));
        QMetaObject::invokeMethod(self, [info]() {
            qDebug().noquote() << "[G72Keys] First key in TV mode reached the hook:" << info;
        }, Qt::QueuedConnection);
    }

    if (isDown && (!isRepeat || repeatAllowed)) {
        QMetaObject::invokeMethod(self, "processKey",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, vk),
                                  Q_ARG(bool, true));
    }

    // Громкость всегда остаётся системной (Windows): раньше в режиме ТВ она
    // уходила в Android, и получалось две независимые громкости
    if (vk == VK_VOLUME_UP || vk == VK_VOLUME_DOWN || vk == VK_VOLUME_MUTE)
        return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);

    if (self->isInputBlocked())
        return 1;

    if (self->m_emulatorMode.load() || vk == 0xAC || vk == 0xAA)
        return 1;

    return CallNextHookEx(s_keyboardHook, nCode, wParam, lParam);
}

/*static*/ LRESULT CALLBACK G72KeysModule::mouseHookProc(
    int nCode, WPARAM wParam, LPARAM lParam)
{
    G72KeysModule *self = s_instance.load();
    // Вне режима ТВ — сразу дальше: этот путь вызывается на каждое движение мыши
    if (nCode < 0 || !self || !self->m_emulatorMode.load())
        return CallNextHookEx(s_mouseHook, nCode, wParam, lParam);

    {
        switch (wParam) {
        // Курсор в режиме ТВ не двигается: он над окном эмулятора и был бы
        // виден (схема B — MediaStation спрятан). AndroidTVModule убирает его
        // к краю экрана при показе ТВ и возвращает на выходе.
        case WM_MOUSEMOVE:
            return 1;
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
                if (!self->m_overlayClosing.load()) {
                    QMetaObject::invokeMethod(self, "processMouseAsSelect",
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
    // Esc приходит сюда только из режима ТВ (см. keyboardHookProc). Если режим
    // уже закончился, пока событие шло через очередь, — ничего не делаем.
    if (vkCode == VK_ESCAPE && !m_emulatorMode && !m_overlayOpen) return;
    const char *keyName = (vkCode == VK_ESCAPE) ? G72Keys::kMenu : vkToKeyName(vkCode);
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
