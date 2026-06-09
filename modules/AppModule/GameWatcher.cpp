// GameWatcher.cpp
// ─────────────────────────────────────────────────────────────────────────────
// GameWatcher — реализация мониторинга игрового процесса.
// ─────────────────────────────────────────────────────────────────────────────

#include "GameWatcher.h"

#include <QThread>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <tlhelp32.h>
#  include <psapi.h>
#endif

// Таймауты (мс)
static constexpr int kPidFileWaitMs = 30'000; ///< максимальное ожидание PID-файла
static constexpr int kPidPollMs     =    100; ///< интервал опроса PID-файла
static constexpr int kInputIdleMs   = 30'000; ///< максимальное ожидание WaitForInputIdle
static constexpr int kHwndPollMs    =    100; ///< интервал опроса HWND
static constexpr int kHwndMaxTries  =    300; ///< максимальное число попыток найти HWND (30 сек)

// ─────────────────────────────────────────────────────────────────────────────

GameWatcher::GameWatcher(QObject *parent) : QObject(parent) {}
GameWatcher::~GameWatcher() { stopAll(); }
bool GameWatcher::isGameRunning() const { return m_running.loadAcquire() != 0; }

void GameWatcher::stopAll()
{
    m_running.storeRelease(0);
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(5000);
        m_thread = nullptr;
    }
}

void GameWatcher::launch(const QString &ps1Path, const QString &tileId)
{
    if (m_thread && m_thread->isRunning()) {
        m_running.storeRelease(0);
        m_thread->quit();
        if (!m_thread->wait(3000)) {
            emit launchFailed(tileId, "Previous session did not finish in time");
            return;
        }
    }

#ifdef Q_OS_WIN
    AllowSetForegroundWindow(ASFW_ANY);
#endif

    m_running.storeRelease(1);

    QThread *thread = new QThread(this);
    m_thread = thread;

    connect(thread, &QThread::started, this, [this, ps1Path, tileId]() {
        monitorGame(ps1Path, tileId);
    }, Qt::DirectConnection);

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this,   [this]() { m_thread = nullptr; });

    thread->start();
}

