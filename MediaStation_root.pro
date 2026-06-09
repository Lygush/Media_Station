TEMPLATE = subdirs
CONFIG  += ordered   # собирать строго по порядку

SUBDIRS += \
    MediaStation \
    Updater

# Указываем Qt Creator где лежат .pro файлы для каждого subdir
MediaStation.file = MediaStation.pro
Updater.file      = updater/Updater.pro
