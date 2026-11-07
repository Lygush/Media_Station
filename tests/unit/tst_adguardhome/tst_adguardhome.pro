include(../../common.pri)
QT += network
TARGET = tst_adguardhome
INCLUDEPATH += $$PROJECT_ROOT/modules/AndroidTVModule
SOURCES += tst_adguardhome.cpp \
           $$PROJECT_ROOT/modules/AndroidTVModule/AdGuardHome.cpp
HEADERS += $$PROJECT_ROOT/modules/AndroidTVModule/AdGuardHome.h
