// modules/KaraokeModule/LrcFetcher.h
// ─────────────────────────────────────────────────────────────────────────────
// LrcFetcher — получение синхронизированного LRC-текста с lrclib.net.
//
// Использование (из фонового потока):
//
//   LrcFetcher fetcher;
//   LrcResult r = fetcher.fetch("Кино", "Группа крови", "Группа крови", 220);
//   if (r.hasSynced) {
//       // r.lrc — готовый Enhanced LRC
//   } else if (r.hasPlain) {
//       // r.plainLyrics — текст без тайм-кодов (fallback на Whisper)
//   }
//
// ВАЖНО: вызывать ТОЛЬКО из фонового потока (использует HttpClient::get).
//
// API: https://lrclib.net/api/get
//   Параметры: artist_name, track_name, album_name (optional), duration (optional, сек)
//   Ответ JSON: { syncedLyrics: "...", plainLyrics: "..." }
//   syncedLyrics — Enhanced LRC с тайм-кодами на слова, либо null.
//   plainLyrics  — просто текст, либо null.
//
// Кэш: не хранится здесь — кэширование делает KaraokeModule через AtomicFile.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>
#include <atomic>

struct LrcResult {
    bool    hasSynced   = false;  ///< syncedLyrics получен и не пуст
    bool    hasPlain    = false;  ///< plainLyrics получен и не пуст
    bool    networkError = false; ///< таймаут или нет сети
    QString lrc;                  ///< синхронизированный LRC (если hasSynced)
    QString plainLyrics;          ///< текст без меток (если hasPlain)
};

class LrcFetcher
{
public:
    LrcFetcher() = default;

    /// Запрашивает LRC у lrclib.net.
    ///
    /// @param artist    Исполнитель (из ID3-тегов)
    /// @param title     Название трека (из ID3-тегов)
    /// @param album     Альбом (опционально, улучшает точность)
    /// @param durationSec Длительность в секундах (опционально, улучшает точность)
    /// @param cancelled   Флаг отмены (опционально)
    LrcResult fetch(const QString          &artist,
                    const QString          &title,
                    const QString          &album      = {},
                    int                     durationSec = 0,
                    const std::atomic<bool> *cancelled  = nullptr) const;

    /// Строит URL для запроса (удобно для логов и тестов).
    static QString buildUrl(const QString &artist,
                            const QString &title,
                            const QString &album,
                            int            durationSec);
};
