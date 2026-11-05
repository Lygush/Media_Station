QT += core testlib concurrent
CONFIG += c++20 console testcase
CONFIG -= app_bundle

TARGET = tst_installmanager_local
TEMPLATE = app
include($$PWD/../../common.pri)

INCLUDEPATH += $$PWD/../../..
DEPENDPATH  += $$PWD/../../..

SOURCES += \
    tst_installmanager_local.cpp \
    ../../../modules/TileModule/InstallManager.cpp \
    ../../../modules/TileModule/InstallUtils.cpp

HEADERS += \
    ../../../modules/TileModule/InstallManager.h \
    ../../../modules/TileModule/InstallUtils.h