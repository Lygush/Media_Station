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
#include <iterator>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <tlhelp32.h>
#  include <psapi.h>
#endif

#ifdef Q_OS_WIN
// Таймауты (мс)
static constexpr int kPidFileWaitMs = 30'000; ///< максимальное ожидание PID-файла
static constexpr int kPidPollMs     =    100; ///< интервал опроса PID-файла
static constexpr int kInputIdleMs   = 30'000; ///< максимальное ожидание WaitForInputIdle
static constexpr int kHwndPollMs    =    100; ///< интервал опроса HWND
static constexpr int kHwndMaxTries  =    300; ///< максимальное число попыток найти HWND (30 сек)
static constexpr int kSuccessorWaitStartMs = 20'000; ///< ожидание «настоящей» игры, если exe-запускатор вышел при старте
static constexpr int kSuccessorWaitExitMs  =  8'000; ///< то же после закрытия отслеживаемого процесса
static constexpr int kMaxSuccessors        =      5; ///< защита от бесконечной смены процессов

// ── Поиск процессов игры по папке ────────────────────────────────────────────
// Многие игры запускаются в несколько процессов: exe-запускатор стартует
// настоящую игру и выходит (Steam-версии перезапускают себя через Steam,
// лаунчеры издателей, «game.exe → game_x64.exe»). Раньше выход первого
// процесса считался закрытием игры: медиацентр всплывал поверх запущенной
// игры или запуск считался неудачным. Теперь следим за любым процессом,
// чей exe лежит в папке игры.

static QString processImagePath(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = static_cast<DWORD>(std::size(buf));
    QString path;
    if (QueryFullProcessImageNameW(h, 0, buf, &len))
        path = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromWCharArray(buf, int(len))));
    CloseHandle(h);
    return path;
}

static bool isInDir(const QString &path, const QString &dir)
{
    if (path.isEmpty() || dir.isEmpty()) return false;
    const QString d = dir.endsWith('/') ? dir : dir + '/';
    return path.startsWith(d, Qt::CaseInsensitive);
}

/// Живой процесс из папки игры (кроме exclude).
static DWORD findProcessInDir(const QString &dir, DWORD exclude)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == exclude || pe.th32ProcessID == 0) continue;
            if (isInDir(processImagePath(pe.th32ProcessID), dir)) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────

GameWatcher::GameWatcher(QObject *parent) : QObject(parent) {}
GameWatcher::~GameWatcher() { stopAll(); }
bool GameWatcher::isGameRunning() const { return m_running.loadAcquire() != 0; }

