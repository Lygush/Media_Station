// Version.h
// ─────────────────────────────────────────────────────────────────────────────
// Версия MediaStation. Этот файл обновляется при каждом релизном билде.
//
// В реальном CI-пайплайне генерируется скриптом из git tag:
//   version=$(git describe --tags --abbrev=0)
//   sed "s/0.0.0/$version/g" Version.h.in > Version.h
//
// Пример Version.h.in (хранится в репозитории):
//   #define MEDIASTATION_VERSION "0.0.0"
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#define MEDIASTATION_VERSION "1.0.0"
#define MEDIASTATION_VERSION_MAJOR 1
#define MEDIASTATION_VERSION_MINOR 0
#define MEDIASTATION_VERSION_PATCH 0
