// modules/AndroidTVModule/AndroidTVWindow.cpp
//
// Управление окнами Win32 (без SetParent)
//   • Эмулятор — отдельное окно без рамки, позиционируемое под MediaStation
//   • MediaStation — TOPMOST + WS_EX_LAYERED; при показе эмулятора alpha = 0
//   • Оверлей (отдельное Tool-окно) работает поверх
//   • Таймер синхронизации позиции эмулятора
//
// Как находится окно эмулятора:
//   emulator.exe перезапускает себя как qemu-system-*.exe, поэтому окно
//   принадлежит ДОЧЕРНЕМУ процессу. Окно считается нашим, только если его
//   процесс входит в дерево процессов запущенного emulator.exe. Раньше
//   достаточно было слова "Emulator" в заголовке — так перехватывались окна
//   посторонних программ (Dolphin Emulator, PCSX2 …), и при остановке
//   Android TV убивался чужой процесс.

#include "AndroidTVModule.h"
#include "LoggerModule.h"

#ifdef Q_OS_WIN
#  include <dwmapi.h>
#endif

#include <QSet>
#include <QHash>
#include <QTimer>
#include <QCoreApplication>
#include <memory>

AndroidTVModule *AndroidTVModule::s_hookInstance = nullptr;

#ifdef Q_OS_WIN

namespace {

// Все процессы дерева rootPid (включая сам rootPid)
QSet<DWORD> processTree(DWORD rootPid)
{
    QSet<DWORD> result;
    if (!rootPid) return result;

    QHash<DWORD, DWORD> parentOf;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do { parentOf.insert(pe.th32ProcessID, pe.th32ParentProcessID); }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    result.insert(rootPid);
    bool grown = true;
    while (grown) {
        grown = false;
        for (auto it = parentOf.constBegin(); it != parentOf.constEnd(); ++it) {
            if (!result.contains(it.key()) && result.contains(it.value()) && it.key() != it.value()) {
                result.insert(it.key());
                grown = true;
            }
        }
    }
    return result;
}

QString windowTitle(HWND hwnd)
{
    wchar_t buf[512] = {};
    GetWindowTextW(hwnd, buf, 511);
    return QString::fromWCharArray(buf);
}

QString windowClass(HWND hwnd)
{
    wchar_t buf[256] = {};
    GetClassNameW(hwnd, buf, 255);
    return QString::fromWCharArray(buf);
}

bool isTopLevelUnowned(HWND hwnd)
{
    if (!IsWindow(hwnd)) return false;
    if (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CHILD) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    return true;
}

} // namespace

bool AndroidTVModule::isEmulatorProcess(DWORD pid) const
{
    if (!pid) return false;
    if (pid == static_cast<DWORD>(m_emulatorPid) || pid == static_cast<DWORD>(m_initialEmulatorPid))
        return true;
    if (!m_initialEmulatorPid) return false;
    return processTree(static_cast<DWORD>(m_initialEmulatorPid)).contains(pid);
}

// Главное окно эмулятора: верхнего уровня, без владельца, заголовок
// "Android Emulator - <avd>:<port>".
bool AndroidTVModule::isEmulatorMainWindow(HWND hwnd) const
{
    if (!isTopLevelUnowned(hwnd)) return false;
    if (windowClass(hwnd).contains(QLatin1String("Tool"), Qt::CaseInsensitive)) return false;

    const QString title = windowTitle(hwnd);
    if (title.contains(QLatin1String("Android Emulator"), Qt::CaseInsensitive)) return true;
    if (!m_launchAvdName.isEmpty() &&
        title.contains(m_launchAvdName + QLatin1Char(':') + QString::number(m_consolePort)))
        return true;
    return false;
}

