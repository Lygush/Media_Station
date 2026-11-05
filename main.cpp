// main.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Точка входа MediaStation.
//
// Порядок регистрации модулей определяет порядок завершения (обратный).
// Правило: первым завершается тот, кто держит глобальные ресурсы ОС.
//
// Порядок завершения (от первого к последнему):
//   1. G72KeysModule      — сразу снимает хуки мыши/клавиатуры WH_LL
//   2. AndroidTVModule    — убивает эмулятор и adb
//   3. JoystickModule     — останавливает DS4Windows
//   4. G72BatteryModule   — завершает WinRT-поток
//   5. OverlayModule      — нет блокирующих операций
//   6. GameLibraryModule  — отменяет активные задачи 7zr
//   7. GameMediaModule    — отменяет загрузки
//   8. GameInfoModule     — освобождает llama.cpp
//   9. UpdateManager      — нет блокирующих операций
//  10. NotificationModule — нет блокирующих операций
//  11. AppController      — останавливает GameWatcher
//  12. NetworkModule      — закрывает TCP/UDP сокеты
//  13. TileManager        — отменяет активные установки
//  14. InstallManager     — нет блокирующих операций после TileManager
//  15. KaraokeModule      — отменяет обработку, ждёт завершения потока
//  16. LoggerModule       — последним, чтобы поймать ошибки завершения
// ─────────────────────────────────────────────────────────────────────────────

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QCursor>
#include <QPixmap>
#include <QDebug>
#include <QWindow>
#include <QTimer>
#include <QElapsedTimer>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

