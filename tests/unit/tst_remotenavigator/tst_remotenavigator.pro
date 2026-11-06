TARGET = tst_remotenavigator
TEMPLATE = app
include($$PWD/../../common.pri)
QT += gui gui-private qml quick

INCLUDEPATH += $$PROJECT_ROOT/modules/AppModule
SOURCES += \
    tst_remotenavigator.cpp \
    $$PROJECT_ROOT/modules/AppModule/RemoteNavigator.cpp
HEADERS += $$PROJECT_ROOT/modules/AppModule/RemoteNavigator.h
