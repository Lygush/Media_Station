TEMPLATE = app
include($$PWD/../../common.pri)

TARGET = tst_installutils

SOURCES += \
    tst_installutils.cpp \
    $$PROJECT_ROOT/modules/TileModule/InstallUtils.cpp
