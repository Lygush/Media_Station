QT += core testlib
CONFIG += c++20 console testcase
CONFIG -= app_bundle

TARGET = tst_joystickmodule_mock
TEMPLATE = app

INCLUDEPATH += $$PWD/../../..
DEPENDPATH  += $$PWD/../../..

SOURCES += \
    tst_joystickmodule_mock.cpp \
    ../../../modules/JoystickModule/JoystickModule.cpp

HEADERS += \
    ../../../modules/JoystickModule/JoystickModule.h \
    ../../../core/IModule.h \
    ../../../core/EventBus.h