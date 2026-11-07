# ==================== Основная конфигурация ====================
QT += core quick gui concurrent network multimedia
QT += gui-private   # QWindowSystemInterface (RemoteNavigator: щелчок по OK)
CONFIG += c++20

# ==================== Версия ====================
# Единственный источник версии — файл app_version.txt в корне репозитория.
# Не называть его VERSION: на Windows (регистр не важен) он перекрывает
# стандартный заголовок C++20 <version> — INCLUDEPATH содержит ".".
# Из него: макрос MEDIASTATION_VERSION (Version.h) и ресурс VERSIONINFO
# в MediaStation.exe (Свойства → Подробно). Сервер читает версию из exe.
# QMAKE_INTERNAL_INCLUDED_FILES: при изменении app_version.txt qmake перезапустится сам.
APP_VERSION = $$cat($$PWD/app_version.txt)
APP_VERSION = $$first(APP_VERSION)
isEmpty(APP_VERSION): APP_VERSION = 0.0.0
VERSION = $$APP_VERSION
QMAKE_INTERNAL_INCLUDED_FILES += $$PWD/app_version.txt
DEFINES += MEDIASTATION_VERSION=\\\"$$APP_VERSION\\\"
QMAKE_TARGET_PRODUCT     = MediaStation
QMAKE_TARGET_DESCRIPTION = MediaStation
QMAKE_TARGET_COPYRIGHT   = MediaStation

# Раскомментировать для тестирования уведомлений:
# DEFINES += DEBUG_NOTIFICATIONS

# Раскомментировать если подключён TagLib для чтения ID3-тегов:
# DEFINES += USE_TAGLIB
# INCLUDEPATH += tools/taglib/include
# LIBS += -L$$PWD/tools/taglib/lib -ltaglib

# ==================== Пути к заголовочным файлам ====================
INCLUDEPATH += \
    .                                                       \
    core                                                    \
    tools/llama/include                                     \
    tools/whisper/include                                   \
    tools/onnxruntime/include                               \
    modules/AppModule                                       \
    modules/TileModule                                      \
    modules/NetworkModule                                   \
    modules/JoystickModule                                  \
    modules/SettingsModule                                  \
    modules/NotificationModule                              \
    modules/AirMouseModule                                  \
    modules/GameLibraryModule                               \
    modules/LoggerModule                                    \
    modules/AndroidTVModule                                 \
    modules/OverlayModule                                   \
    modules/UpdateManager                                   \
    modules/GameInfoModule                                  \
    modules/GameMediaModule                                 \
    modules/KaraokeModule

# ==================== Исходные файлы ====================
SOURCES += \
    main.cpp                                                \
    core/IModule.cpp                                        \
    core/ModuleHost.cpp                                     \
    tools/AtomicFile.cpp                                    \
    tools/GameInfoStore.cpp                                 \
    tools/HttpClient.cpp                                    \
    tools/LlamaClient.cpp                                   \
    tools/SevenZip.cpp                                      \
    tools/WofCompact.cpp                                    \
    modules/AndroidTVModule/AndroidTVInput.cpp              \
    modules/AndroidTVModule/EmulatorGrpcClient.cpp          \
    modules/AndroidTVModule/AdGuardHome.cpp                 \
    modules/AndroidTVModule/AndroidTVLifecycle.cpp          \
    modules/AndroidTVModule/AndroidTVLog.cpp                \
    modules/AndroidTVModule/AndroidTVModule.cpp             \
    modules/AndroidTVModule/AndroidTVProfiles.cpp           \
    modules/AndroidTVModule/AndroidTVWindow.cpp             \
    modules/AirMouseModule/G72BatteryModule.cpp             \
    modules/AirMouseModule/G72KeysModule.cpp                \
    modules/AppModule/AppController.cpp                     \
    modules/AppModule/RemoteNavigator.cpp                   \
    modules/AppModule/GameWatcher.cpp                       \
    modules/GameInfoModule/GameInfoFetcher.cpp              \
    modules/GameInfoModule/GameInfoModule.cpp               \
    modules/GameInfoModule/HtmlStripper.cpp                 \
    modules/GameInfoModule/TitleNormalizer.cpp              \
    modules/GameLibraryModule/GameLibraryModule.cpp         \
    modules/GameMediaModule/CoverFetcher.cpp                \
    modules/GameMediaModule/GameMediaModule.cpp             \
    modules/GameMediaModule/SteamAppList.cpp                \
    modules/GameMediaModule/TrailerFetcher.cpp              \
    modules/JoystickModule/JoystickModule.cpp               \
    modules/LoggerModule/LoggerModule.cpp                   \
    modules/NetworkModule/NetworkLogSender.cpp              \
    modules/NetworkModule/NetworkModule.cpp                 \
    modules/NetworkModule/NetworkServer.cpp                 \
    modules/NotificationModule/NotificationModule.cpp       \
    modules/OverlayModule/OverlayModule.cpp                 \
    modules/SettingsModule/SettingsManager.cpp              \
    modules/TileModule/InstallManager.cpp                   \
    modules/TileModule/InstallUtils.cpp                     \
    modules/TileModule/TileManager.cpp                      \
    modules/UpdateManager/UpdateManager.cpp                 \
    modules/KaraokeModule/KaraokeModule.cpp                 \
    modules/KaraokeModule/AudioTags.cpp                     \
    modules/KaraokeModule/KaraokeWorker.cpp                 \
    modules/KaraokeModule/DemucsWrapper.cpp                 \
    modules/KaraokeModule/LrcFetcher.cpp                    \
    modules/KaraokeModule/LrcBuilder.cpp                    \
    modules/KaraokeModule/LyricsAligner.cpp                 \
    modules/KaraokeModule/SongNameResolver.cpp              \
    modules/KaraokeModule/WhisperWrapper.cpp                \
    modules/KaraokeModule/kiss/kiss_fft.c                        \
    modules/KaraokeModule/kiss/kiss_fftr.c

