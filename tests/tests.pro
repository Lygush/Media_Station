# Все автотесты MediaStation. Общие пути и исходники ядра — tests/common.pri.
TEMPLATE = subdirs

SUBDIRS = \
    unit/tst_settingsmanager \
    unit/tst_installutils \
    unit/tst_notificationmodule_queue \
    unit/tst_tilemanager_structure \
    unit/tst_gamelibrarymodule_scan \
    unit/tst_joystickmodule_mock \
    unit/tst_network_protocol \
    integration/tst_installmanager_local

# G72BatteryModule использует WinRT — собирается только MSVC
win32-msvc*: SUBDIRS += unit/tst_g72battery_mock
