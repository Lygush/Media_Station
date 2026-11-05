QT += core testlib
CONFIG += c++20 console testcase
TEMPLATE = app
include($$PWD/../../common.pri)

PROJECT_ROOT = $$PWD/../../..
INCLUDEPATH += $$PROJECT_ROOT \
               $$PROJECT_ROOT/modules/SettingsModule

SOURCES += \
    tst_settingsmanager.cpp \
    $$PROJECT_ROOT/modules/SettingsModule/SettingsManager.cpp

HEADERS += \
    $$PROJECT_ROOT/modules/SettingsModule/SettingsManager.h
