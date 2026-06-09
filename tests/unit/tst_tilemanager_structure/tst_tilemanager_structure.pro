QT += core testlib
CONFIG += c++20 console testcase
CONFIG -= app_bundle

TARGET = tst_tilemanager_structure
TEMPLATE = app

INCLUDEPATH += $$PWD/../../..
DEPENDPATH  += $$PWD/../../..

SOURCES += \
    tst_tilemanager_structure.cpp \
    ../../../modules/TileModule/TileManager.cpp

HEADERS += \
    ../../../modules/TileModule/TileManager.h \
    ../../../core/IModule.h