HWND AndroidTVModule::findEmulatorMainWindow(bool allowFallback) const
{
    struct Ctx {
        const AndroidTVModule *self;
        QSet<DWORD> pids;
        bool allowFallback;
        HWND match    = nullptr;
        HWND fallback = nullptr;
        long fallbackArea = 0;
    } ctx{ this, processTree(static_cast<DWORD>(m_initialEmulatorPid ? m_initialEmulatorPid : m_emulatorPid)),
           allowFallback };
    if (m_emulatorPid) ctx.pids.insert(static_cast<DWORD>(m_emulatorPid));
    if (ctx.pids.isEmpty()) return nullptr;

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!c->pids.contains(pid)) return TRUE;

        if (c->self->isEmulatorMainWindow(hwnd)) {
            c->match = hwnd;
            return FALSE;
        }
        if (c->allowFallback && isTopLevelUnowned(hwnd) &&
            !windowClass(hwnd).contains(QLatin1String("Tool"), Qt::CaseInsensitive)) {
            RECT r;
            GetWindowRect(hwnd, &r);
            const long area = long(r.right - r.left) * long(r.bottom - r.top);
            if ((r.right - r.left) >= 200 && (r.bottom - r.top) >= 200 && area > c->fallbackArea) {
                c->fallback = hwnd;
                c->fallbackArea = area;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.match ? ctx.match : ctx.fallback;
}

void CALLBACK AndroidTVModule::s_winEventProc(
    HWINEVENTHOOK, DWORD, HWND hwnd,
    LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    AndroidTVModule *self = s_hookInstance;
    if (!self || !hwnd) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;

    // Дешёвая проверка заголовка перед дорогим снимком процессов: события SHOW
    // приходят от всех окон системы (подсказки, меню и т.д.).
    const bool knownPid = pid == static_cast<DWORD>(self->m_emulatorPid)
                          || pid == static_cast<DWORD>(self->m_initialEmulatorPid);
    if (!self->isEmulatorMainWindow(hwnd)) {
        // Побочное окно эмулятора (панель инструментов и т.п.) — прячем, пока
        // эмулятор не показан, чтобы оно не мелькало поверх рабочего стола.
        if (knownPid && !self->m_emulatorShown && IsWindowVisible(hwnd) && hwnd != self->m_emulatorHwnd)
            ShowWindow(hwnd, SW_HIDE);
        return;
    }
    if (!knownPid && !self->isEmulatorProcess(pid)) return;

    if (pid != static_cast<DWORD>(self->m_emulatorPid)) {
        self->logToEmulatorFile(QString("WinEvent: emulator re-exec PID %1 → %2")
                                    .arg(self->m_emulatorPid).arg(pid));
        self->m_emulatorPid = pid;
        self->applyEmulatorProcessPriority(pid);
    }

    if (self->m_emulatorHwnd != hwnd) {
        self->logToEmulatorFile(
            QString("WinEvent: emulator main window HWND=%1 title=\"%2\" class=\"%3\"")
                .arg(reinterpret_cast<quintptr>(hwnd)).arg(windowTitle(hwnd), windowClass(hwnd)));
        self->m_emulatorHwnd = hwnd;
    }

    if (self->m_emulatorShown) return;   // уже показан пользователю — не прячем
    self->positionEmulatorBehindMediaStation(hwnd);
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
    if (s_hookInstance == this) s_hookInstance = nullptr;
    logToEmulatorFile("WinEvent hook removed");
#endif
}

#ifdef Q_OS_WIN

// Главное окно MediaStation: самое большое видимое окно верхнего уровня
// нашего процесса (не панель оверлея и не всплывающее окно).
HWND AndroidTVModule::findMediaStationWindow() const
{
    struct EnumData { DWORD pid; HWND result; long area; } data = { GetCurrentProcessId(), nullptr, 0 };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto *d = reinterpret_cast<EnumData *>(lParam);
        DWORD wPid = 0;
        GetWindowThreadProcessId(hwnd, &wPid);
        if (wPid != d->pid || !IsWindowVisible(hwnd) || !isTopLevelUnowned(hwnd)) return TRUE;
        if (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        RECT r;
        GetWindowRect(hwnd, &r);
        const long area = long(r.right - r.left) * long(r.bottom - r.top);
        if (area > d->area) { d->area = area; d->result = hwnd; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    return data.result;
}

QSize AndroidTVModule::getCurrentScreenResolution() const
{
    // Монитор, на котором находится MediaStation (а не всегда основной)
    if (m_mediaStationHwnd && IsWindow(m_mediaStationHwnd)) {
        HMONITOR mon = MonitorFromWindow(m_mediaStationHwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
                return QSize(int(dm.dmPelsWidth), int(dm.dmPelsHeight));
        }
    }
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        return QSize(int(dm.dmPelsWidth), int(dm.dmPelsHeight));
    return QSize(1920, 1080);
}

void AndroidTVModule::hideExtraEmulatorWindows(DWORD pid, HWND mainHwnd)
{
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
            ctx->module->logToEmulatorFile(
                QString("Hiding extra emulator window: HWND=%1, class='%2'")
                    .arg(reinterpret_cast<quintptr>(hwnd)).arg(windowClass(hwnd)));
            ShowWindow(hwnd, SW_HIDE);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void AndroidTVModule::positionEmulatorBehindMediaStation(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) return;
    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd)) {
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd) return;
    }
    if (m_emulatorShown) return;   // уже показан — не скрываем

    // Без рамки и заголовка
    LONG_PTR style   = GetWindowLongPtr(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    style   &= ~LONG_PTR(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    exStyle &= ~LONG_PTR(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

    // Прячем до позиционирования (без мелькания)
    ShowWindow(hwnd, SW_HIDE);

    RECT msRect;
    GetWindowRect(m_mediaStationHwnd, &msRect);
    const int x = msRect.left;
    const int y = msRect.top;
    const int w = msRect.right - msRect.left;
    const int h = msRect.bottom - msRect.top;

    // SWP_FRAMECHANGED — иначе изменение стиля не применяется
    SetWindowPos(hwnd, nullptr, x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) hideExtraEmulatorWindows(pid, hwnd);

    logToEmulatorFile(QString("Emulator positioned under MediaStation at (%1,%2) %3x%4")
                          .arg(x).arg(y).arg(w).arg(h));

    startPositionSyncTimer();
}

#endif // Q_OS_WIN

void AndroidTVModule::updateEmulatorPosition()
{
#ifdef Q_OS_WIN
    if (!m_emulatorHwnd || !IsWindow(m_emulatorHwnd)) return;
    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd)) {
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd) return;
    }
    if (!m_emulatorShown) return;

    RECT msRect, emRect;
    GetWindowRect(m_mediaStationHwnd, &msRect);
    GetWindowRect(m_emulatorHwnd, &emRect);
    const int x = msRect.left;
    const int y = msRect.top;
    const int w = msRect.right - msRect.left;
    const int h = msRect.bottom - msRect.top;

    // Двигаем только если позиция реально изменилась (экономим вызовы DWM).
    // SWP_NOREDRAW не используем — он блокирует перерисовку эмулятора.
    if (emRect.left != x || emRect.top != y ||
        (emRect.right - emRect.left) != w || (emRect.bottom - emRect.top) != h) {
        SetWindowPos(m_emulatorHwnd, nullptr, x, y, w, h,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
    }
#endif
}

void AndroidTVModule::startPositionSyncTimer()
{
    if (m_positionSyncTimer) return;
    m_positionSyncTimer = new QTimer(this);
    // 500 мс достаточно: окно MediaStation практически не двигается.
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
        restoreMediaStation();   // QML ждёт сигнала, чтобы убрать затемнение
        return;
    }
    if (m_switching) {
        logToEmulatorFile("applyFullscreenAndSwitch: already switching, ignoring");
        return;
    }
    setSwitching(true);

    if (!m_emulatorHwnd || !IsWindow(m_emulatorHwnd))
        m_emulatorHwnd = findEmulatorMainWindow(true);
    if (!m_emulatorHwnd || !IsWindow(m_emulatorHwnd)) {
        logImportant(Logger::kLevelError, "applyFullscreenAndSwitch: emulator window not found");
        m_emulatorHwnd = nullptr;
        setSwitching(false);
        restoreMediaStation();
        return;
    }

    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd)) {
        m_mediaStationHwnd = findMediaStationWindow();
        if (!m_mediaStationHwnd) {
            logImportant(Logger::kLevelError, "applyFullscreenAndSwitch: MediaStation window not found");
            setSwitching(false);
            restoreMediaStation();
            return;
        }
    }

    // 1. Эмулятор: без рамки, на месте MediaStation, пока скрыт
    RECT msRect;
    GetWindowRect(m_mediaStationHwnd, &msRect);
    const int x = msRect.left;
    const int y = msRect.top;
    const int w = msRect.right - msRect.left;
    const int h = msRect.bottom - msRect.top;

    LONG_PTR style = GetWindowLongPtr(m_emulatorHwnd, GWL_STYLE);
    style &= ~LONG_PTR(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowLongPtr(m_emulatorHwnd, GWL_STYLE, style);
    SetWindowPos(m_emulatorHwnd, nullptr, x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);

    DWORD pid = 0;
    GetWindowThreadProcessId(m_emulatorHwnd, &pid);
    if (pid) hideExtraEmulatorWindows(pid, m_emulatorHwnd);

    // 2. Показываем эмулятор под (непрозрачным пока) MediaStation
    setEmulatorShown(true);
    ShowWindow(m_emulatorHwnd, SW_SHOWNOACTIVATE);
    InvalidateRect(m_emulatorHwnd, nullptr, FALSE);
    UpdateWindow(m_emulatorHwnd);
    startPositionSyncTimer();

    // 3. Через кадр (16 мс) делаем MediaStation прозрачным — эмулятор уже отрисован
    QTimer::singleShot(16, this, [this]() {
        if (!m_running || !m_emulatorShown) {
            setSwitching(false);
            return;
        }
        setMediaStationOpacity(0);

        // Фокус остаётся на MediaStation: оверлей и клавиши обрабатываются здесь
        if (m_mediaStationHwnd) {
            SetForegroundWindow(m_mediaStationHwnd);
            SetFocus(m_mediaStationHwnd);
        }

        logToEmulatorFile(QString("applyFullscreenAndSwitch: emulator visible=%1")
                              .arg(m_emulatorHwnd && IsWindowVisible(m_emulatorHwnd)));
        notifyKeyMode(true);
        setSwitching(false);
    });

#else
    logToEmulatorFile("applyFullscreenAndSwitch: not supported on this platform");
    restoreMediaStation();
#endif
}

