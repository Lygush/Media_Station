// modules/KaraokeModule/SongMeta.h
// ─────────────────────────────────────────────────────────────────────────────
// SongMeta — метаданные трека, передаются через весь пайплайн.
//
// Заполняется в KaraokeModule::readMeta() из ID3-тегов (или имени файла),
// затем передаётся в KaraokeWorker и далее в LrcFetcher.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>

struct SongMeta {
    QString filePath;       ///< абсолютный путь к исходному аудиофайлу
    QString artist;         ///< исполнитель (из ID3 или из имени файла)
    QString title;          ///< название трека
    QString album;          ///< альбом (опционально, улучшает точность lrclib)
    int     durationSec = 0; ///< длительность в секундах (0 если неизвестна)

    /// true если есть минимум для запроса к lrclib.net
    bool hasEnoughMeta() const {
        return !artist.trimmed().isEmpty() && !title.trimmed().isEmpty();
    }
};