void GameWatcher::stopAll()
{
    m_running.storeRelease(0);
    if (m_thread) {
        m_thread->quit();
        // Все ожидания в monitorGame идут короткими шагами и проверяют
        // m_running, поэтому поток завершается за доли секунды.
        if (!m_thread->wait(5000)) {
            qWarning() << "[GameWatcher] monitor thread did not stop in time";
            // Не даём родителю удалить работающий QThread — это abort().
            // Сам объект удалится по finished (подключено в launch()).
            m_thread->setParent(nullptr);
        }
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
    // Обнуляем только если это всё ещё текущий поток: отложенный finished
    // старой сессии раньше затирал указатель на новую, и при выходе новая
    // не дожидалась → QThread уничтожался работающим (аварийное завершение).
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_thread == thread) m_thread = nullptr;
    });

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
    quint32 gamePid = readPidFile(pidFile, kPidFileWaitMs, m_running);
    QFile::remove(pidFile);

    if (m_running.loadAcquire() == 0) {   // программа закрывается
        QThread::currentThread()->quit();
        return;
    }
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
    // Ждём короткими шагами, чтобы выход из программы не блокировался до 30 с.
    for (int waited = 0; waited < kInputIdleMs; waited += 250) {
        if (m_running.loadAcquire() == 0) {
            CloseHandle(hGame);
            QThread::currentThread()->quit();
            return;
        }
        if (WaitForInputIdle(hGame, 250) != WAIT_TIMEOUT)
            break;
    }

    // Папка игры — по exe запущенного процесса (для внешних ярлыков это не
    // папка launcher); в ней ищем процессы-преемники
    // Для установленных игр — вся папка <корень>/game (запускатор и игра могут
    // лежать в разных подпапках), для внешних ярлыков — папка exe.
    const QString installedDir = QDir::cleanPath(QFileInfo(ps1Path).absolutePath() + "/../game");
    const QString exePath      = processImagePath(static_cast<DWORD>(gamePid));
    QString gameDir = isInDir(exePath, installedDir) ? installedDir : QFileInfo(exePath).absolutePath();
    if (exePath.isEmpty()) gameDir = installedDir;
    qInfo().noquote() << "[GameWatcher] started PID" << gamePid << "in" << QDir::toNativeSeparators(gameDir);

    // Ждём процесс-преемник из той же папки. Возвращает новый HANDLE или nullptr.
    int successors = 0;
    auto waitSuccessor = [&](DWORD exitedPid, int timeoutMs) -> HANDLE {
        if (successors >= kMaxSuccessors) return nullptr;
        for (int waited = 0; waited < timeoutMs; waited += 500) {
            if (m_running.loadAcquire() == 0) return nullptr;
            const DWORD next = findProcessInDir(gameDir, exitedPid);
            if (next) {
                HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, next);
                if (h) {
                    ++successors;
                    qInfo().noquote() << "[GameWatcher] process" << exitedPid << "exited, following"
                                      << next << QFileInfo(processImagePath(next)).fileName();
                    gamePid = next;
                    return h;
                }
            }
            QThread::msleep(500);
        }
        return nullptr;
    };

    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(hGame, &exitCode);
    if (exitCode != STILL_ACTIVE) {
        CloseHandle(hGame);
        hGame = waitSuccessor(static_cast<DWORD>(gamePid), kSuccessorWaitStartMs);
        if (!hGame) {
            if (m_running.loadAcquire() == 0) { QThread::currentThread()->quit(); return; }
            m_running.storeRelease(0);
            emit launchFailed(tileId, "Game process exited during startup");
            QThread::currentThread()->quit();
            return;
        }
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
        if (hwnd == 0 && attempt % 10 == 9) {
            // Окно может принадлежать другому процессу игры (запускатор ещё жив)
            const DWORD other = findProcessInDir(gameDir, static_cast<DWORD>(gamePid));
            if (other) hwnd = findMainWindowByPid(other);
        }
        if (hwnd == 0) {
            // Отслеживаемый процесс вышел, пока ждали окно — переключаемся
            if (WaitForSingleObject(hGame, 0) == WAIT_OBJECT_0) {
                CloseHandle(hGame);
                hGame = waitSuccessor(static_cast<DWORD>(gamePid), kSuccessorWaitStartMs);
                if (!hGame) break;
                continue;
            }
            QThread::msleep(kHwndPollMs);
        }
    }
    if (!hGame) {
        if (m_running.loadAcquire() == 0) { QThread::currentThread()->quit(); return; }
        m_running.storeRelease(0);
        emit launchFailed(tileId, "Game process exited during startup");
        QThread::currentThread()->quit();
        return;
    }

    // ── 7. Применение borderless fullscreen ───────────────────────────────────
    // Только для большого окна (оконный режим игры). Раньше растягивалось любое
    // первое окно — заставки и окна настроек лаунчеров тоже.
    if (hwnd != 0 && !hasOwnFullscreen(hwnd) && isLargeGameWindow(hwnd))
        applyBorderlessFullscreen(hwnd);

    // ── 8. Сигнал: игра готова ────────────────────────────────────────────────
    emit gameWindowAppeared(tileId, hwnd != 0);

    // ── 9. Ожидание завершения игры ───────────────────────────────────────────
    // Если процесс закрылся, но из папки игры работает другой (перезапуск
    // через лаунчер, отдельный процесс игры), следим за ним.
    while (m_running.loadAcquire() != 0) {
        if (WaitForSingleObject(hGame, 500) != WAIT_OBJECT_0) continue;
        CloseHandle(hGame);
        hGame = waitSuccessor(static_cast<DWORD>(gamePid), kSuccessorWaitExitMs);
        if (!hGame) break;
    }

    if (hGame) CloseHandle(hGame);
    qInfo().noquote() << "[GameWatcher] game closed:" << tileId;
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
quint32 GameWatcher::readPidFile(const QString &pidFilePath, int timeoutMs,
                                 const QAtomicInt &running)
{
#ifdef Q_OS_WIN
    const quint64 deadline = GetTickCount64() + static_cast<quint64>(timeoutMs);
    while (GetTickCount64() < deadline && running.loadAcquire() != 0) {
        QFile f(pidFilePath);
        if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(f.readAll()).trimmed();
            f.close();
            bool ok = false;
            const DWORD pid = static_cast<DWORD>(content.toULong(&ok));
            // "0" — launch.ps1 сообщает об ошибке запуска: не ждём весь таймаут
            if (ok) return static_cast<quint32>(pid);
        }
        QThread::msleep(kPidPollMs);
    }
    return 0;
#else
    Q_UNUSED(pidFilePath)
    Q_UNUSED(timeoutMs)
    Q_UNUSED(running)
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
// isLargeGameWindow — окно занимает не меньше половины монитора
// ─────────────────────────────────────────────────────────────────────────────
bool GameWatcher::isLargeGameWindow(quintptr hwndRaw)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(hwndRaw);
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    const double winArea = double(rc.right - rc.left) * double(rc.bottom - rc.top);
    const double monArea = double(mi.rcMonitor.right - mi.rcMonitor.left)
                         * double(mi.rcMonitor.bottom - mi.rcMonitor.top);
    return monArea > 0 && winArea >= monArea * 0.5;
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
