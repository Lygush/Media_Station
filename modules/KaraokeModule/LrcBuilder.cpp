// modules/KaraokeModule/LrcBuilder.cpp
#include "LrcBuilder.h"

#include <QStringList>

// ─────────────────────────────────────────────────────────────────────────────

QString LrcBuilder::formatTimestamp(qint64 ms)
{
    const qint64 totalSec = ms / 1000;
    const qint64 min      = totalSec / 60;
    const qint64 sec      = totalSec % 60;
    const qint64 cs       = (ms % 1000) / 10; // сотые секунды
    return QString("[%1:%2.%3]")
        .arg(min, 2, 10, QChar('0'))
        .arg(sec, 2, 10, QChar('0'))
        .arg(cs,  2, 10, QChar('0'));
}

QString LrcBuilder::build(const QVector<WhisperWord> &words, qint64 pauseThresholdMs)
{
    if (words.isEmpty())
        return {};

    QStringList lines;
    QString     lineBody;       // inline-часть текущей строки: <mm:ss.xx> слово ...
    qint64      lineStartMs = -1;
    qint64      prevEndMs   = -1;

    auto flushLine = [&]() {
        if (lineStartMs < 0 || lineBody.isEmpty()) return;
        // Строка LRC: [mm:ss.xx]<inline тайм-коды>
        lines << formatTimestamp(lineStartMs) + lineBody.trimmed();
        lineBody.clear();
        lineStartMs = -1;
    };

    for (const WhisperWord &w : words) {
        // Пауза перед словом — начинаем новую строку
        if (prevEndMs >= 0 && (w.t0_ms - prevEndMs) > pauseThresholdMs)
            flushLine();

        // Формат inline-тайм-кода слова: <mm:ss.xx>
        const qint64 min = w.t0_ms / 60000;
        const qint64 sec = (w.t0_ms % 60000) / 1000;
        const qint64 cs  = (w.t0_ms % 1000) / 10;
        const QString inlineTs = QString("<%1:%2.%3>")
            .arg(min, 2, 10, QChar('0'))
            .arg(sec, 2, 10, QChar('0'))
            .arg(cs,  2, 10, QChar('0'));

        if (lineStartMs < 0)
            lineStartMs = w.t0_ms;

        lineBody += inlineTs + w.text + ' ';
        prevEndMs = w.t1_ms;
    }

    flushLine();

    return lines.join('\n') + '\n';
}
