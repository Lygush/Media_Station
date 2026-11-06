# ui_snapshots — снимки экранов интерфейса с заглушками C++-модулей.
# Не автотест: запускается вручную, результат — PNG для просмотра.
#   ui_snapshots <папка> [1280x720 1920x1080 ...]
QT += core gui gui-private qml quick
CONFIG += c++20 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = ui_snapshots

DEFINES += SNAPSHOT_SRC_DIR=\\\"$$PWD\\\"

INCLUDEPATH += $$PWD/../../modules/AppModule
SOURCES += main.cpp $$PWD/../../modules/AppModule/RemoteNavigator.cpp
HEADERS += $$PWD/../../modules/AppModule/RemoteNavigator.h
RESOURCES += $$PWD/../../qml.qrc $$PWD/../../assets.qrc
