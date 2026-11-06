// tools/SevenZip.h
// ─────────────────────────────────────────────────────────────────────────────
// SevenZip — обёртка над 7zr.exe / 7z.exe для архивации и разархивации.
//
// Все методы блокирующие — вызывать только из фонового потока.
//
// Использование:
//
//   // Архивация:
//   SevenZip::Result r = SevenZip::compress(
//       srcDir,       // рабочая директория
//       "game",       // что архивировать (относительный путь)
//       archivePath,  // куда писать .7z
//       cancelFlag,   // опциональный флаг отмены
//       [](int pct, const QString &file) { qDebug() << pct << file; }
//   );
//
//   // Разархивация:
//   SevenZip::Result r = SevenZip::extract(
//       archivePath,  // что распаковывать
//       destDir,      // куда
//       cancelFlag,
//       [](int pct, const QString &) { emit progress(pct); }
//   );
//
//   if (r == SevenZip::Ok) { ... }
//
// Прогресс-callback вызывается из того же потока что и compress/extract.
// Для передачи в GUI используйте QMetaObject::invokeMethod(..., Qt::QueuedConnection).
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>
#include <QStringList>
#include <functional>
#include <atomic>

namespace SevenZip {

enum Result {
    Ok        = 0,  // успешно
    Cancelled = 1,  // отменено через cancelFlag
    NotFound  = 2,  // 7zr.exe не найден
    StartFail = 3,  // процесс не запустился
    ExitError = 4,  // процесс завершился с ненулевым кодом
};

// Callback прогресса: percent (0–100), currentFile (может быть пустым)
using ProgressFn = std::function<void(int percent, const QString &currentFile)>;

/// Находит путь к 7zr.exe или 7z.exe.
/// Ищет в: <appDir>/tools/7zr.exe → <appDir>/tools/7z.exe → fallback "7zr" (PATH).
/// Возвращает пустую строку если не найден (только fallback).
QString findBinary();

/// Возвращает true если бинарник найден на диске.
bool isAvailable();

/// Архивирует содержимое в формате 7z с максимальным сжатием LZMA2.
///
/// @param workDir      Рабочая директория процесса
/// @param source       Что архивировать (относительный путь от workDir, напр. "game")
/// @param archivePath  Полный путь к создаваемому .7z файлу
/// @param cancelFlag   Опциональный флаг отмены (проверяется каждые 200 мс)
/// @param onProgress   Опциональный callback прогресса
Result compress(const QString          &workDir,
                const QString          &source,
                const QString          &archivePath,
                const std::atomic<bool> *cancelFlag  = nullptr,
                ProgressFn              onProgress   = nullptr,
                int                     level        = 3);   // -mx: 0 (без сжатия) … 9

/// Разархивирует .7z файл в указанную директорию.
///
/// @param archivePath  Путь к .7z файлу
/// @param destDir      Куда распаковывать (-o параметр 7zr)
/// @param cancelFlag   Опциональный флаг отмены
/// @param onProgress   Опциональный callback прогресса
Result extract(const QString          &archivePath,
               const QString          &destDir,
               const std::atomic<bool> *cancelFlag  = nullptr,
               ProgressFn              onProgress   = nullptr);

/// Текстовое описание результата для логов.
const char *resultToString(Result r);

} // namespace SevenZip