void AndroidTVModule::restoreMediaStation()
{
    const bool wasShown = m_emulatorShown;
    setEmulatorShown(false);

#ifdef Q_OS_WIN
    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd))
        m_mediaStationHwnd = findMediaStationWindow();

    if (m_emulatorHwnd && IsWindow(m_emulatorHwnd))
        ShowWindow(m_emulatorHwnd, SW_HIDE);

    if (m_mediaStationHwnd) {
        setMediaStationOpacity(255);
        // Пока эмулятор жив (работает «за» MediaStation) — держим TOPMOST,
        // после его закрытия флаг снимает onEmulatorFinished().
        if (m_running || m_booting)
            SetWindowPos(m_mediaStationHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        else
            releaseMediaStationTopmost();
        if (wasShown) SetForegroundWindow(m_mediaStationHwnd);

        DwmFlush();
        InvalidateRect(m_mediaStationHwnd, nullptr, TRUE);
        UpdateWindow(m_mediaStationHwnd);
    }
    logToEmulatorFile("restoreMediaStation: emulator hidden, MediaStation restored");
#else
    Q_UNUSED(wasShown)
#endif
    notifyKeyMode(false);
    emit emulatorRestoringMediaStation();
}

void AndroidTVModule::onMediaStationRestored()
{
    // Вызывается из QML после завершения анимации возврата. Пока эмулятор
    // работает, HWND сохраняем — он понадобится при следующем показе.
    if (m_running || m_booting) return;
#ifdef Q_OS_WIN
    m_emulatorHwnd = nullptr;
#endif
    setEmulatorShown(false);
    stopPositionSyncTimer();
}

