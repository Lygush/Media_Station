// modules/AndroidTVModule/AndroidTVWindow.cpp
//
// Управление окнами Win32 (версия БЕЗ SetParent)
//   • Эмулятор — отдельное окно, позиционируемое позади MediaStation
//   • Прозрачность MediaStation (LWA_ALPHA) не влияет на эмулятор
//   • Оверлей (дочернее окно) работает поверх, как и раньше
//   • Таймер синхронизации позиции эмулятора
//   • Переключение без мелькания рабочего стола (задержка 16 мс для отрисовки эмулятора)

#include "AndroidTVModule.h"
#include "LoggerModule.h"

#ifdef Q_OS_WIN
#  include <dwmapi.h>
#  pragma comment(lib, "dwmapi.lib")
#endif

#include <QThread>
#include <QTimer>
#include <QCoreApplication>

AndroidTVModule *AndroidTVModule::s_hookInstance = nullptr;

#ifdef Q_OS_WIN

static DWORD getParentPid(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    DWORD ppid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) { ppid = pe.th32ParentProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return ppid;
}

void CALLBACK AndroidTVModule::s_winEventProc(
    HWINEVENTHOOK, DWORD, HWND hwnd,
    LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!s_hookInstance) return;

    char className[256] = {};
    GetClassNameA(hwnd, className, sizeof(className));
    if (strstr(className, "Tool") != nullptr) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;

    char title[512] = {};
    GetWindowTextA(hwnd, title, sizeof(title));

    const bool matchTitle  = strstr(title, "MediaStation_TV") != nullptr
                            || strstr(title, "Android Emulator") != nullptr
                            || strstr(title, "Emulator") != nullptr;
    const bool matchPid    = pid == static_cast<DWORD>(s_hookInstance->m_emulatorPid);

    bool matchParent = false;
    if (!matchTitle && !matchPid && s_hookInstance->m_initialEmulatorPid) {
        DWORD ppid = getParentPid(pid);
        matchParent = (ppid == static_cast<DWORD>(s_hookInstance->m_initialEmulatorPid)
                       || ppid == static_cast<DWORD>(s_hookInstance->m_emulatorPid));
    }

    if (!matchTitle && !matchPid && !matchParent) return;

    if ((matchTitle || matchParent) &&
        pid != static_cast<DWORD>(s_hookInstance->m_emulatorPid)) {
        s_hookInstance->logToEmulatorFile(
            QString("WinEvent: re-exec PID %1 → %2 (title: \"%3\", matchParent=%4)")
                .arg(s_hookInstance->m_emulatorPid).arg(pid)
                .arg(title).arg(matchParent));
        s_hookInstance->m_emulatorPid = pid;
    }

    const bool isMainWindow = matchTitle
                              || (matchParent && (IsWindowVisible(hwnd) || (GetWindowLong(hwnd, GWL_STYLE) & WS_VISIBLE)));

    if (!isMainWindow) return;

    s_hookInstance->logToEmulatorFile(
        QString("WinEvent: found emulator main window HWND=%1 title=\"%2\" class=\"%3\"")
            .arg((quintptr)hwnd).arg(title).arg(className));

    if (!s_hookInstance->m_emulatorHwnd)
        s_hookInstance->m_emulatorHwnd = hwnd;

    // Если эмулятор уже показан, не позиционируем повторно (чтобы не скрыть)
    if (s_hookInstance->m_emulatorShown) {
        s_hookInstance->logToEmulatorFile("WinEvent: emulator shown, skipping position");
        return;
    }

    s_hookInstance->positionEmulatorBehindMediaStation(hwnd);
}

#endif // Q_OS_WIN

void AndroidTVModule::installWinEventHook()
{
#ifdef Q_OS_WIN
    if (m_winEventHook) return;
    s_hookInstance = this;
    m_winEventHook = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
        nullptr, s_winEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (m_winEventHook)
        logToEmulatorFile("WinEvent hook installed");
    else
        logToEmulatorFile("WinEvent hook FAILED, error=" + QString::number(GetLastError()));
#endif
}

void AndroidTVModule::removeWinEventHook()
{
#ifdef Q_OS_WIN
    if (!m_winEventHook) return;
    UnhookWinEvent(m_winEventHook);
    m_winEventHook = nullptr;
    s_hookInstance = nullptr;
    logToEmulatorFile("WinEvent hook removed");
#endif
}

#ifdef Q_OS_WIN

