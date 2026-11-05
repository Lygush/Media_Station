QT += core testlib
CONFIG += c++20 console testcase
CONFIG -= app_bundle

TARGET = tst_g72battery_mock
TEMPLATE = app
include($$PWD/../../common.pri)

DEFINES += TEST_MODE  # отключаем реальный WinRT

INCLUDEPATH += $$PWD/../../..
DEPENDPATH  += $$PWD/../../..

SOURCES += \
    tst_g72battery_mock.cpp \
    ../../../modules/AirMouseModule/G72BatteryModule.cpp

HEADERS += \
    ../../../modules/AirMouseModule/G72BatteryModule.h