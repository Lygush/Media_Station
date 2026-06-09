// modules/KaraokeModule/LrcParser.cpp
#include "LrcParser.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>

// ─────────────────────────────────────────────────────────────────────────────

LrcParser::LrcParser(QObject *parent) : QObject(parent) {}

// ── Парсинг тайм-кодов ────────────────────────────────────────────────────────

qint64 LrcParser::parseTimestamp(const QString &s)
{
    // Формат: [mm:ss.xx] или [mm:ss.xxx]
    static const QRegularExpression re(R"(\[(\d+):(\d+)\.(\d+)\])");
    const auto m = re.match(s);
    if (!m.hasMatch()) return -1;
    const qint64 min = m.captured(1).toLongLong();
    const qint64 sec = m.captured(2).toLongLong();
    const QString csStr = m.captured(3);
    // Нормализуем к миллисекундам: xx → *10, xxx → *1
    qint64 ms = csStr.toLongLong();
    if (csStr.length() == 2) ms *= 10;
    else if (csStr.length() == 1) ms *= 100;
    return min * 60000 + sec * 1000 + ms;
}

qint64 LrcParser::parseInlineTs(const QString &s)
{
    // Формат: <mm:ss.xx>
    static const QRegularExpression re(R"(<(\d+):(\d+)\.(\d+)>)");
    const auto m = re.match(s);
    if (!m.hasMatch()) return -1;
    const qint64 min = m.captured(1).toLongLong();
    const qint64 sec = m.captured(2).toLongLong();
    const QString csStr = m.captured(3);
    qint64 ms = csStr.toLongLong();
    if (csStr.length() == 2) ms *= 10;
    else if (csStr.length() == 1) ms *= 100;
    return min * 60000 + sec * 1000 + ms;
}

// ── load ──────────────────────────────────────────────────────────────────────

bool LrcParser::load(const QString &lrcPath)
{
    clear();

    QFile file(lrcPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[LrcParser] не удалось открыть:" << lrcPath;
        return false;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    // Regex для inline тайм-кодов внутри строки: <mm:ss.xx> текст
    static const QRegularExpression inlineRe(R"((<\d+:\d+\.\d+>)([^<]*))");

    // Метатеги [ti:], [ar:] и т.п. — пропускаем
    static const QRegularExpression metaRe(R"(^\[(?:ti|ar|al|by|offset|length|re|ve):)");

    while (!in.atEnd()) {
        const QString rawLine = in.readLine().trimmed();
        if (rawLine.isEmpty()) continue;

        // Пропускаем метатеги
        if (metaRe.match(rawLine).hasMatch()) continue;

        // Строка может иметь несколько тайм-кодов в начале (повторяющиеся строки)
        // Пример: [00:12.34][00:45.67] текст — берём первый
        const qint64 lineTs = parseTimestamp(rawLine);
        if (lineTs < 0) continue;

        // Отрезаем тайм-код строки ([mm:ss.xx])
        const int closeBracket = rawLine.indexOf(']');
        const QString rest = rawLine.mid(closeBracket + 1).trimmed();

        LrcLine line;
        line.startMs   = lineTs;
        line.plainText = rest;

        // Ищем inline тайм-коды
        auto it = inlineRe.globalMatch(rest);
        while (it.hasNext()) {
            const auto match = it.next();
            const qint64 wTs  = parseInlineTs(match.captured(1));
            const QString wTxt = match.captured(2).trimmed();
            if (wTs >= 0 && !wTxt.isEmpty()) {
                LrcWord word;
                word.startMs = wTs;
                word.text    = wTxt;
                line.words.append(word);
            }
        }

        // Если inline тайм-коды есть — обновляем plainText из них
        if (!line.words.isEmpty()) {
            QStringList parts;
            for (const LrcWord &w : line.words)
                parts << w.text;
            line.plainText = parts.join(' ');
        }

        m_lines.append(line);
    }

    // Сортируем по времени (на случай если файл не отсортирован)
    std::sort(m_lines.begin(), m_lines.end(),
              [](const LrcLine &a, const LrcLine &b){ return a.startMs < b.startMs; });

    qDebug() << "[LrcParser] загружено строк:" << m_lines.size() << "из" << lrcPath;
    return !m_lines.isEmpty();
}

void LrcParser::clear()
{
    m_lines.clear();
}

// ── Поиск активной строки / слова ─────────────────────────────────────────────

int LrcParser::activeLineIndex(qint64 posMs) const
{
    if (m_lines.isEmpty() || posMs < m_lines.first().startMs)
        return -1;

    // Бинарный поиск: последняя строка у которой startMs <= posMs
    int lo = 0, hi = m_lines.size() - 1, result = 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m_lines[mid].startMs <= posMs) { result = mid; lo = mid + 1; }
        else                                 hi = mid - 1;
    }
    return result;
}

int LrcParser::activeWordIndex(int lineIndex, qint64 posMs) const
{
    if (lineIndex < 0 || lineIndex >= m_lines.size()) return -1;
    const LrcLine &line = m_lines[lineIndex];
    if (line.words.isEmpty()) return -1;

    int result = 0;
    for (int i = 0; i < line.words.size(); ++i) {
        if (line.words[i].startMs <= posMs) result = i;
        else break;
    }
    return result;
}

// ── QML export ────────────────────────────────────────────────────────────────

QVariantList LrcParser::lines() const
{
    QVariantList result;
    result.reserve(m_lines.size());
    for (const LrcLine &line : m_lines) {
        QVariantMap lineMap;
        lineMap["startMs"]  = static_cast<qlonglong>(line.startMs);
        lineMap["text"]     = line.plainText;

        QVariantList words;
        for (const LrcWord &w : line.words) {
            QVariantMap wm;
            wm["startMs"] = static_cast<qlonglong>(w.startMs);
            wm["text"]    = w.text;
            words << wm;
        }
        lineMap["words"] = words;
        result << lineMap;
    }
    return result;
}
