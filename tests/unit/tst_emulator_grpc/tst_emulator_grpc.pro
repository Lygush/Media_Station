QT += core network testlib
include($$PWD/../../common.pri)
SOURCES += tst_emulator_grpc.cpp \
    $$PROJECT_ROOT/modules/AndroidTVModule/EmulatorGrpcClient.cpp
HEADERS += $$PROJECT_ROOT/modules/AndroidTVModule/EmulatorGrpcClient.h
DEFINES += FAKE_SERVER_DIR=\\\"$$PWD\\\"
