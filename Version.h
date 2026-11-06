// Version.h
// ─────────────────────────────────────────────────────────────────────────────
// Версия MediaStation. Источник — файл app_version.txt в корне репозитория:
// MediaStation.pro читает его и передаёт сюда через DEFINES, а также
// записывает в ресурс VERSIONINFO exe-файла. Руками здесь ничего не менять —
// меняется только app_version.txt.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#ifndef MEDIASTATION_VERSION
#  define MEDIASTATION_VERSION "0.0.0"   // сборка не через MediaStation.pro (тесты)
#endif
