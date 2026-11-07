QT += core network testlib gui qml
CONFIG += c++20 console testcase
TEMPLATE = app
include($$PWD/../../common.pri)

PROJECT_ROOT = $$PWD/../../..
INCLUDEPATH += $$PROJECT_ROOT $$PROJECT_ROOT/core $$PROJECT_ROOT/tools \
               $$PROJECT_ROOT/modules/AndroidTVModule $$PROJECT_ROOT/modules/AirMouseModule \
               $$PROJECT_ROOT/modules/OverlayModule $$PROJECT_ROOT/modules/LoggerModule \
               $$PROJECT_ROOT/modules/SettingsModule $$PROJECT_ROOT/modules/NotificationModule

SOURCES += tst_androidtv_console.cpp \
    $$files($$PROJECT_ROOT/modules/AndroidTVModule/*.cpp) \
    $$PROJECT_ROOT/modules/SettingsModule/SettingsManager.cpp
HEADERS += $$files($$PROJECT_ROOT/modules/AndroidTVModule/*.h) \
    $$PROJECT_ROOT/core/EventBus.h \
    $$PROJECT_ROOT/modules/SettingsModule/SettingsManager.h

DEFINES += GRPC_SERVER_DIR=\\\"$$PWD/../tst_emulator_grpc\\\"
