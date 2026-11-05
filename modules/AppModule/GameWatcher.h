// GameWatcher.h
// ─────────────────────────────────────────────────────────────────────────────
// GameWatcher — отслеживание жизненного цикла игрового процесса через PID.
//
// Протокол запуска:
//   1. C++ запускает powershell.exe -File launch.ps1 (без elevation, скрытно)
//   2. PS1 запускает игру через Start-Process -PassThru и записывает PID
//      в временный файл <TempDir>/mc_pid_<token>.txt
//   3. C++ читает PID из файла и открывает HANDLE процесса
//   4. WaitForInputIdle(hGame) — ждём, пока процесс готов к вводу
//      (главное окно создано, первый цикл сообщений запущен)
//   5. Находим HWND по PID; если окно не fullscreen — применяем borderless
//   6. Emit gameWindowAppeared → QML скрывает оверлей медиацентра
//   7. WaitForSingleObject(hGame, ...) — ждём завершения без активного поллинга
//   8. Emit gameClosed → QML возвращает медиацентр
//
// Тип ввода:
//   Перед запуском PS1 GameWatcher читает game_info.json["inputTypes"] из
//   <gameRoot>/game_info.json (gameRoot = родитель папки launcher где лежит ps1).
//   Эмитирует gameInputTypeDetected — AppController пробрасывает в EventBus,
//   JoystickModule получает команду на переключение профиля DS4Windows.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QThread>
#include <QString>
#include <QStringList>
#include <QAtomicInt>

class GameWatcher : public QObject
{
    Q_OBJECT

public:
    explicit GameWatcher(QObject *parent = nullptr);
    ~GameWatcher();

    /// Запустить мониторинг игры: ps1Path — скрипт запуска, tileId — идентификатор плитки.
    Q_INVOKABLE void launch(const QString &ps1Path, const QString &tileId);

    /// Остановить мониторинг (при завершении приложения).
    Q_INVOKABLE void stopAll();

    /// Возвращает true если игра сейчас запущена.
    Q_INVOKABLE bool isGameRunning() const;

signals:
    /// Эмитируется до запуска PS1. inputTypes берётся из game_info.json["inputTypes"].
    /// Пустой список — тип неизвестен (старая установка или внешний ярлык без анализа).
    void gameInputTypeDetected(const QString &tileId, const QStringList &inputTypes);

    /// windowFound = true  — окно найдено, игра готова к вводу.
    /// windowFound = false — окно так и не появилось за kHwndMaxTries×kHwndPollMs мс,
    ///                       но процесс жив; оверлей показывать не нужно.
    void gameWindowAppeared(const QString &tileId, bool windowFound);
    void gameClosed(const QString &tileId);
    void launchFailed(const QString &tileId, const QString &reason);

private:
    QThread    *m_thread  = nullptr;
    QAtomicInt  m_running = 0;

    void monitorGame(const QString &ps1Path, const QString &tileId);

    /// Читает inputTypes из game_info.json рядом с ps1-скриптом.
    /// ps1Path должен быть в <gameRoot>/launcher/launch.ps1 →
    /// game_info.json ищется в <gameRoot>/game_info.json.
    /// Возвращает пустой список если файл не найден или поле отсутствует.
    static QStringList readInputTypesFromPs1(const QString &ps1Path);

    /// Запускает powershell.exe с launch.ps1 через CreateProcess (без UAC).
    /// pidFilePath — путь куда PS1 запишет PID игры.
    /// Возвращает PID процесса powershell или 0 при ошибке.
    static quint64 launchPs1(const QString &ps1Path, const QString &pidFilePath);

    /// Читает PID из файла, ожидая его появления не более timeoutMs мс.
    /// Прерывается, как только running становится 0 (выход из программы).
    static quint32  readPidFile(const QString &pidFilePath, int timeoutMs,
                                const QAtomicInt &running);

    /// Находит главный HWND процесса по его PID.
    static quintptr findMainWindowByPid(quint32 pid);

    /// Проверяет, занимает ли окно весь монитор без рамки (exclusive/borderless FS).
    static bool hasOwnFullscreen(quintptr hwnd);

    /// Окно занимает не меньше половины монитора (игра в окне, а не заставка/лаунчер).
    static bool isLargeGameWindow(quintptr hwnd);

    /// Переводит окно в режим borderless fullscreen на мониторе, где оно находится.
    static void applyBorderlessFullscreen(quintptr hwnd);
};
