# MediaStation — Архитектура проекта

> Структура · Ответственность · Зависимости · Влияние изменений  
> **Версия от 21 марта 2026 (rev 2)**

---

## Содержание

1. [Общая архитектура](#1-общая-архитектура)
2. [Дерево проекта](#2-дерево-проекта)
3. [Ядро (core/)](#3-ядро-core)
4. [Модули C++](#4-модули-c)
5. [QML слой](#5-qml-слой)
6. [Точка входа: main.cpp](#6-точка-входа-maincpp)
7. [Карта зависимостей](#7-карта-зависимостей)
8. [Типичные сценарии изменений](#8-типичные-сценарии-изменений)
9. [Быстрый справочник](#9-быстрый-справочник)
10. [Ресурсы (.qrc файлы)](#10-ресурсы-qrc-файлы)
11. [Формат game_info.json](#11-формат-game_infojson)
12. [Структура папки игры](#12-структура-папки-игры)

---

## 1. Общая архитектура

Проект построен по **модульной архитектуре**. C++ слой содержит ядро (`core/`) и модули бизнес-логики, QML слой — весь UI. Взаимодействие между C++ модулями происходит **исключительно через EventBus**. Модули не знают друг о друге напрямую.

### Ключевые свойства архитектуры

- **Потокобезопасность**: фоновые модули (`G72BatteryModule`, `InstallManager`, `GameLibraryModule`) используют Qt-сигналы с автоматическим `QueuedConnection` и явным флагом занятости. Все `applicationDirPath()` кешируются в GUI-потоке при старте.
- **Жизненный цикл**: `ModuleHost` управляет порядком инициализации и завершения. `shutdown()` у каждого модуля вызывается в порядке, обратном регистрации, до уничтожения объектов.
- **Протокол событий**: типизированные сигналы EventBus — для межмодульных событий; `moduleEvent()` — для расширения без правок `EventBus.h`. Протоколы `moduleEvent` закреплены как именованные константы в namespace соответствующего модуля (например, `Notifications::kModule`, `Notifications::makeEvent()`).

### Схема уровней

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  QML                                                                          │
│  Main · HomeScreen · DynamicScreen · Tile · TileContextMenu                  │
│  EditTileDialog · DeleteConfirmDialog · FileBrowserDialog                    │
│  InstallRequestDialog · JoystickBar · SettingsScreen · PowerMenu             │
│  Settings · Notification · G72BatteryWidget · GameLibraryScreen             │
├──────────────────────────────────────────────────────────────────────────────┤
│  Модули C++                                                                   │
│  TileManager · NetworkListener · AppController (+GameWatcher)                │
│  JoystickModule · SettingsManager · NotificationModule · G72BatteryModule    │
│  GameLibraryModule                                                            │
├──────────────────────────────────────────────────────────────────────────────┤
│  Ядро (core/)                                                                 │
│  ModuleHost · EventBus · IModule                                             │
├──────────────────────────────────────────────────────────────────────────────┤
│  Вспомогательные классы (не IModule)                                          │
│  InstallManager · GameWatcher                                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Дерево проекта

```
MediaStation/
│
├── core/
│   ├── IModule.h / IModule.cpp
│   ├── ModuleHost.h / ModuleHost.cpp
│   └── EventBus.h
│
├── modules/
│   ├── AppModule/
│   │   ├── AppController.h / .cpp
│   │   └── GameWatcher.h / .cpp
│   ├── TileModule/
│   │   ├── TileManager.h / .cpp
│   │   └── InstallManager.h / .cpp
│   ├── NetworkModule/
│   │   └── NetworkListener.h / .cpp
│   ├── JoystickModule/
│   │   └── JoystickModule.h / .cpp
│   ├── SettingsModule/
│   │   └── SettingsManager.h / .cpp
│   ├── NotificationModule/
│   │   └── NotificationModule.h / .cpp
│   ├── AirMouseModule/
│   │   └── G72BatteryModule.h / .cpp
│   └── GameLibraryModule/
│       ├── GameLibraryModule.h
│       └── GameLibraryModule.cpp
│
├── main.cpp
│
├── qml/
│   ├── Main.qml
│   ├── Settings.qml               # синглтон
│   ├── HomeScreen.qml
│   ├── DynamicScreen.qml
│   ├── SettingsScreen.qml
│   ├── Tile.qml
│   ├── TileContextMenu.qml
│   ├── EditTileDialog.qml
│   ├── DeleteConfirmDialog.qml
│   ├── FileBrowserDialog.qml
│   ├── InstallRequestDialog.qml
│   ├── JoystickBar.qml
│   ├── PowerMenu.qml
│   ├── Notification.qml
│   ├── G72BatteryWidget.qml
│   └── GameLibraryScreen.qml
│
├── translation/
│   └── i18n.js
│
├── assets/
│   ├── background.png
│   ├── cursor/
│   └── icons/
│
├── fonts/
│   └── Montserrat-SemiBold.ttf
│
├── tools/
│   ├── DS4WindowsCmd.exe
│   ├── PEDependencyAnalyzer.exe
│   └── 7zr.exe                    ← требуется для архивирования
│
├── qml.qrc
├── assets.qrc
└── MediaStation.pro
```

> `INCLUDEPATH` в `.pro` содержит все папки `modules/*`, поэтому `#include "GameLibraryModule.h"` работает без изменений.

---

## 3. Ядро (core/)

| Файл | Роль |
|------|------|
| `IModule.h / IModule.cpp` | Базовый интерфейс модуля: `initialize`, `shutdown`, `qmlObject`, `registerWithQml` |
| `ModuleHost.h / ModuleHost.cpp` | Реестр и жизненный цикл модулей. Владеет `EventBus` и `SettingsManager`. Гарантирует однократный `shutdown` через флаг `m_shuttingDown` |
| `EventBus.h` | Шина событий: `networkExtractFinished`, `settingChanged`, `moduleEvent` |

### EventBus: два уровня событий

**Типизированные сигналы** добавляются только когда событие затрагивает несколько существующих модулей одновременно:

```cpp
// NetworkListener → TileManager
void networkExtractFinished(tileId, title, gameRootPath, exeRelPath, iconRelPath, kind);

// ModuleHost → все модули
void settingChanged(key, value);
```

**`moduleEvent(module, event, data)`** — для новых модулей без правки `EventBus.h`:

```cpp
// Уведомление из любого модуля:
emit bus->moduleEvent(
    Notifications::kModule,
    Notifications::kEventShow,
    Notifications::makeEvent("Текст", NotificationLevel::Warning));

// GameLibraryModule → TileManager:
emit bus->moduleEvent("gameLibrary", "createTile",        data);
emit bus->moduleEvent("gameLibrary", "removeTiles",       data);
emit bus->moduleEvent("gameLibrary", "queryTileCounts",   data);

// TileManager → GameLibraryModule (ответы):
emit bus->moduleEvent("tileManager", "tileCountChanged",  data);
```

### Добавление нового модуля — два файла

```cpp
// modules/MyModule/MyModule.h
#include "core/IModule.h"
class MyModule : public QObject, public IModule {
    Q_OBJECT
public:
    QString  moduleId() const override { return "mymodule"; }
    void     initialize(EventBus *bus, SettingsManager *s) override;
    QObject *qmlObject() override { return this; }
    QString  qmlName()   const override { return "MyModule"; }
};

// main.cpp — одна строка:
host.registerModule(new MyModule(&host));
```

---

## 4. Модули C++

### TileManager (`modules/TileModule/`)

Отвечает за дерево плиток и файл `structure.json`. Логика установки полностью делегирована `InstallManager`.

**Основные обязанности:**
- Чтение и запись `structure.json` (атомарная запись через `.tmp` → rename).
- Методы работы с деревом: `loadScreen`, `saveFolder`, `listFolderTiles` и т.д.
- Проброс сигналов `InstallManager` в QML.
- Подписка на `networkExtractFinished` → вызов `m_installer.finalizeInstallation`.
- Подписка на `moduleEvent("gameLibrary", ...)` → создание/удаление плиток по запросу библиотеки.
- Эмиссия `tileStructureChanged(storageKey, isFolder)` → `DynamicScreen` перезагружает модель.
- `shutdown()`: явная отмена всех активных задач копирования до уничтожения объектов.

**Сигналы:** `installationProgress` · `installationFinished` · `installationCancelled` · `exeScanReady` · `exeDescriptionReady` · `dependencyCheckReady` · `launchExeUpdated` · `tileStructureChanged`

**Протокол событий GameLibraryModule → TileManager:**

```cpp
// createTile: data = { gameId, storageKey, isFolder, title, iconPath, ps1Path, installPath }
// Создаёт плитку с id "lib_<gameId>" в указанном экране/папке.
// После создания эмитирует tileCountChanged и tileStructureChanged.

// removeTiles: data = { gameId, tileLocations: [...] }
// Удаляет плитку "lib_<gameId>" и все id из tileLocations.
// После удаления эмитирует tileCountChanged и tileStructureChanged(broadcast).

// queryTileCounts: data = { entries: [{ gameId, tileLocations }, ...] }
// Проверяет "lib_<gameId>", при нулевом результате — tileLocations (legacy).
// Отвечает пачкой tileCountChanged для каждой игры.
```

**Влияние изменений:**
- Изменение `structure.json` → правки в `DynamicScreen` (loadData/saveAll).
- Логика установки → только `InstallManager`.

---

### InstallManager (`modules/TileModule/`)

> Не является `IModule`. Живёт как приватный член `TileManager`.

Вся логика установки игр, создания ярлыков, сканирования exe и проверки зависимостей.

| Метод | Описание |
|-------|----------|
| `startInstallation` | Асинхронное копирование с поддержкой отмены (`QtConcurrent` + `atomic<bool>`) |
| `cancelAllInstallations` | Отмена всех активных задач и ожидание их завершения |
| `finalizeInstallation` | Финализация сетевой установки после распаковки 7z |
| `createExternalShortcut` | Создание ярлыков на внешние приложения |
| `prepareInstallDir` | Подготовка каталога для сетевой установки |
| `deleteInstalledGame` | Удаление установленной игры |
| `scanExeFiles` | Асинхронное сканирование exe в папке |
| `getExeDescription` | Описание exe через PowerShell VersionInfo |
| `updateLaunchExe` | Смена exe → пересоздаёт `launch.ps1`, обновляет `game_info.json` |
| `checkDependencies` | Проверка зависимостей через `PEDependencyAnalyzer.exe` |
| `writeGameInfoFile` | Запись `game_info.json` при каждой установке (атомарная) |
| `makeFolderGameId` | Детерминированный ID папки (MD5 от пути, 12 символов) |

**Примечания:**
- `applicationDirPath()` кешируется в `m_appDir` при конструировании (в GUI-потоке). Фоновые лямбды читают `m_appDir`.
- `cancelAllInstallations()` вызывается как из `TileManager::shutdown()`, так и из деструктора — защита от двойного вызова встроена.
- `writeGameInfoFile()` вызывается в трёх точках: `startInstallation`, `finalizeInstallation`, `createExternalShortcut`. Формат — см. [раздел 11](#11-формат-game_infojson).
- `makeFolderGameId()` генерирует тот же ID что и `GameLibraryModule::makeGameId()` — оба вычисляют MD5 от нормализованного пути. Дублирование намеренное — модули не должны зависеть друг от друга.

---

### NetworkListener (`modules/NetworkModule/`)

TCP-сервер, принимающий и распаковывающий пакеты установки.

- `prepareInstallDir` намеренно дублирует одноимённый метод `InstallManager` — для изоляции модуля.
- `shutdown()`: закрывает все активные соединения, останавливает `QProcess` распаковки, закрывает TCP-сервер и UDP-сокет.

---

### AppController + GameWatcher (`modules/AppModule/`)

Управляет запуском приложений и игр. `GameWatcher` — внутренний класс (не `IModule`), наблюдающий за окном запущенной игры.

**Таймеры:**

| Константа | Значение | Назначение |
|-----------|----------|------------|
| `kWindowFallbackMs` | 5 сек | Страховочный таймер при потере окна игры |
| `kWatchdogMs` | 20 сек | Максимальное время скрытого taskbar |

**Особенности:**
- Запуск `.exe/.bat/.cmd` через временный PS1-скрипт `makeTempLaunchScript()` (самоудаляющийся).
- `GameWatcher`: PowerShell + PID-файл, `WaitForInputIdle`, поиск HWND по PID.
- `borderless fullscreen` для игр без собственного управления режимом (`hasOwnFullscreen`).

---

### JoystickModule (`modules/JoystickModule/`)

Управляет подключением геймпадов и взаимодействием с DS4Windows. Активируется при старте приложения.

---

### SettingsManager (`modules/SettingsModule/`)

Хранит и предоставляет доступ ко всем настройкам через `QSettings`. Изменения пробрасываются в `EventBus::settingChanged` через `ModuleHost`.

| Ключ | Тип | Значение по умолчанию |
|------|-----|-----------------------|
| `accentColor` | string | `#87CEEB` |
| `backgroundImage` | path | `""` (фоллбек → `qrc:/assets/background.png`) |
| `fullscreen` | bool | `false` |
| `language` | string | `"ru"` |
| `installPath` | path | `""` — папка установки игр и библиотеки |
| `uiScale` | double | `1.0` / `1.25` / `1.5` |
| `fontOffset` | int | `0`, диапазон `−4…+8`, шаг 2 |

> `installPath` — единственная папка для всего: установки игр, библиотеки и архивов. Архивы хранятся внутри папки каждой игры (`<game>/archive/<name>.7z`), отдельной настройки пути к архивам нет.

> Добавление новой настройки: геттер + сеттер + сигнал в `SettingsManager` + connect в `ModuleHost` + прокси в `Settings.qml`.

---

### NotificationModule (`modules/NotificationModule/`)

Принимает события через `EventBus::moduleEvent` и управляет очередью уведомлений с приоритетами.

**Отправка уведомления из любого модуля:**

```cpp
#include "NotificationModule.h"

emit bus->moduleEvent(
    Notifications::kModule,
    Notifications::kEventShow,
    Notifications::makeEvent("Установка завершена", NotificationLevel::Info, true, 4000));
```

Константы `Notifications::kModule`, `Notifications::kEventShow` и фабрика `Notifications::makeEvent()` объявлены в `NotificationModule.h` — единственный источник правды по протоколу.

**Логика очереди:**
- Одно уведомление на экране, остальные в очереди.
- Уведомление строго выше уровнем (`Error > Warning > Info`) вытесняет текущее немедленно; текущее возвращается в начало очереди.
- `INFO`-уведомления при вытеснении теряются (не важны).

**Сигналы:** `showRequested()` · `hideRequested()` · `visibleChanged` · `currentChanged`

---

### G72BatteryModule (`modules/AirMouseModule/`)

Чтение заряда пульта G72 по BLE (WinRT), фоновый поток с синхронными `.get()` вызовами.

**Структура:**
- `G72BatteryWorker` — в `QThread`, содержит WinRT-объекты.
- `G72BatteryModule` — в GUI-потоке, управляет потоком и таймерами.

**Алгоритм:**
1. Быстрое подключение по MAC `DC:6D:86:30:E0:ED`.
2. Если не удалось — поиск по имени `"G72"`.
3. Сервис Battery `0x180F`, характеристика `0x2A19`.
4. Сырое значение `0x87…0xBB` → проценты (Li-ion таблица).
5. Детектор просадки + EMA-фильтрация (α = 0.3).

---

### GameLibraryModule (`modules/GameLibraryModule/`)

Модуль библиотеки игр. Сканирует `installPath` из настроек, управляет метаданными (`game_info.json`), архивирует и разархивирует игры.

**Основные обязанности:**
- Сканирование `installPath` → список игр с метаданными.
- Чтение/запись `game_info.json` (атомарная запись через `.tmp` → rename).
- Генерация `game_info.json` для legacy-установок (только с `tile_id.txt`).
- Архивирование: 7zr LZMA2 `-mx=9 -mqs=on`, асинхронно с прогрессом и отменой.
- Разархивирование: 7zr x, асинхронно с прогрессом и отменой.
- Уведомление TileManager через шину о создании/удалении ярлыков.
- Синхронизация счётчика ярлыков через `queryTileCounts` / `tileCountChanged`.
- Уведомления пользователя через NotificationModule при завершении операций.
- Кэширование размеров (асинхронный подсчёт, сохраняется в `game_info.json`).

**API для QML:**

| Метод | Описание |
|-------|----------|
| `scanLibrary()` | Асинхронное сканирование `installPath`. Пропускает папки с активными задачами |
| `currentEntries()` | Текущее состояние `m_entries` без скана диска (для восстановления UI) |
| `requestFolderSize(gameId, path)` | Асинхронный подсчёт размера папки или файла |
| `createTile(gameId, storageKey, isFolder)` | Отправить событие на создание ярлыка |
| `archiveGame(gameId)` | Архивировать `game/` → `archive/<name>.7z`, удалить `game/` |
| `cancelArchive(gameId)` | Отменить архивирование, восстановить ярлыки |
| `extractGame(gameId, keepArchive, storageKey, isFolder)` | Разархивировать `archive/<name>.7z` → `game/` |
| `cancelExtract(gameId)` | Отменить распаковку, удалить частичный результат |
| `removeFromLibrary(gameId)` | Удалить только `game_info.json` (метаданные) |
| `deleteGame(gameId)` | Физически удалить папку игры + ярлыки через шину |
| `openFolder(path)` | Открыть папку в Explorer |
| `reassignPath(gameId, newExePath)` | Переназначить путь к exe |
| `tileCount(gameId)` | Количество ярлыков (синхронизируется через шину) |

**Сигналы:**

| Сигнал | Когда |
|--------|-------|
| `scanFinished(entries)` | Сканирование завершено |
| `folderSizeReady(gameId, bytes)` | Размер подсчитан |
| `tileCreated(gameId, success)` | Ярлык создан |
| `archivingChanged()` | Изменилось наличие активных задач архивирования |
| `archiveProgress(gameId, progress, currentFile)` | Ход архивирования |
| `archiveFinished(gameId, success)` | Архивирование завершено |
| `extractProgress(gameId, progress)` | Ход распаковки |
| `extractFinished(gameId, success)` | Распаковка завершена |
| `libraryEntryChanged(gameId, entry)` | Метаданные игры обновлены |

**QML-свойства:**

| Свойство | Тип | Значение |
|----------|-----|----------|
| `scanning` | bool | true во время сканирования |
| `archiving` | bool | true если есть активные задачи архивирования |

**Алгоритм сканирования одной папки:**
1. Есть `game_info.json` → читаем. Определяем `available`: для `installed` — наличие непустой `game/`, для `archive` — наличие файла `archive/<name>.7z`, для `external` — наличие exe по пути.
2. Нет `game_info.json`, есть `tile_id.txt` → генерируем `game_info.json` автоматически, `legacyNoMetadata: true`. Тип определяется по наличию `archive/<name>.7z` или `game/`.
3. Ничего → папка игнорируется.
4. Папки с активными задачами (`m_archiveTasks`, `m_extractTasks`) пропускаются при скане.

**Алгоритм архивирования:**
1. Сохраняем `tileLocations` в `_savedTileLocs`.
2. Удаляем ярлыки: `moduleEvent("gameLibrary", "removeTiles", ...)`.
3. Создаём `archive/`, запускаем `7zr a` с рабочей директорией `installPath`, архивируем подпапку `game/`.
4. При успехе: обновляем `game_info.json` (`type → archive`), удаляем `game/`. Папка игры остаётся.
5. При отмене: удаляем неполный архив, восстанавливаем ярлыки через `createTile`.
6. При ошибке: удаляем неполный архив, `game/` не трогаем.

**Алгоритм разархивирования:**
1. Запускаем `7zr x archive/<name>.7z` с рабочей директорией `installPath` — восстанавливает `game/` на месте.
2. При успехе: обновляем `game_info.json` (`type → installed`), удаляем архив (если `keepArchive=false`), удаляем пустую `archive/`, создаём ярлык если указан `storageKey`, запрашиваем пересчёт размера `game/`.
3. При отмене: убиваем процесс, удаляем частично восстановленную `game/`.

**Алгоритм удаления игры (`deleteGame`):**
- `installed` / `archive` → удаляем всю папку игры (`QDir::removeRecursively`).
- `external` → удаляем только `game_info.json`, файлы игры не трогаем.
- Во всех случаях: удаляем ярлыки через шину.

**Потокобезопасность:**
- Сканирование — `QtConcurrent::run`, результат через `QMetaObject::invokeMethod(..., QueuedConnection)`.
- Архивирование — `QFutureWatcher<bool>` + `std::atomic<bool>` cancelFlag.
- Извлечение — аналогично архивированию, трекается в `m_extractTasks`.
- `m_appDir` кешируется в конструкторе (GUI-поток).
- `shutdown()` отменяет все активные задачи архивирования и извлечения.

---

## 5. QML слой

### Settings.qml (синглтон)

Прокси для всех настроек `SettingsManager`. Функции `us(base)` и `fs(base)` используются во всех QML-файлах для масштабирования размеров и подстройки шрифта.

- `backgroundImage` — с фоллбеком на `qrc:/assets/background.png` если путь пустой.
- `installPath` — единственная папка для игр и библиотеки.

---

### Main.qml

- `G72BatteryWidget` в верхней панели.
- `Notification` внизу по центру.
- `JoystickBar` виден только на экране games, анимация выезда справа.
- `JoystickModule.activate()` при старте приложения.
- Кнопка `GameLibraryScreen` рядом с `PowerMenu`. Свойство `libraryOpen` управляет активным состоянием.
- Фон: `Image` + `FastBlur` без `visible: false` — иначе FastBlur не получает пиксели в Qt6.

---

### DynamicScreen.qml

- Поддержка `installPhase`: `"packing"` / `"receiving"` / `"extracting"`.
- При удалении плитки учитывается `kind` — для `external` удаляется папка-ярлык.
- `TileManager.findInstallFolderByTileId` при удалении.
- Подписка на `TileManager.tileStructureChanged(storageKey, isFolder)` → перезагружает `tilesModel` если `storageKey` совпадает или пришёл broadcast (пустой storageKey).

---

### Tile.qml

- Режим `launching` — спиннер + пульсирующее свечение.
- `installPhase` — текст над прогресс-баром.
- `cancelZone` с обработкой hover и клика.
- Детектор долгого нажатия (1500 мс) → контекстное меню.

---

### TileContextMenu.qml

Три режима: `add`, `normal`, `installing`. Автозакрытие через 5 сек, если мышь не внутри.

---

### EditTileDialog.qml

- Размеры через `us()`, шрифты через `fs()`.
- Выбор `kind`: `web`, `app`, `playstation1`.
- Для `app`: выбор корневой папки, сканирование exe, автоподстановка описания.
- Чекбокс `installChecked` для `app`.
- Проверка зависимостей при создании внешнего ярлыка без установки.

---

### FileBrowserDialog.qml

- `filterExt` — фильтр по расширениям.
- Используется только внутри `EditTileDialog`.

---

### InstallRequestDialog.qml

Отображает дерево экранов и папок. Логика дерева переиспользована в `GameLibraryScreen`.

---

### GameLibraryScreen.qml

Экран библиотеки игр. Открывается кнопкой рядом с `PowerMenu`. Сканирование запускается автоматически при открытии.

**Структура:**
- Таблица: иконка · название · статус · размер (для `external` размер не показывается).
- Контекстное меню по удержанию ЛКМ ≥ 500 мс с визуальным откликом (нарастающий фон строки + полоска снизу).
- Диалог выбора места для ярлыка (переиспользует логику `InstallRequestDialog`). Работает и для создания ярлыка, и для разархивирования.
- Диалог подтверждения архивирования.
- Диалог подтверждения удаления игры (текст зависит от типа).
- `FileDialog` для переназначения пути недоступной игры.

**Статусы строк:**

| Статус | Цвет | Условие |
|--------|------|---------|
| Установлена | 🟢 зелёный | `type === "installed" && tileCount > 0` |
| Нет ярлыка | ⚫ серый | `type !== "archive" && available && tileCount === 0` |
| Внешний ярлык | 🔵 синий | `type === "external" && available && tileCount > 0` |
| Недоступна | 🟡 жёлтый | `available === false` |
| Архив | ⚫ серый | `type === "archive"` |
| Архивирование… | 🔵 синий | `archiving === true` |
| Распаковка… | 🔵 синий | `extracting === true` |

**Пункты контекстного меню по статусу:**

| Пункт | installed | external | unavailable | archive |
|-------|:---------:|:--------:|:-----------:|:-------:|
| Создать / Управлять ярлыками | ✓ | ✓ | ✓ | — |
| Архивировать | ✓ | — | — | — |
| Разархивировать | — | — | — | ✓ |
| Открыть папку | ✓ | ✓ | — | ✓ |
| Переназначить путь | — | — | ✓ | — |
| Удалить игру | ✓ | ✓ | ✓ | ✓ |

**Умное обновление модели (`_mergeModel`):**
- Вызывается из `onScanFinished` (вместо полного сброса).
- Сохраняет строки с активными задачами (`archiving`/`extracting`) — они пропущены при скане.
- Добавляет новые записи, обновляет существующие (кроме активных), удаляет исчезнувшие.
- `_fillModel` используется только при первом открытии экрана.

**Восстановление UI при повторном открытии:**
- `Component.onCompleted` вызывает `GameLibraryModule.currentEntries()` — получает текущее состояние включая флаги `archiving`/`extracting` и прогресс.
- Если данные есть — восстанавливает модель без скана и сразу запускает фоновый скан.
- Активные сигналы `archiveProgress`/`extractProgress` продолжают обновлять модель через `Connections`.

**Удержание ЛКМ:**
- `holdTimer` 500 мс, `preventStealing: true` блокирует прокрутку ListView.
- Визуальный отклик: отдельный прямоугольник-оверлей с `opacity` прямо привязанным к `_holdProgress` (без `Behavior` — синхронно с `NumberAnimation`). Полоска снизу аналогично.
- `onPositionChanged` отсутствует намеренно — сброс только по отпусканию кнопки (airmouse дрожит).

---

### JoystickBar.qml

Карточки подключённых контроллеров. Анимация выезда справа, батарея, тип подключения (USB/Bluetooth), анимация зарядки.

---

### PowerMenu.qml

Кнопки **Exit** и **Shutdown**. Автозакрытие через 3 сек.

---

### SettingsScreen.qml

Три секции: **Внешний вид** · **Язык** · **Установка**.
- Переключатель полноэкранного режима.
- Выбор масштаба UI: 1.0 / 1.25 / 1.5.
- Регулировка размера шрифта (− / +, шаг 2).
- Выбор языка (RU/EN).
- Поле пути установки с кнопкой «Обзор» — единственная папка для игр и архивов.

---

### Notification.qml

- Появляется снизу по центру, скрывается автоматически при `autoDismiss`.
- Цвета: `info` / `warning` / `error`.
- Прогресс-бар времени до скрытия.

---

### G72BatteryWidget.qml

- Иконка пульта и полоска заряда (зелёный / жёлтый / красный).
- Тултип с процентом.
- Анимация появления/исчезновения при подключении/отключении.

---

## 6. Точка входа: main.cpp

```cpp
ModuleHost host(&engine);
host.registerModule(new TileManager(&host));        // первым — подписка на networkExtractFinished и gameLibrary/*
host.registerModule(new NetworkListener(&host));
host.registerModule(new AppController(&host));
host.registerModule(new JoystickModule(&host));
host.registerModule(new NotificationModule(&host));
host.registerModule(new G72BatteryModule(&host));
host.registerModule(new GameLibraryModule(&host));  // последним — подписывается на tileManager/tileCountChanged
host.initializeAll();  // до engine.load()
```

Порядок важен: `TileManager` должен подписаться на `gameLibrary/*` до того как `GameLibraryModule` начнёт их эмитировать. `GameLibraryModule` должен быть после `NotificationModule` — он использует шину для уведомлений.

---

## 7. Карта зависимостей

### C++ → C++

| Файл / Модуль | Зависит от |
|---------------|-----------|
| `main.cpp` | ModuleHost, все модули |
| `ModuleHost` | IModule, EventBus, SettingsManager |
| `AppController + GameWatcher` | IModule, EventBus, Win32 API |
| `TileManager` | IModule, EventBus, InstallManager (внутренний) |
| `InstallManager` | QtConcurrent, QProcess, PEDependencyAnalyzer.exe |
| `NetworkListener` | IModule, EventBus |
| `JoystickModule` | IModule, EventBus, DS4WindowsCmd.exe |
| `SettingsManager` | QSettings (автономен) |
| `NotificationModule` | IModule, EventBus |
| `G72BatteryModule` | IModule, EventBus, WinRT (Windows.Devices.Bluetooth) |
| `GameLibraryModule` | IModule, EventBus, SettingsManager, QtConcurrent, QProcess, NotificationModule.h, 7zr.exe |

### QML → C++

| QML файл | Использует C++ объекты |
|----------|------------------------|
| `Main.qml` | AppController, NetworkListener, Settings, G72Battery |
| `DynamicScreen.qml` | TileManager, NetworkListener, JoystickModule, Settings |
| `EditTileDialog.qml` | TileManager, SettingsManager (installPath), Settings |
| `FileBrowserDialog.qml` | TileManager, AppController, Settings |
| `InstallRequestDialog.qml` | TileManager, NetworkListener, Settings |
| `JoystickBar.qml` | JoystickModule, Settings |
| `SettingsScreen.qml` | Settings (→ SettingsManager) |
| `Notification.qml` | NotificationModule, Settings |
| `G72BatteryWidget.qml` | G72Battery, Settings |
| `GameLibraryScreen.qml` | GameLibraryModule, TileManager, Settings |
| `Tile · TileContextMenu · DeleteConfirmDialog · HomeScreen · PowerMenu` | Settings (цвет, переводы) |

---

## 8. Типичные сценарии изменений

| Что меняю | Что затрагивает |
|-----------|----------------|
| Новый тип уведомления (уровень) | `NotificationLevel` (enum), `Notifications::levelFromString/levelToString`, `Notification.qml` |
| Отправить уведомление из нового модуля | `Notifications::makeEvent(...)` — больше ничего не трогать |
| Логика батареи G72 | `G72BatteryModule.cpp` (WinRT, фильтры), `G72BatteryWidget.qml` |
| Добавление настройки | `SettingsManager`, `Settings.qml` (прокси), `ModuleHost.cpp` (connect), QML-файлы где используется |
| Таймауты watchdog | `AppController.h` (`kWatchdogMs`, `kWindowFallbackMs`) |
| Новый тип ярлыка | `EditTileDialog.qml`, `TileManager` (kind), `InstallManager::createLaunchScript` |
| Формат `structure.json` | `TileManager` (load/save), `DynamicScreen` (loadData/saveAll) |
| TCP-протокол | `NetworkListener.cpp` (ClientPhase, readyRead) |
| Профили DS4Windows | `JoystickModule.cpp` (константы имён профилей) |
| Добавить новый модуль | Один новый файл пары `.h/.cpp` + одна строка в `main.cpp` |
| Изменить алгоритм сжатия | `GameLibraryModule::archiveGame()` — `QStringList args` для 7zr |
| Формат `game_info.json` | `GameLibraryModule` (read/write), `InstallManager::writeGameInfoFile`, раздел 11 |
| Новый статус в библиотеке | `GameLibraryScreen.qml` (`statusColors`, `statusKey`, `statusLabel`) + `i18n.js` |
| Строки UI библиотеки | `i18n.js` (ключи `lib_*`) |

---

## 9. Быстрый справочник

| Задача | Файл(ы) |
|--------|---------|
| Логика очереди уведомлений | `NotificationModule.cpp` (enqueue, вытеснение) |
| Протокол отправки уведомлений | `NotificationModule.h` (namespace Notifications) |
| Параметры фильтра батареи | `G72BatteryModule.h` (`EMA_ALPHA`, `DROP_THRESHOLD`, `VAR_WIN`) |
| MAC-адрес пульта G72 | `G72BatteryModule.cpp` (`kFallbackMac`) |
| Масштаб UI по умолчанию | `SettingsManager.cpp` (`uiScale()`, defaultValue) |
| Диапазон смещения шрифта | `SettingsManager.h` (`setFontOffset`, qBound) |
| Текст фазы установки | `Tile.qml` (_phaseLabel), `i18n.js` (ключи `phase_packing` и т.д.) |
| Новый внешний инструмент | `InstallManager.cpp` |
| Добавить уровень уведомления | `NotificationModule.h` (enum + levelFromString/levelToString) + `Notification.qml` |
| Порядок инициализации модулей | `main.cpp` |
| Путь к 7zr | `GameLibraryModule::sevenZipPath()` — `tools/7zr.exe` → `tools/7z.exe` → PATH |
| Параметры сжатия 7zr | `GameLibraryModule::archiveGame()` — `QStringList args` |
| Запись метаданных игры | `InstallManager::writeGameInfoFile()` |
| ID папки игры | `InstallManager::makeFolderGameId()` = `GameLibraryModule::makeGameId()` |
| Строки UI библиотеки | `i18n.js` (ключи `lib_*`) |
| Синхронизация счётчика ярлыков | `TileManager::initialize()` → `queryTileCounts` / `tileCountChanged` |
| Восстановление UI библиотеки | `GameLibraryModule::currentEntries()` |
| Стандартный путь к архиву | `GameLibraryModule::archivePathForGame(installPath)` |

---

## 10. Ресурсы (.qrc файлы)

### assets.qrc — иконки и ресурсы

`background.png` · `ic_bluetooth.png` · `ic_error.png` · `ic_info.png` · `ic_minus.png` · `ic_remote_control.png` · `ic_label.png` · `ic_folder.png` · `ic_settings.png` · `ic_back.png` · `ic_add.png` · `ic_power.png` · `ic_exit.png` · `ic_check.png` · `ic_up.png` · `ic_download.png` · `ic_usb.png` · `Montserrat-SemiBold.ttf` · курсоры

### qml.qrc — QML файлы

`Main` · `HomeScreen` · `DynamicScreen` · `Tile` · `TileContextMenu` · `EditTileDialog` · `DeleteConfirmDialog` · `FileBrowserDialog` · `InstallRequestDialog` · `JoystickBar` · `SettingsScreen` · `PowerMenu` · `Settings` · `Notification` · `G72BatteryWidget` · `GameLibraryScreen`

---

## 11. Формат game_info.json

Файл хранится в корне папки каждой игры рядом с `tile_id.txt`. Пишется в двух местах: `InstallManager` (при установке) и `GameLibraryModule` (при сканировании, архивировании, разархивировании).

```json
{
  "id":               "a3f1c8d9e2b4",
  "name":             "Half-Life 2",
  "type":             "installed",
  "executablePath":   "hl2.exe",
  "installPath":      "C:\\Games\\Half-Life 2",
  "iconPath":         "C:\\Games\\Half-Life 2\\launcher\\icon.png",
  "tileLocations":    ["games", "folder_abc123"],
  "archivePath":      "",
  "installDate":      "2026-03-21",
  "sizeCached":       15728640000,
  "legacyNoMetadata": false
}
```

| Поле | Тип | Описание |
|------|-----|----------|
| `id` | string | MD5(нормализованный путь)[0:12]. Детерминированный |
| `name` | string | Отображаемое название игры |
| `type` | string | `"installed"` / `"external"` / `"archive"` |
| `executablePath` | string | Для `installed` — относительный путь внутри `game/`. Для `external` — абсолютный путь к exe |
| `installPath` | string | Абсолютный путь к корневой папке игры |
| `iconPath` | string | Абсолютный путь к иконке в `launcher/` |
| `tileLocations` | array | `storageKey` экранов/папок где созданы ярлыки |
| `archivePath` | string | Путь к `.7z` архиву (`<installPath>/archive/<name>.7z`). Заполнен только при `type === "archive"` |
| `installDate` | string | ISO 8601, только дата (`YYYY-MM-DD`) |
| `sizeCached` | number | Размер в байтах (`-1` если не подсчитан). Для `installed` — размер `game/`, для `archive` — размер `.7z` файла |
| `legacyNoMetadata` | bool | `true` если файл сгенерирован автоматически из `tile_id.txt` |

### Жизненный цикл type

```
  [установка]
      │
      ▼
  installed ──────────────────────→ archive
  (startInstallation /           (archiveGame:
   finalizeInstallation /         7zr game/ → archive/<n>.7z
   createExternalShortcut)        удалить game/)
                                      │
  external ──────────────────────────┤
  (createExternalShortcut)           │  (только installed → archive,
   нельзя архивировать external)     │   external архивировать нельзя)
                                      ▼
                                  archive ──→ installed
                                            (extractGame:
                                             7zr x → game/
                                             удалить archive/ если !keepArchive)
```

### Атомарная запись

Оба модуля пишут через `.tmp` → rename:
1. Записать в `game_info.json.tmp`.
2. `QFile::remove(infoPath)`.
3. `QFile::rename(tmpPath, infoPath)`.

---

## 12. Структура папки игры

Все файлы одной игры хранятся внутри одной папки — ничего не выходит за её пределы.

```
<installPath>/
└── Half-Life 2/                    ← корневая папка игры (installPath)
    ├── game/                       ← игровые файлы (есть = installed)
    │   ├── hl2.exe
    │   └── ...
    ├── launcher/                   ← всегда присутствует
    │   ├── launch.ps1
    │   └── icon.png
    ├── archive/                    ← создаётся при архивировании
    │   └── Half-Life 2.7z          ← архив game/ (есть = archive)
    ├── game_info.json              ← метаданные (всегда)
    └── tile_id.txt                 ← legacy, только у старых установок
```

**Определение типа при сканировании:**
- Есть непустая `game/` → `installed`
- Есть `archive/<name>.7z`, нет `game/` → `archive`
- Нет ни того, ни другого, но есть `launcher/launch.ps1` → `external`

**Путь к архиву** всегда предсказуем: `installPath + "/archive/" + dirName + ".7z"`. Метод `archivePathForGame(installPath)` в `GameLibraryModule` возвращает этот путь.

**Что удаляется при операциях:**
- Архивирование → удаляется только `game/`, остальное остаётся
- Разархивирование → восстанавливается `game/`, удаляется `archive/` (если `!keepArchive`)
- Удаление игры (`installed`/`archive`) → удаляется вся папка целиком
- Удаление игры (`external`) → удаляется только `game_info.json`

---

*MediaStation Architecture · v21.03.2026 rev2*
