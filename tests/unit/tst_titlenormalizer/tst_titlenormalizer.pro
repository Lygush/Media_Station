TARGET = tst_titlenormalizer
TEMPLATE = app
include($$PWD/../../common.pri)

SOURCES += \
    tst_titlenormalizer.cpp \
    $$PROJECT_ROOT/modules/GameInfoModule/TitleNormalizer.cpp
