# MediaStation

Медиацентр для телевизора под Windows: плитки игр и приложений, установка игр по
локальной сети, запуск и отслеживание игр, Android TV в эмуляторе, караоке,
описания и обложки игр (локальная LLM + Steam), управление пультом G72 и
геймпадами DualShock (DS4Windows).

Qt 6 (C++20 + QML), сборка через qmake. Только Windows.

## Структура

```
MediaStation_root.pro      subdirs: приложение + Updater
MediaStation.pro           приложение
main.cpp                   точка входа: регистрация модулей, загрузка Main.qml
Version.h                  версия (MEDIASTATION_VERSION)
core/                      ModuleHost, EventBus, IModule — каркас модулей
modules/                   модули (по папке на модуль, см. ниже)
tools/                     общие утилиты (AtomicFile, HttpClient, SevenZip,
                           LlamaClient) + заголовки llama / whisper / onnxruntime
qml/                       интерфейс (Main.qml, экраны, диалоги, плитки)
assets/, fonts/,
translation/               ресурсы (assets.qrc, qml.qrc)
Updater/                   отдельный exe обновления (замена папки приложения)
external_tools/            DS4Windows, скрипт установки Android SDK
tests/                     автотесты (tests.pro), общие настройки — common.pri
documentation/             архитектура, журналы изменений
```

### Модули

| Папка | Что делает |
|---|---|
| `TileModule` | Плитки и их хранение (`structure.json`), локальная установка игр, ярлыки |
| `NetworkModule` | Приём установок игр и обновлений по сети (TCP 12345, UDP 12346), отправка логов |
| `AppModule` | Запуск игр (`launch.ps1`), отслеживание процесса и окна, панель задач |
| `GameLibraryModule` | Библиотека игр: сканирование, архивация в 7z и обратно, удаление |
| `GameInfoModule` | Описание игры: поиск в Steam + генерация локальной LLM |
| `GameMediaModule` | Обложки и трейлеры из Steam (ffmpeg для конвертации) |
| `KaraokeModule` | Караоке: разделение голоса (Demucs/ONNX), распознавание (Whisper), LRC |
| `AndroidTVModule` | Android TV в эмуляторе, встраивание окна, профили, adb |
| `JoystickModule` | Геймпады через DS4Windows, профили под тип ввода игры |
| `AirMouseModule` | Пульт G72: кнопки (глобальные хуки), батарея (WinRT, только MSVC) |
| `OverlayModule` | Оверлей поверх игры / Android TV |
| `NotificationModule` | Очередь уведомлений |
| `LoggerModule` | Журнал `logs/MediaStation.log` (ротация при 10 МБ); подробности Android TV — `logs/Android_TV.log` (+ `.prev.log` прошлого запуска) |
| `SettingsModule` | Настройки (`QSettings`) |
| `UpdateManager` | Приём обновления и запуск Updater |
| `GigaAM` | Альтернатива Whisper на ONNX. **Не подключена к сборке** (нужен kiss_fft) |

Подробнее — `documentation/MediaStation_Architecture.md`.

## Сборка

1. Qt 6 (Quick, Multimedia, Network, Concurrent), компилятор MSVC или MinGW.
2. Положить бинарные зависимости (в репозиторий не входят):
   - `tools/llama/lib`, `tools/whisper/lib`, `tools/onnxruntime/lib` — `.lib` и DLL;
   - рядом с собранным exe: `tools/7zr.exe`, `tools/ffmpeg/ffmpeg.exe`,
     модель LLM в `tools/llama/models/`, DS4Windows в `tools/DS4Windows/`.
3. Android SDK: `external_tools/setup_android_sdk.bat`
   (нужен `tools/cmdline-tools-latest-windows.zip`).
4. Открыть `MediaStation_root.pro` в Qt Creator и собрать.

Путь установки игр по умолчанию — `<папка приложения>/data/games`
(меняется в настройках).

## Тесты

```
cd tests
qmake tests.pro && make            # nmake / mingw32-make на Windows
```

Каждый тест — отдельный exe в своей папке. `tst_network_protocol` поднимает
сервер на порту 12345 (закройте MediaStation) и требует 7zr/7z рядом с тестом
или в PATH. `tst_g72battery_mock` собирается только MSVC (WinRT).
`tests/qml` — заготовка QML-тестов, в `tests.pro` не подключена.

### Снимки интерфейса (`tests/ui_snapshots`)

Загружает настоящий `Main.qml` с заглушками C++-модулей и сохраняет PNG основных
экранов и диалогов при разных размерах окна, масштабе, кегле и языке — чтобы
видеть наложения и обрезку текста без запуска полного приложения.

```
cd tests/ui_snapshots && qmake && make
xvfb-run -a -s "-screen 0 2600x1500x24" ui_snapshots <папка_для_png> 1280x720 1920x1080
```
По умолчанию рисует через OpenGL (видны тени, маски, размытие); `SNAP_SOFTWARE=1`
— программный рендер без Xvfb (быстрее, но эффекты слоёв не видны).
Рядом с каждым снимком — отчёт `.txt` автоматической проверки текстов
(OVERFLOW / OUTSIDE / OFFSCREEN / TIGHT / ELIDED), `SNAP_PERF=1` печатает время
открытия экрана и худший кадр, в конце — проверка, что закрытые экраны
уничтожены.
Данные заглушек (`mocks/Mocks.qml`) нарочно «неудобные»: длинные названия,
много плиток. Быстрый прогон части снимков:
`SNAP_ONLY=13b,14_ SNAP_CONFIGS=base,tv_big ui_snapshots out 2560x1440`.

## Журналы изменений

`documentation/CHANGES_*.md`, `documentation/FIXES_*.md`.
