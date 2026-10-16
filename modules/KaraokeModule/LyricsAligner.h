// modules/KaraokeModule/LyricsAligner.h
// ─────────────────────────────────────────────────────────────────────────────
// LyricsAligner — расставляет тайм-коды на ИЗВЕСТНЫЙ текст песни.
//
// Ситуация: lrclib вернул plainLyrics (текст без времени). Whisper не умеет
// «принять текст и трек и вернуть метки» — он всегда декодирует заново. Поэтому:
//
//   1. Whisper распознаёт дорожку (с подсказкой — словарём слов песни);
//   2. распознанные слова сопоставляются с эталонным текстом
//      (выравнивание последовательностей по словам, устойчиво к опечаткам,
//       пропущенным и лишним словам);
//   3. итог — ПРАВИЛЬНЫЙ текст и его деление на строки из lrclib,
//      а время слов берётся у Whisper. Не расслышанные слова получают время
//      интерполяцией между ближайшими опорными.
//
// Если совпала лишь малая доля слов (текст не от той песни / Whisper не
// справился) — matchedRatio мал, и вызывающий код использует свободное
// распознавание.
//
// Зависит только от QtCore.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include "WhisperWrapper.h"

class LyricsAligner
{
public:
    struct Result {
        QVector<QVector<WhisperWord>> lines;   ///< строки эталонного текста со временем слов
        int    refWords     = 0;
        int    matchedWords = 0;
        double matchedRatio = 0.0;             ///< matchedWords / refWords
    };

    /// refText — plainLyrics; recognized — слова Whisper (с абсолютным временем).
    static Result align(const QString &refText, const QVector<WhisperWord> &recognized);

    /// Словарь для Whisper initial_prompt: уникальные слова текста (≥ minLen букв)
    /// в порядке появления, не длиннее maxChars символов.
    static QString promptVocabulary(const QString &refText, int minLen = 4, int maxChars = 450);

    /// Нормализация слова для сравнения: нижний регистр, ё→е, без пунктуации.
    static QString normalize(const QString &word);

    /// Схожесть слов 0..1 (1 − Левенштейн / длина).
    static double similarity(const QString &a, const QString &b);
};
