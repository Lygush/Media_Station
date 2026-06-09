QT += core testlib concurrent
CONFIG += c++20 console testcase
CONFIG -= app_bundle

TARGET = tst_gamelibrarymodule_scan
TEMPLATE = app

INCLUDEPATH += $$PWD/../../..
DEPENDPATH  += $$PWD/../../..

SOURCES += \
    tst_gamelibrarymodule_scan.cpp \
    ../../../modules/GameLibraryModule/GameLibraryModule.cpp \
    ../../../modules/TileModule/InstallUtils.cpp \
    ../../../modules/SettingsModule/SettingsManager.cpp

HEADERS += \
    ../../../modules/GameLibraryModule/GameLibraryModule.h \
    ../../../modules/TileModule/InstallUtils.h \
    ../../../modules/SettingsModule/SettingsManager.h \
    ../../../core/IModule.h \
    ../../../core/EventBus.h