// ─────────────────────────────────────────────────────────────────────────────
// readInputTypesFromPs1 — читает inputTypes из game_info.json рядом с ps1.
//
// Структура каталогов:
//   <gameRoot>/
//     game_info.json        ← здесь лежит поле "inputTypes"
//     launcher/
//       launch.ps1          ← ps1Path указывает сюда
//
// Возвращает пустой список если файл не найден, поле отсутствует или пусто.
// ─────────────────────────────────────────────────────────────────────────────
QStringList GameWatcher::readInputTypesFromPs1(const QString &ps1Path)
{
    // launcher dir → gameRoot (родитель launcher-а)
    const QString launcherDir = QFileInfo(ps1Path).absolutePath();
    const QString gameRoot    = QDir::cleanPath(launcherDir + "/..");
    const QString infoPath    = gameRoot + "/game_info.json";

    QFile f(infoPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qDebug() << "[GameWatcher] game_info.json not found at" << infoPath;
        return {};
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[GameWatcher] Failed to parse game_info.json:" << err.errorString();
        return {};
    }

    const QJsonArray arr = doc.object().value("inputTypes").toArray();
    QStringList result;
    for (const auto &v : arr) {
        const QString s = v.toString().trimmed();
        if (!s.isEmpty()) result << s;
    }

    qDebug() << "[GameWatcher] inputTypes from game_info.json:" << result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// monitorGame — основная логика мониторинга, выполняется в отдельном потоке.
// ─────────────────────────────────────────────────────────────────────────────
void GameWatcher::monitorGame(const QString &ps1Path, const QString &tileId)
{
#ifdef Q_OS_WIN

    // ── 0. Определение типа ввода из game_info.json ───────────────────────────
    // Эмитируем до запуска PS1, чтобы JoystickModule успел переключить профиль
    // DS4Windows ещё до того, как игра начнёт читать контроллер.
    const QStringList inputTypes = readInputTypesFromPs1(ps1Path);
    emit gameInputTypeDetected(tileId, inputTypes);

    // ── 1. Подготовка PID-файла ────────────────────────────────────────────────
    const QString tempDir  = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString pidToken = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString pidFile  = QDir(tempDir).filePath("mc_pid_" + pidToken + ".txt");

    // ── 2. Запуск PowerShell со скриптом ──────────────────────────────────────
    const quint64 psPid = launchPs1(ps1Path, pidFile);
    if (psPid == 0) {
        m_running.storeRelease(0);
        emit launchFailed(tileId, "Failed to start PowerShell");
        QThread::currentThread()->quit();
        return;
    }

    // ── 3. Чтение PID игрового процесса ───────────────────────────────────────
    const quint32 gamePid = readPidFile(pidFile, kPidFileWaitMs);
    QFile::remove(pidFile);

    if (gamePid == 0) {
        m_running.storeRelease(0);
        emit launchFailed(tileId, "Game process did not start (no PID received)");
        QThread::currentThread()->quit();
        return;
    }

    // ── 4. Получение HANDLE игрового процесса ─────────────────────────────────
    HANDLE hGame = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, static_cast<DWORD>(gamePid));

    if (!hGame) {
        m_running.storeRelease(0);
        emit launchFailed(tileId, "Game process already exited");
        QThread::currentThread()->quit();
        return;
    }

    // ── 5. Ожидание готовности процесса ───────────────────────────────────────
    // WaitForInputIdle возвращает WAIT_FAILED для консольных приложений — это нормально.
    WaitForInputIdle(hGame, kInputIdleMs);

    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(hGame, &exitCode);
    if (exitCode != STILL_ACTIVE) {
        CloseHandle(hGame);
        m_running.storeRelease(0);
        emit launchFailed(tileId, "Game process exited during startup");
        QThread::currentThread()->quit();
        return;
    }

    // ── 6. Поиск главного окна игры ───────────────────────────────────────────
    quintptr hwnd = 0;
    for (int attempt = 0; attempt < kHwndMaxTries && hwnd == 0; ++attempt) {
        if (m_running.loadAcquire() == 0) {
            CloseHandle(hGame);
            QThread::currentThread()->quit();
            return;
        }
        hwnd = findMainWindowByPid(gamePid);
        if (hwnd == 0) QThread::msleep(kHwndPollMs);
    }

    // ── 7. Применение borderless fullscreen ───────────────────────────────────
    if (hwnd != 0 && !hasOwnFullscreen(hwnd))
        applyBorderlessFullscreen(hwnd);

    // ── 8. Сигнал: игра готова ────────────────────────────────────────────────
    emit gameWindowAppeared(tileId, hwnd != 0);

    // ── 9. Ожидание завершения игры ───────────────────────────────────────────
    while (m_running.loadAcquire() != 0) {
        if (WaitForSingleObject(hGame, 500) == WAIT_OBJECT_0)
            break;
    }

    CloseHandle(hGame);
    m_running.storeRelease(0);
    emit gameClosed(tileId);

#else
    Q_UNUSED(ps1Path)
    m_running.storeRelease(0);
    emit launchFailed(tileId, "GameWatcher is only supported on Windows");
#endif

    QThread::currentThread()->quit();
}

