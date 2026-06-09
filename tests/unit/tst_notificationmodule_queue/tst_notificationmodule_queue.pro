QT += core testlib
CONFIG += c++20 console testcase
CONFIG -= app_bundle

TARGET = tst_notificationmodule_queue
TEMPLATE = app

INCLUDEPATH += $$PWD/../../..
DEPENDPATH  += $$PWD/../../..

SOURCES += \
    tst_notificationmodule_queue.cpp \
    ../../../modules/NotificationModule/NotificationModule.cpp

HEADERS += \
    ../../../modules/NotificationModule/NotificationModule.h \
    ../../../core/IModule.h \
    ../../../core/EventBus.h