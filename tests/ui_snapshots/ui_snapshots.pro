# ui_snapshots — снимки экранов интерфейса с заглушками C++-модулей.
# Не автотест: запускается вручную, результат — PNG для просмотра.
#   ui_snapshots <папка> [1280x720 1920x1080 ...]
QT += core gui qml quick
CONFIG += c++20 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = ui_snapshots

DEFINES += SNAPSHOT_SRC_DIR=\\\"$$PWD\\\"

SOURCES += main.cpp
RESOURCES += $$PWD/../../qml.qrc $$PWD/../../assets.qrc
