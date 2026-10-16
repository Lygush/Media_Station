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

// ─────────────────────────────────────────────────────────────────────────────
// Разбивка слов Whisper на строки песни.
//
// Whisper не отдаёт переносы строк, но в песнях без пауз целый куплет иначе
// склеился бы в одну строку. Поэтому новая строка начинается, когда:
//   • пауза перед словом > pauseThresholdMs;
//   • предыдущее слово закончило предложение (. ! ? …), а в строке уже ≥ 2 слов;
//   • слово начинается с заглавной буквы и в строке уже ≥ 3 слов
//     (Whisper пишет начало каждой стихотворной строки с заглавной;
//      английское "I" / "I'm" не считается);
//   • строка стала длиннее kMaxLineChars символов.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int kMaxLineChars   = 46;
constexpr int kMinWordsForCap = 3;

bool startsNewPhrase(const QString &word)
{
    QString w = word;
    while (!w.isEmpty() && !w.at(0).isLetter()) w.remove(0, 1);   // «Ты, (Ты
    if (w.isEmpty() || !w.at(0).isUpper()) return false;

    // Английское местоимение I (I, I'm, I'll, I've, I'd) — не начало строки
    if (w.at(0) == QLatin1Char('I') && (w.size() == 1 || w.at(1) == QLatin1Char('\'')
                                        || w.at(1) == QChar(0x2019)))
        return false;
    return true;
}

bool endsSentence(const QString &word)
{
    QString w = word.trimmed();
    while (!w.isEmpty() && (w.endsWith(QChar(0x00BB)) || w.endsWith(QLatin1Char('"'))
                            || w.endsWith(QLatin1Char(')'))))
        w.chop(1);
    return w.endsWith(QLatin1Char('.')) || w.endsWith(QLatin1Char('!'))
        || w.endsWith(QLatin1Char('?')) || w.endsWith(QChar(0x2026));
}

} // namespace

QString LrcBuilder::build(const QVector<WhisperWord> &words, qint64 pauseThresholdMs)
{
    if (words.isEmpty())
        return {};

    QStringList lines;
    QString     lineBody;       // inline-часть текущей строки: <mm:ss.xx> слово ...
    qint64      lineStartMs = -1;
    qint64      prevEndMs   = -1;
    int         lineWords   = 0;
    int         lineChars   = 0;
    QString     prevText;

    auto flushLine = [&]() {
        if (lineStartMs >= 0 && !lineBody.isEmpty())
            lines << formatTimestamp(lineStartMs) + lineBody.trimmed();   // [mm:ss.xx]<inline…>
        lineBody.clear();
        lineStartMs = -1;
        lineWords   = 0;
        lineChars   = 0;
    };

    for (const WhisperWord &w : words) {
        if (lineWords > 0) {
            const bool pause   = prevEndMs >= 0 && (w.t0_ms - prevEndMs) > pauseThresholdMs;
            const bool endSent = lineWords >= 2 && endsSentence(prevText);
            const bool capital = lineWords >= kMinWordsForCap && startsNewPhrase(w.text);
            const bool tooLong = lineChars + 1 + w.text.size() > kMaxLineChars
                                 && lineWords >= kMinWordsForCap;
            if (pause || endSent || capital || tooLong)
                flushLine();
        }

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

        lineBody  += inlineTs + w.text + ' ';
        lineChars += (lineWords > 0 ? 1 : 0) + w.text.size();
        ++lineWords;
        prevEndMs = w.t1_ms;
        prevText  = w.text;
    }

    flushLine();

    return lines.join('\n') + '\n';
}

QString LrcBuilder::buildFromLines(const QVector<QVector<WhisperWord>> &lines)
{
    QStringList out;
    for (const QVector<WhisperWord> &line : lines) {
        if (line.isEmpty()) continue;
        QString body;
        for (const WhisperWord &w : line) {
            const qint64 min = w.t0_ms / 60000;
            const qint64 sec = (w.t0_ms % 60000) / 1000;
            const qint64 cs  = (w.t0_ms % 1000) / 10;
            body += QString("<%1:%2.%3>%4 ")
                        .arg(min, 2, 10, QChar('0'))
                        .arg(sec, 2, 10, QChar('0'))
                        .arg(cs,  2, 10, QChar('0'))
                        .arg(w.text);
        }
        out << formatTimestamp(line.first().t0_ms) + body.trimmed();
    }
    return out.isEmpty() ? QString() : out.join('\n') + '\n';
}