void AndroidTVModule::forceMediaStationTopmost()
{
#ifdef Q_OS_WIN
    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd))
        m_mediaStationHwnd = findMediaStationWindow();

    if (m_mediaStationHwnd && IsWindow(m_mediaStationHwnd))
        SetWindowPos(m_mediaStationHwnd, HWND_TOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

// MediaStation делается TOPMOST только на время сессии Android TV (чтобы окно
// эмулятора не перекрыло его). Раньше флаг не снимался никогда: после первого
// запуска Android TV окна игр оказывались ПОД MediaStation (чёрный экран).
void AndroidTVModule::releaseMediaStationTopmost()
{
#ifdef Q_OS_WIN
    if (!m_mediaStationHwnd || !IsWindow(m_mediaStationHwnd))
        m_mediaStationHwnd = findMediaStationWindow();
    if (m_mediaStationHwnd && IsWindow(m_mediaStationHwnd))
        SetWindowPos(m_mediaStationHwnd, HWND_NOTOPMOST,
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

    auto tries = std::make_shared<int>(0);
    connect(m_windowPollTimer, &QTimer::timeout, this, [this, tries]() {
#ifdef Q_OS_WIN
        ++*tries;
        if (*tries > kWindowPollMaxTries) {
            logImportant(Logger::kLevelWarning, "Window polling timeout: emulator window not found");
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

        // Опрос раз в 100 мс, но перечисление окон дорогое — делаем его раз в 5 тиков
        if (*tries % 5 != 0) return;

        HWND hwnd = findEmulatorMainWindow(*tries > kWindowFallbackAfter);
        if (!hwnd) return;

        DWORD actualPid = 0;
        GetWindowThreadProcessId(hwnd, &actualPid);
        if (actualPid && actualPid != static_cast<DWORD>(m_emulatorPid)) {
            logToEmulatorFile(QString("Polling: emulator re-exec detected, PID %1 → %2")
                                  .arg(m_emulatorPid).arg(actualPid));
            m_emulatorPid = actualPid;
            applyEmulatorProcessPriority(actualPid);
        }
        logToEmulatorFile(QString("Polling: emulator window HWND=%1 title=\"%2\"")
                              .arg(reinterpret_cast<quintptr>(hwnd)).arg(windowTitle(hwnd)));
        m_emulatorHwnd = hwnd;
        if (!m_emulatorShown) positionEmulatorBehindMediaStation(hwnd);
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
