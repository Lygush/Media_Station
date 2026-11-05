# tests/common.pri — общие настройки для всех тестов.
# Подключается в каждом тестовом .pro:  include($$PWD/../../common.pri)
# Пути совпадают с MediaStation.pro, чтобы исходники модулей собирались
# в тестах так же, как в приложении.

PROJECT_ROOT = $$clean_path($$PWD/..)

CONFIG += c++20 console testcase
CONFIG -= app_bundle
QT     += core testlib

INCLUDEPATH += \
    $$PROJECT_ROOT \
    $$PROJECT_ROOT/core \
    $$PROJECT_ROOT/tools \
    $$PROJECT_ROOT/modules/AppModule \
    $$PROJECT_ROOT/modules/TileModule \
    $$PROJECT_ROOT/modules/NetworkModule \
    $$PROJECT_ROOT/modules/JoystickModule \
    $$PROJECT_ROOT/modules/SettingsModule \
    $$PROJECT_ROOT/modules/NotificationModule \
    $$PROJECT_ROOT/modules/AirMouseModule \
    $$PROJECT_ROOT/modules/GameLibraryModule \
    $$PROJECT_ROOT/modules/LoggerModule \
    $$PROJECT_ROOT/modules/AndroidTVModule \
    $$PROJECT_ROOT/modules/OverlayModule \
    $$PROJECT_ROOT/modules/UpdateManager \
    $$PROJECT_ROOT/modules/GameInfoModule \
    $$PROJECT_ROOT/modules/GameMediaModule \
    $$PROJECT_ROOT/modules/KaraokeModule

DEPENDPATH += $$INCLUDEPATH

DEFINES += MEDIASTATION_TESTS

# Ядро и утилиты, от которых зависят почти все модули
QT += qml concurrent
SOURCES += \
    $$PROJECT_ROOT/core/IModule.cpp \
    $$PROJECT_ROOT/tools/AtomicFile.cpp \
    $$PROJECT_ROOT/tools/SevenZip.cpp
HEADERS += \
    $$PROJECT_ROOT/core/EventBus.h