HWND AndroidTVModule::findEmulatorWindowByTitle() const
{
    struct FindCtx {
        HWND  result    = nullptr;
        DWORD targetPid = 0;
    } ctx;
    ctx.targetPid = static_cast<DWORD>(m_emulatorPid);

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *ctx = reinterpret_cast<FindCtx *>(lp);

        char className[256] = {};
        GetClassNameA(hwnd, className, sizeof(className));
        if (strstr(className, "Tool") != nullptr) return TRUE;

        char title[512] = {};
        GetWindowTextA(hwnd, title, sizeof(title));

        const bool matchTitle =
            strstr(title, "MediaStation_TV") != nullptr
            || strstr(title, "Android Emulator") != nullptr
            || strstr(title, "Emulator") != nullptr;
        if (!matchTitle) return TRUE;

        if (ctx->targetPid) {
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid != ctx->targetPid) return TRUE;
        }
        ctx->result = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.result;
}

HWND AndroidTVModule::findMediaStationWindow() const
{
    const DWORD currentPid = GetCurrentProcessId();
    struct EnumData { DWORD pid; HWND result; } data = { currentPid, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto *d = reinterpret_cast<EnumData *>(lParam);
        DWORD wPid = 0;
        GetWindowThreadProcessId(hwnd, &wPid);
        if (wPid == d->pid && IsWindowVisible(hwnd)) {
            d->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    return data.result;
}

QSize AndroidTVModule::getCurrentScreenResolution() const
{
    DEVMODE dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettings(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        return QSize(dm.dmPelsWidth, dm.dmPelsHeight);
    return QSize(1920, 1080);
}

#endif // Q_OS_WIN

void AndroidTVModule::hideExtraEmulatorWindows(DWORD pid, HWND mainHwnd)
{
#ifdef Q_OS_WIN
    if (!pid || !mainHwnd) return;

    struct EnumCtx {
        DWORD            targetPid;
        HWND             mainHwnd;
        AndroidTVModule *module;
    } ctx = { pid, mainHwnd, this };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto *ctx = reinterpret_cast<EnumCtx *>(lParam);
        if (hwnd == ctx->mainHwnd) return TRUE;

        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != ctx->targetPid) return TRUE;

        if (IsWindowVisible(hwnd)) {
            char className[256] = {};
            GetClassNameA(hwnd, className, sizeof(className));
            ctx->module->logToEmulatorFile(
                QString("Hiding extra window: HWND=%1, class='%2'")
                    .arg((quintptr)hwnd).arg(className));
            ShowWindow(hwnd, SW_HIDE);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
#endif
}

void AndroidTVModule::positionEmulatorBehindMediaStation(HWND hwnd)
{
#ifdef Q_OS_WIN
    if (!hwnd || !IsWindow(hwnd)) return;
    if (!m_mediaStationHwnd) {
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd) return;
    }

    // Если эмулятор уже показан, не трогаем стили и не скрываем
    if (m_emulatorShown) {
        logToEmulatorFile("positionEmulatorBehindMediaStation: emulator shown, skipping hide");
        return;
    }

    // Убираем декорации и делаем окно без рамки (popup без заголовка)
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLong(hwnd, GWL_STYLE, style);
    SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);

    // Скрываем окно до того, как установим позицию (избегаем мелькания)
    ShowWindow(hwnd, SW_HIDE);

    // Получаем координаты и размер MediaStation
    RECT msRect;
    GetWindowRect(m_mediaStationHwnd, &msRect);
    int x = msRect.left;
    int y = msRect.top;
    int w = msRect.right - msRect.left;
    int h = msRect.bottom - msRect.top;

    // Размещаем эмулятор точно под MediaStation, сзади по Z-порядку.
    // SWP_NOREDRAW здесь не нужен — окно скрыто, мелькания не будет.
    SetWindowPos(hwnd, m_mediaStationHwnd,
                 x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);

    // Скрываем лишние окна
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) hideExtraEmulatorWindows(pid, hwnd);

    logToEmulatorFile(QString("Emulator positioned behind MediaStation at (%1,%2) %3x%4")
                          .arg(x).arg(y).arg(w).arg(h));

    // Запускаем таймер синхронизации позиции (если ещё не запущен)
    startPositionSyncTimer();
#endif
}

