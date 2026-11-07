QT += core testlib
include($$PWD/../../common.pri)
SOURCES += tst_audiotags.cpp $$PROJECT_ROOT/modules/KaraokeModule/AudioTags.cpp
HEADERS += $$PROJECT_ROOT/modules/KaraokeModule/AudioTags.h
INCLUDEPATH += $$PROJECT_ROOT/modules/KaraokeModule
DEFINES += DATA_DIR=\\\"$$PWD/data\\\"