# ==================== Заголовочные файлы ====================
HEADERS += \
    Version.h                                               \
    core/EventBus.h                                         \
    core/IModule.h                                          \
    core/ModuleHost.h                                       \
    tools/AtomicFile.h                                      \
    tools/LocalModelPaths.h                                 \
    tools/GameInfoStore.h                                   \
    tools/HttpClient.h                                      \
    tools/LlamaClient.h                                     \
    tools/SevenZip.h                                        \
    tools/WofCompact.h                                      \
    modules/AndroidTVModule/AndroidTVModule.h               \
    modules/AndroidTVModule/EmulatorGrpcClient.h            \
    modules/AndroidTVModule/AdGuardHome.h                   \
    modules/AirMouseModule/G72BatteryModule.h               \
    modules/AirMouseModule/G72KeysModule.h                  \
    modules/AppModule/AppController.h                       \
    modules/AppModule/RemoteNavigator.h                     \
    modules/AppModule/GameWatcher.h                         \
    modules/GameInfoModule/GameInfoFetcher.h                \
    modules/GameInfoModule/GameInfoModule.h                 \
    modules/GameInfoModule/HtmlStripper.h                   \
    modules/GameInfoModule/TitleNormalizer.h                \
    modules/GameLibraryModule/GameLibraryModule.h           \
    modules/GameMediaModule/CoverFetcher.h                  \
    modules/GameMediaModule/GameMediaModule.h               \
    modules/GameMediaModule/SteamAppList.h                  \
    modules/GameMediaModule/TrailerFetcher.h                \
    modules/JoystickModule/JoystickModule.h                 \
    modules/LoggerModule/LoggerModule.h                     \
    modules/NetworkModule/NetworkModule.h                   \
    modules/NotificationModule/NotificationModule.h         \
    modules/OverlayModule/OverlayModule.h                   \
    modules/SettingsModule/SettingsManager.h                \
    modules/TileModule/InstallManager.h                     \
    modules/TileModule/InstallUtils.h                       \
    modules/TileModule/TileManager.h                        \
    modules/UpdateManager/UpdateManager.h                   \
    modules/KaraokeModule/KaraokeModule.h                   \
    modules/KaraokeModule/AudioTags.h                       \
    modules/KaraokeModule/KaraokeWorker.h                   \
    modules/KaraokeModule/DemucsWrapper.h                   \
    modules/KaraokeModule/DemucsDsp.h                       \
    modules/KaraokeModule/VoiceActivity.h                   \
    modules/KaraokeModule/SongMeta.h                        \
    modules/KaraokeModule/LrcFetcher.h                      \
    modules/KaraokeModule/LrcBuilder.h                      \
    modules/KaraokeModule/LyricsAligner.h                   \
    modules/KaraokeModule/SongNameResolver.h                \
    modules/KaraokeModule/WhisperWrapper.h                  \
    modules/KaraokeModule/kiss/kiss_fft.h                        \
    modules/KaraokeModule/kiss/kiss_fftr.h                       \
    modules/KaraokeModule/kiss/_kiss_fft_guts.h                  \
    modules/KaraokeModule/kiss/kiss_fft_log.h

# ==================== Ресурсы ====================
RESOURCES += \
    assets.qrc \
    qml.qrc

# ==================== Библиотеки ====================
# llama.cpp
LIBS += -L$$PWD/tools/llama/lib -lllama

# ONNX Runtime (для DemucsWrapper)
LIBS += -L$$PWD/tools/onnxruntime/lib -lonnxruntime

# whisper.cpp (DLL загружается динамически через QLibrary)
LIBS += -L$$PWD/tools/whisper/lib -lwhisper

# Платформозависимые библиотеки Windows
win32: CONFIG += windows
win32: LIBS += -luser32
win32: LIBS += -lWindowsApp
win32: LIBS += -lwinmm
win32: LIBS += -ladvapi32 -lpowrprof   # выключение/сон (AppController::powerAction)
win32: LIBS += -ldwmapi   # DwmFlush (AndroidTVModule); #pragma comment работает только в MSVC
win32: LIBS += -lgdi32    # снимок окна эмулятора перед показом ТВ (AndroidTVModule::sampleEmulatorContent)
win32: LIBS += -lshell32 -lole32   # ShellExecuteEx: запуск игр без PowerShell (GameWatcher), служба AdGuard Home

# ==================== Прочее ====================
DISTFILES +=
