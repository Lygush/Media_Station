// modules/KaraokeModule/AudioTags.h
// ─────────────────────────────────────────────────────────────────────────────
// Чтение тегов «исполнитель / название / альбом» без сторонних библиотек
// (аудит §9.2 п.2: TagLib не подключён, источник "tags" был недостижим, и для
// каждого файла грузилась нейросеть, даже если теги в порядке).
//
//   mp3  — ID3v2.2/2.3/2.4 в начале файла, иначе ID3v1 в конце;
//   flac — блок VORBIS_COMMENT (+ длительность из STREAMINFO).
//
// Однобайтовые строки (ID3v1 и кодировка 0 в ID3v2) в русских файлах почти
// всегда на самом деле cp1251 — распознаётся по содержимому.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <QByteArray>
#include <QString>

namespace AudioTags {

struct Tags {
    QString artist;
    QString title;
    QString album;
    int     durationSec = 0;   // только FLAC
    bool isEmpty() const { return artist.isEmpty() && title.isEmpty(); }
};

/// Прочитать теги файла (по расширению и сигнатуре). Ошибки — пустой результат.
Tags read(const QString &filePath);

// Открыто для тестов
Tags parseId3v2(const QByteArray &data);     // data — начало файла
Tags parseId3v1(const QByteArray &last128);  // последние 128 байт
Tags parseFlac(const QByteArray &data);      // начало файла (метаданные)
QString decodeSingleByte(const QByteArray &bytes);   // Latin-1 или cp1251

} // namespace AudioTags