void AndroidTVModule::updateEmulatorPosition()
{
#ifdef Q_OS_WIN
    if (!m_emulatorHwnd || !IsWindow(m_emulatorHwnd)) return;
    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd)) {
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd) return;
    }

    RECT msRect;
    GetWindowRect(m_mediaStationHwnd, &msRect);
    int x = msRect.left;
    int y = msRect.top;
    int w = msRect.right - msRect.left;
    int h = msRect.bottom - msRect.top;

    // Обновляем позицию, только если окно эмулятора видимо.
    // SWP_NOREDRAW здесь НЕ используем — он блокирует перерисовку эмулятора и вызывает лаги.
    if (m_emulatorShown) {
        RECT emRect;
        GetWindowRect(m_emulatorHwnd, &emRect);
        // Двигаем только если позиция реально изменилась (экономим вызовы DWM)
        if (emRect.left != x || emRect.top != y ||
            (emRect.right - emRect.left) != w || (emRect.bottom - emRect.top) != h)
        {
            SetWindowPos(m_emulatorHwnd, m_mediaStationHwnd,
                         x, y, w, h,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
        }
    }
#endif
}

void AndroidTVModule::startPositionSyncTimer()
{
    if (m_positionSyncTimer) return;
    m_positionSyncTimer = new QTimer(this);
    // 500 мс достаточно: окно MediaStation практически не двигается.
    // 100 мс (10 раз/сек) создавало постоянную нагрузку на DWM и крало CPU у эмулятора.
    m_positionSyncTimer->setInterval(500);
    connect(m_positionSyncTimer, &QTimer::timeout, this, &AndroidTVModule::updateEmulatorPosition);
    m_positionSyncTimer->start();
    logToEmulatorFile("Position sync timer started (500ms interval)");
}

void AndroidTVModule::stopPositionSyncTimer()
{
    if (m_positionSyncTimer) {
        m_positionSyncTimer->stop();
        m_positionSyncTimer->deleteLater();
        m_positionSyncTimer = nullptr;
        logToEmulatorFile("Position sync timer stopped");
    }
}

