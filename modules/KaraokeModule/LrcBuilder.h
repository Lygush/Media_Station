// modules/KaraokeModule/LrcBuilder.h
// ─────────────────────────────────────────────────────────────────────────────
// LrcBuilder — конвертация QVector<WhisperWord> → Enhanced LRC строка.
//
// Используется только как fallback когда lrclib.net не вернул syncedLyrics.
// Если lrclib вернул готовый LRC — этот класс не задействуется.
//
// Формат Enhanced LRC:
//   [mm:ss.xx]<mm:ss.xx> слово <mm:ss.xx> слово\n
//
// Группировка в строки: пауза между словами > pauseThresholdMs (по умолчанию 1500 мс).
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>
#include "WhisperWrapper.h"

class LrcBuilder
{
public:
    /// Порог паузы между словами для разбивки на строки (мс).
    static constexpr qint64 kDefaultPauseThresholdMs = 1500;

    /// Конвертирует список слов с тайм-кодами в Enhanced LRC.
    /// @param words             Результат WhisperWrapper::transcribe()
    /// @param pauseThresholdMs  Пауза для разбивки на строки
    static QString build(const QVector<WhisperWord> &words,
                         qint64 pauseThresholdMs = kDefaultPauseThresholdMs);

private:
    /// Форматирует миллисекунды в [mm:ss.xx]
    static QString formatTimestamp(qint64 ms);
};
