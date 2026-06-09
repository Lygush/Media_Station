QT       += core gui widgets
TARGET    = Updater
TEMPLATE  = app
CONFIG   += c++17 windows

# OUT_PWD для этого subproject:
#   C:\...\build\Desktop_MSVC-Release\Updater\
#
# MediaStation.exe лежит в:
#   C:\...\build\Desktop_MSVC-Release\release\
#
# Поднимаемся на уровень вверх (из Updater\ → Desktop_MSVC-Release\)
# и спускаемся в release\Updater\ или debug\Updater\

CONFIG(release, debug|release): DESTDIR = $$OUT_PWD/../release/Updater
else:                            DESTDIR = $$OUT_PWD/../debug/Updater

win32: LIBS += -lshell32 -ladvapi32

SOURCES += \
    Updater_main.cpp  \
    UpdaterWindow.cpp

HEADERS += \
    UpdaterWindow.h