#include "core/ModuleHost.h"
#include "InstallManager.h"
#include "TileManager.h"
#include "NetworkModule.h"
#include "AppController.h"
#include "JoystickModule.h"
#include "NotificationModule.h"
#include "G72BatteryModule.h"
#include "G72KeysModule.h"
#include "GameLibraryModule.h"
#include "AndroidTVModule.h"
#include "LoggerModule.h"
#include "OverlayModule.h"
#include "UpdateManager.h"
#include "GameInfoModule.h"
#include "GameMediaModule.h"
#include "KaraokeModule.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    const QPixmap cursorPixmap(":/assets/cursor/cursor_32.png");
    if (!cursorPixmap.isNull()) {
        app.setOverrideCursor(QCursor(cursorPixmap, 8, 4));
    } else {
        qWarning() << "main: cursor asset not found, using system default";
    }

    // Движок создаётся в куче и удаляется явно ДО ModuleHost: иначе (порядок
    // деструкторов стековых объектов) модули удалялись раньше QML-движка, и
    // при выходе биндинги обращались к уже уничтоженным контекст-объектам.
    auto *enginePtr = new QQmlApplicationEngine;
    QQmlApplicationEngine &engine = *enginePtr;
    qmlRegisterSingletonType(QUrl("qrc:/qml/Settings.qml"), "AppSettings", 1, 0, "Settings");

    // Раньше здесь был жёстко прописан путь C:/Qt/6.10.2-msvc-full/qml: на другой
    // машине/версии Qt он либо отсутствует, либо подмешивает чужие QML-плагины.
    // QML-модули ищутся рядом с exe (windeployqt) и в стандартных путях Qt.

    ModuleHost host(&engine);

    // ── Зависимости между модулями ────────────────────────────────────────────
    auto *installManager = new InstallManager(&host);
    auto *tileManager    = new TileManager(&host);
    tileManager->setInstallManager(installManager);

    auto *androidTVModule = new AndroidTVModule(&host);

    // ── Регистрация в порядке инициализации ───────────────────────────────────
    // Завершение идёт в ОБРАТНОМ порядке — см. комментарий выше.

    // Завершается последним — логгер должен пережить всех
    host.registerModule(new LoggerModule(&host));

    // Завершаются предпоследними — файловые операции и сеть
    host.registerModule(installManager);
    host.registerModule(tileManager);
    host.registerModule(new NetworkModule(&host));

    // Завершается после сети — может писать события при закрытии игры
    host.registerModule(new AppController(&host));

    // Завершается после AppController — уведомления могут прийти при закрытии
    host.registerModule(new NotificationModule(&host));

    // Завершается после NotificationModule — может отправить уведомление
    host.registerModule(new UpdateManager(&host));

    // Завершается после UpdateManager — освобождает llama.cpp (медленно)
    host.registerModule(new GameInfoModule(&host));

    // Завершается после GameInfoModule — отменяет загрузки медиа
    host.registerModule(new GameMediaModule(&host));

    // Завершается после GameMediaModule — отменяет задачи архивации
    host.registerModule(new GameLibraryModule(&host));

    // Завершается после GameLibraryModule — скрывает оверлей
    host.registerModule(new OverlayModule(&host));

    // Завершается после OverlayModule — WinRT Bluetooth поток
    host.registerModule(new G72BatteryModule(&host));

    // Завершается после G72BatteryModule — останавливает DS4Windows
    host.registerModule(new JoystickModule(&host));

    // Завершается после JoystickModule — убивает эмулятор и adb
    host.registerModule(androidTVModule);

    // Завершается после AndroidTVModule — отменяет обработку аудио
    host.registerModule(new KaraokeModule(&host));

    // Завершается ПЕРВЫМ — немедленно снимает хуки WH_MOUSE_LL и WH_KEYBOARD_LL,
    // устраняя дёргание мыши во время завершения остальных модулей
    host.registerModule(new G72KeysModule(&host));

    host.initializeAll();

    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "main: failed to load Main.qml";
        host.shutdownAll();
        delete enginePtr;
        return -1;
    }

    QWindow *mainWindow = qobject_cast<QWindow*>(engine.rootObjects().first());
    if (mainWindow) {
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(mainWindow->winId());
        // WS_EX_LAYERED нужен AndroidTVModule для прозрачности поверх эмулятора.
        // TOPMOST здесь НЕ ставим: AndroidTVModule включает его только на время
        // сессии Android TV, иначе окна запущенных игр оказывались бы под MediaStation.
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        androidTVModule->setMediaStationHwnd(hwnd);
        qDebug() << "main: main window configured as Layered, HWND =" << hwnd;
#endif
    } else {
        qWarning() << "main: failed to get main window for Layered setup";
    }

    // ── Сторож отзывчивости интерфейса ──────────────────────────────────────
    // Тик каждые 100 мс в GUI-потоке. Если очередной тик опоздал больше чем на
    // 250 мс — поток интерфейса был занят (синхронная загрузка, тяжёлый JS,
    // создание плеера…): пишем в журнал, сколько и на каком экране. Это даёт
    // данные о «фризах» с реальной машины, а не догадки.
    QTimer stallTimer;
    QElapsedTimer stallClock;
    stallClock.start();
    stallTimer.setInterval(100);
    QObject::connect(&stallTimer, &QTimer::timeout, [&]() {
        const qint64 gap = stallClock.restart();
        // Пока на переднем плане игра/эмулятор, система вправе притормаживать
        // наш процесс — это не фриз интерфейса
        if (gap < 350 || QGuiApplication::applicationState() != Qt::ApplicationActive) return;
        QString screen;
        if (QObject *root = engine.rootObjects().value(0)) {
            if (QObject *stack = root->findChild<QObject *>(QStringLiteral("mainStack"))) {
                if (QObject *cur = stack->property("currentItem").value<QObject *>())
                    screen = cur->property("screenTitle").toString();
            }
        }
        Logger::write(gap > 1000 ? Logger::kLevelWarning : Logger::kLevelInfo, "UI",
                      QString("interface was unresponsive for %1 ms (screen: %2)")
                          .arg(gap - 100).arg(screen.isEmpty() ? QStringLiteral("home") : screen));
    });
    // Первые секунды — загрузка экрана и модулей, не считаем
    QTimer::singleShot(3000, &stallTimer, [&]() { stallClock.restart(); stallTimer.start(); });

    const int rc = app.exec();
    stallTimer.stop();

    // 1) Модули завершают внешние процессы (эмулятор, DS4Windows, хуки), пока UI жив
    host.shutdownAll();
    // 2) Удаляем QML до модулей, на которые он ссылается
    delete enginePtr;
    return rc;
}
