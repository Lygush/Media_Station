// tools/AtomicFile.h
// ─────────────────────────────────────────────────────────────────────────────
// AtomicFile — атомарная запись файла через паттерн .tmp → rename.
//
// Гарантирует что файл либо записан полностью, либо не тронут вовсе.
// При сбое питания или краше в момент записи оригинальный файл не повреждается.
//
// Использование:
//
//   // JSON:
//   AtomicFile::writeJson("/path/to/file.json", jsonDocument);
//
//   // Произвольные байты:
//   AtomicFile::writeBytes("/path/to/file.jpg", byteArray);
//
// Возвращает true при успехе, false при ошибке (с qWarning в лог).
//
// Не используется для:
//   - Потоковой записи (TrailerFetcher пишет чанками — там своя логика)
//   - Файлов конфигурации AVD (AndroidTVLifecycle — там нет атомарности)
//   - Лог-файлов (открытых на append)
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>
#include <QByteArray>

class QJsonDocument;

namespace AtomicFile {

/// Записывает JSON-документ в файл атомарно.
/// Использует QJsonDocument::Indented для читаемого форматирования.
bool writeJson(const QString &path, const QJsonDocument &doc);

/// Записывает произвольный массив байт в файл атомарно.
bool writeBytes(const QString &path, const QByteArray &data);

} // namespace AtomicFile