void AndroidTVModule::applyFullscreenAndSwitch()
{
#ifdef Q_OS_WIN
    if (!m_running) {
        logToEmulatorFile("applyFullscreenAndSwitch: emulator not running");
        return;
    }
    if (m_switching) {
        logToEmulatorFile("applyFullscreenAndSwitch: already switching, ignoring");
        return;
    }
    setSwitching(true);

    if (!m_emulatorHwnd) m_emulatorHwnd = findEmulatorWindowByTitle();
    if (!m_emulatorHwnd || !IsWindow(m_emulatorHwnd)) {
        logToEmulatorFile("applyFullscreenAndSwitch: emulator window not found");
        setSwitching(false);
        return;
    }

    if (!m_mediaStationHwnd) {
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd) {
            logToEmulatorFile("applyFullscreenAndSwitch: MediaStation window not found");
            setSwitching(false);
            return;
        }
    }

    // 1. Подготавливаем эмулятор: позиция, стиль, скрыт
    RECT msRect;
    GetWindowRect(m_mediaStationHwnd, &msRect);
    int x = msRect.left;
    int y = msRect.top;
    int w = msRect.right - msRect.left;
    int h = msRect.bottom - msRect.top;

    // Убираем рамку
    LONG style = GetWindowLong(m_emulatorHwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowLong(m_emulatorHwnd, GWL_STYLE, style);

    // Перемещаем эмулятор за MediaStation и скрываем его
    SetWindowPos(m_emulatorHwnd, m_mediaStationHwnd, x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    ShowWindow(m_emulatorHwnd, SW_HIDE);

    // Скрываем лишние окна
    DWORD pid = 0;
    GetWindowThreadProcessId(m_emulatorHwnd, &pid);
    if (pid) hideExtraEmulatorWindows(pid, m_emulatorHwnd);

    // 2. Показываем эмулятор, но MediaStation пока остаётся непрозрачным (перекрывает возможные артефакты)
    m_emulatorShown = true;
    ShowWindow(m_emulatorHwnd, SW_SHOW);

    // Принудительно обновляем эмулятор
    InvalidateRect(m_emulatorHwnd, nullptr, FALSE);
    UpdateWindow(m_emulatorHwnd);

    // 3. Ждём один кадр (16 мс), чтобы эмулятор гарантированно отрисовался,
    //    и только потом делаем MediaStation прозрачным.
    QTimer::singleShot(16, this, [this]() {
        setMediaStationOpacity(0);
        logToEmulatorFile("applyFullscreenAndSwitch: MediaStation transparent (after 16ms)");

        // Фокус на MediaStation, чтобы работал оверлей и клавиши
        SetForegroundWindow(m_mediaStationHwnd);
        SetFocus(m_mediaStationHwnd);

        bool visible = IsWindowVisible(m_emulatorHwnd);
        logToEmulatorFile(QString("Emulator window visible after show: %1").arg(visible));

        logToEmulatorFile("applyFullscreenAndSwitch: emulator visible");
        notifyKeyMode(true);
        setSwitching(false);
    });

#else
    logToEmulatorFile("applyFullscreenAndSwitch stub");
#endif
}

void AndroidTVModule::restoreMediaStation()
{
#ifdef Q_OS_WIN
    m_emulatorShown = false;

    if (!m_mediaStationHwnd) {
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd) return;
    }

    // Скрываем эмулятор
    if (m_emulatorHwnd && IsWindow(m_emulatorHwnd)) {
        ShowWindow(m_emulatorHwnd, SW_HIDE);
    }

    // Возвращаем непрозрачность MediaStation
    setMediaStationOpacity(255);
    logToEmulatorFile("restoreMediaStation: MediaStation opaque");

    // Обеспечиваем, что MediaStation поверх всех
    SetWindowPos(m_mediaStationHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    DwmFlush();
    InvalidateRect(m_mediaStationHwnd, nullptr, TRUE);
    UpdateWindow(m_mediaStationHwnd);

    logToEmulatorFile("restoreMediaStation: emulator hidden, MediaStation restored");
#endif
    notifyKeyMode(false);
    emit emulatorRestoringMediaStation();
}

void AndroidTVModule::onMediaStationRestored()
{
#ifdef Q_OS_WIN
    // При завершении эмулятора просто очищаем HWND
    m_emulatorHwnd = nullptr;
    m_emulatorShown = false;
    stopPositionSyncTimer();
#endif
}

void AndroidTVModule::forceMediaStationTopmost()
{
#ifdef Q_OS_WIN
    if (!m_mediaStationHwnd)
        m_mediaStationHwnd = findMediaStationWindow();

    if (m_mediaStationHwnd && IsWindow(m_mediaStationHwnd))
        SetWindowPos(m_mediaStationHwnd, HWND_TOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

void AndroidTVModule::startKeepTopmostTimer()
{
    if (m_keepTopmostTimer) return;
    m_keepTopmostTimer = new QTimer(this);
    m_keepTopmostTimer->setInterval(200);
    connect(m_keepTopmostTimer, &QTimer::timeout, this, [this]() {
        if (m_booting && !m_running) {
            forceMediaStationTopmost();
        } else {
            stopKeepTopmostTimer();
            logToEmulatorFile("keepTopmostTimer stopped (booting finished)");
        }
    });
    m_keepTopmostTimer->start();
    logToEmulatorFile("keepTopmostTimer started (topmost every 200ms during boot)");
}

void AndroidTVModule::stopKeepTopmostTimer()
{
    if (m_keepTopmostTimer) {
        m_keepTopmostTimer->stop();
        m_keepTopmostTimer->deleteLater();
        m_keepTopmostTimer = nullptr;
        logToEmulatorFile("keepTopmostTimer stopped");
    }
}

void AndroidTVModule::startWindowPolling()
{
    if (m_windowPollTimer) return;
    m_windowPollTimer = new QTimer(this);
    m_windowPollTimer->setInterval(kWindowPollIntervalMs);

    int tries = 0;
    connect(m_windowPollTimer, &QTimer::timeout, this,
            [this, tries]() mutable {
#ifdef Q_OS_WIN
                ++tries;
                if (tries > kWindowPollMaxTries) {
                    logImportant(Logger::kLevelWarning,
                                 "Window polling timeout: emulator window not found");
                    stopWindowPolling();
                    return;
                }

                if (m_emulatorHwnd && IsWindow(m_emulatorHwnd)) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(m_emulatorHwnd, &pid);
                    if (pid) hideExtraEmulatorWindows(pid, m_emulatorHwnd);
                    stopWindowPolling();
                    logToEmulatorFile("Window polling stopped: emulator window found");
                    return;
                }

                HWND hwnd = findEmulatorWindowByTitle();
                if (!hwnd) return;

                DWORD actualPid = 0;
                GetWindowThreadProcessId(hwnd, &actualPid);
                if (actualPid && actualPid != static_cast<DWORD>(m_emulatorPid)) {
                    logToEmulatorFile(
                        QString("Polling: re-exec detected by title, PID %1 → %2")
                            .arg(m_emulatorPid).arg(actualPid));
                    m_emulatorPid = actualPid;
                }
                m_emulatorHwnd = hwnd;
                positionEmulatorBehindMediaStation(hwnd);
#else
                Q_UNUSED(tries)
                stopWindowPolling();
#endif
            });

    m_windowPollTimer->start();
}

void AndroidTVModule::stopWindowPolling()
{
    if (m_windowPollTimer) {
        m_windowPollTimer->stop();
        m_windowPollTimer->deleteLater();
        m_windowPollTimer = nullptr;
    }
}