// ─────────────────────────────────────────────────────────────────────────────
// launchPs1
// ─────────────────────────────────────────────────────────────────────────────
quint64 GameWatcher::launchPs1(const QString &ps1Path, const QString &pidFilePath)
{
#ifdef Q_OS_WIN
    const QFileInfo info(ps1Path);
    const QString nativePs1  = QDir::toNativeSeparators(info.absoluteFilePath());
    const QString nativeWork = QDir::toNativeSeparators(info.absolutePath());
    const QString nativePid  = QDir::toNativeSeparators(pidFilePath);

    const QString cmdLine = QString(
        "powershell.exe"
        " -ExecutionPolicy Bypass"
        " -NonInteractive"
        " -WindowStyle Hidden"
        " -File \"%1\""
        " -PidFile \"%2\""
    ).arg(nativePs1, nativePid);

    STARTUPINFOW si    = {};
    si.cb              = sizeof(si);
    si.dwFlags         = STARTF_USESHOWWINDOW;
    si.wShowWindow     = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    std::wstring wCmd = cmdLine.toStdWString();
    const std::wstring wDir = nativeWork.toStdWString();

    const BOOL ok = CreateProcessW(
        nullptr,
        wCmd.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        wDir.c_str(),
        &si, &pi);

    if (!ok) {
        qWarning() << "GameWatcher: CreateProcess failed, error:" << GetLastError();
        return 0;
    }

    const DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<quint64>(pid);
#else
    Q_UNUSED(ps1Path)
    Q_UNUSED(pidFilePath)
    return 0;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// readPidFile
// ─────────────────────────────────────────────────────────────────────────────
quint32 GameWatcher::readPidFile(const QString &pidFilePath, int timeoutMs)
{
#ifdef Q_OS_WIN
    const quint64 deadline = GetTickCount64() + static_cast<quint64>(timeoutMs);
    while (GetTickCount64() < deadline) {
        QFile f(pidFilePath);
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(f.readAll()).trimmed();
            f.close();
            bool ok = false;
            const DWORD pid = static_cast<DWORD>(content.toULong(&ok));
            if (ok && pid > 0) return static_cast<quint32>(pid);
        }
        QThread::msleep(kPidPollMs);
    }
    return 0;
#else
    Q_UNUSED(pidFilePath)
    Q_UNUSED(timeoutMs)
    return 0;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// findMainWindowByPid
// ─────────────────────────────────────────────────────────────────────────────
quintptr GameWatcher::findMainWindowByPid(quint32 pid)
{
#ifdef Q_OS_WIN
    struct EnumData { DWORD pid; HWND best; int bestArea; };
    EnumData data = { static_cast<DWORD>(pid), nullptr, 0 };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto *d = reinterpret_cast<EnumData*>(lParam);
        DWORD wPid = 0;
        GetWindowThreadProcessId(hwnd, &wPid);
        if (wPid != d->pid)         return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetParent(hwnd))        return TRUE;
        if (GetWindowLong(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
        RECT rc = {};
        GetWindowRect(hwnd, &rc);
        const int area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area < 200 * 150) return TRUE;
        if (area > d->bestArea) { d->bestArea = area; d->best = hwnd; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    return reinterpret_cast<quintptr>(data.best);
#else
    Q_UNUSED(pid)
    return 0;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// hasOwnFullscreen
// ─────────────────────────────────────────────────────────────────────────────
bool GameWatcher::hasOwnFullscreen(quintptr hwndRaw)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwndRaw);
    RECT rc = {};
    GetWindowRect(hwnd, &rc);

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    const bool coversMonitor = (rc.right  - rc.left) >= (mi.rcMonitor.right  - mi.rcMonitor.left) &&
                               (rc.bottom - rc.top)  >= (mi.rcMonitor.bottom - mi.rcMonitor.top);
    const bool atOrigin      = rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top;
    const bool noCaption     = !(GetWindowLong(hwnd, GWL_STYLE) & WS_CAPTION);

    return coversMonitor && atOrigin && noCaption;
#else
    Q_UNUSED(hwndRaw)
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// applyBorderlessFullscreen
// ─────────────────────────────────────────────────────────────────────────────
void GameWatcher::applyBorderlessFullscreen(quintptr hwndRaw)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwndRaw);

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER);
    style |=  (WS_POPUP | WS_VISIBLE | WS_SYSMENU);
    SetWindowLong(hwnd, GWL_STYLE, style);

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLong(hwnd, GWL_EXSTYLE, exStyle);

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    SetWindowPos(hwnd, HWND_TOP,
                 mi.rcMonitor.left,
                 mi.rcMonitor.top,
                 mi.rcMonitor.right  - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
#else
    Q_UNUSED(hwndRaw)
#endif
}
