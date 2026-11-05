QT += core testlib
CONFIG += c++20 console testcase
CONFIG -= app_bundle

TARGET = tst_tilemanager_structure
TEMPLATE = app
include($$PWD/../../common.pri)

INCLUDEPATH += $$PWD/../../..
DEPENDPATH  += $$PWD/../../..

SOURCES += \
    tst_tilemanager_structure.cpp \
    ../../../modules/TileModule/TileManager.cpp \
    ../../../modules/TileModule/InstallManager.cpp \
    ../../../modules/TileModule/InstallUtils.cpp

HEADERS += \
    ../../../modules/TileModule/TileManager.h \
    ../../../modules/TileModule/InstallManager.h