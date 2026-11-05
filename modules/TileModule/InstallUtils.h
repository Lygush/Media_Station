// InstallUtils.h
// ─────────────────────────────────────────────────────────────────────────────
// Статические утилиты для установки игр — общий код, не зависящий от QObject.
//
// Используется из InstallManager (GUI-поток и фоновые потоки).
// Не содержит состояния — все функции чистые или работают с файловой системой.
//
// Функции без побочных эффектов (parseDependencyAnalyzerOutput и др.)
// намеренно вынесены сюда для удобства написания unit-тестов: их можно
// вызвать напрямую без QApplication, EventBus или любого другого окружения.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>
#include <QStringList>
#include <memory>
#include <atomic>

namespace InstallUtils {

/// Паттерны имён exe-файлов, которые не показываем пользователю как вариант запуска.
extern const QStringList kSkipExePatterns;

// ─────────────────────────────────────────────────────────────────────────────
// Результат парсинга вывода PEDependencyAnalyzer
// ─────────────────────────────────────────────────────────────────────────────

struct DepsResult {
    /// DLL-файлы из секции "Other Dependencies", у которых НЕ найден путь.
    /// Пустой список означает что все зависимости найдены.
    QStringList missing;

    /// Типы ввода, обнаруженные по именам dll во всех секциях.
    /// Возможные значения: "xinput", "directinput".
    QStringList inputTypes;
};

/// Парсит текстовый вывод PEDependencyAnalyzer и возвращает результат.
///
/// Чистая функция — не читает файлов, не запускает процессов, не эмитирует
/// сигналов. Подходит для прямого вызова в unit-тестах:
///
///   auto r = InstallUtils::parseDependencyAnalyzerOutput(rawOutput);
///   QVERIFY(r.missing.isEmpty());
///   QCOMPARE(r.inputTypes, QStringList{"xinput"});
///
/// Логика секций:
///   - xinput/dinput детектируются по подстроке в имени dll в ЛЮБОЙ секции.
///   - missing собираются ТОЛЬКО из "Other Dependencies", у которых нет пути.
///   - dll из "Other Dependencies" с непустым путём считается найденной
///     независимо от её расположения на диске.
DepsResult parseDependencyAnalyzerOutput(const QString &output);

/// Поблочное копирование с поддержкой отмены через cancelFlag.
/// Возвращает false если файл не скопирован (отмена или ошибка ввода/вывода).
bool copyFileCancelable(const QString &src,
                        const QString &dst,
                        const std::shared_ptr<std::atomic<bool>> &cancelFlag,
                        qint64 chunkSize = 1 * 1024 * 1024);

/// Безопасное имя папки из названия игры (Windows): заменяет недопустимые и
/// управляющие символы, убирает точки/пробелы в конце, обходит
/// зарезервированные имена (CON, NUL, COM1…). Пустой результат заменяется на
/// "Game_<первые 8 символов fallbackId>" — иначе игра ставилась бы прямо в
/// корень каталога игр.
QString sanitizeFolderName(const QString &title, const QString &fallbackId = {});

/// true, если папка похожа на папку игры, созданную MediaStation
/// (есть launcher/launch.ps1, tile_id.txt или game_info.json).
/// Используется как защита перед рекурсивным удалением.
bool isManagedGameFolder(const QString &folderPath);

/// Возвращает уникальное имя папки внутри gamesDirPath на основе baseName.
/// Если baseName уже занят — добавляет " (1)", " (2)" и т.д.
QString generateUniqueFolderName(const QString &baseName,
                                 const QString &gamesDirPath);

/// Записывает tileId в <gameRootPath>/tile_id.txt (legacy-файл).
void writeTileIdFile(const QString &gameRootPath, const QString &tileId);

/// Атомарно записывает game_info.json в корень папки игры (.tmp → rename).
/// type:     "installed" | "external"
/// exePath:  для installed — относительный путь внутри game/,
///           для external — абсолютный путь к exe.
/// iconPath: абсолютный путь к иконке в launcher/.
void writeGameInfoFile(const QString &gameRootPath,
                       const QString &tileId,
                       const QString &title,
                       const QString &type,
                       const QString &exePath,
                       const QString &iconPath);

/// Создаёт <gameRootPath>/launcher/launch.ps1.
/// kind: "app" (exe внутри game/) | "external" (абсолютный путь).
/// Возвращает нативный путь к ps1 или пустую строку при ошибке.
QString createLaunchScript(const QString &gameRootPath,
                           const QString &exePathOrRelative,
                           const QString &kind);

/// Детерминированный ID папки: первые 12 символов MD5 от нормализованного пути.
/// Дублируется из GameLibraryModule — чтобы модули не зависели друг от друга.
QString makeFolderGameId(const QString &folderPath);

} // namespace InstallUtils
