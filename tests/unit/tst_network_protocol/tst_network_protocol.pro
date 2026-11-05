QT += network
TEMPLATE = app
include($$PWD/../../common.pri)

SOURCES += \
    tst_network_protocol.cpp \
    $$PROJECT_ROOT/modules/NetworkModule/NetworkModule.cpp \
    $$PROJECT_ROOT/modules/NetworkModule/NetworkServer.cpp \
    $$PROJECT_ROOT/modules/NetworkModule/NetworkLogSender.cpp \
    $$PROJECT_ROOT/modules/TileModule/InstallUtils.cpp

HEADERS += \
    $$PROJECT_ROOT/modules/NetworkModule/NetworkModule.h
