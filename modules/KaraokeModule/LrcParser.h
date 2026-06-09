// modules/KaraokeModule/LrcParser.h
// ─────────────────────────────────────────────────────────────────────────────
// LrcParser — парсинг Enhanced LRC файла в структуры для QML.
//
// Поддерживает два формата:
//   Enhanced LRC:  [mm:ss.xx]<mm:ss.xx> слово <mm:ss.xx> слово
//   Simple LRC:    [mm:ss.xx] строка целиком (без inline тайм-кодов)
//
// QML получает список LrcLine, каждая строка содержит список LrcWord
// с тайм-кодами. QML по текущей позиции плеера подсвечивает активное слово.
//
// Использование:
//   LrcParser parser;
//   parser.load("/path/to/song.lrc");
//   QVariantList lines = parser.lines();   // передаётся в QML
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QVariantList>

struct LrcWord {
    qint64  startMs = 0;   ///< время начала слова
    QString text;           ///< текст слова
};

struct LrcLine {
    qint64          startMs = 0;   ///< время начала строки (из [mm:ss.xx])
    QVector<LrcWord> words;         ///< слова с inline тайм-кодами (может быть пустым)
    QString         plainText;      ///< строка целиком (для Simple LRC)
};

class LrcParser : public QObject
{
    Q_OBJECT

public:
    explicit LrcParser(QObject *parent = nullptr);

    /// Загружает и парсит .lrc файл.
    /// @return true при успехе
    Q_INVOKABLE bool load(const QString &lrcPath);

    /// Сбрасывает состояние.
    Q_INVOKABLE void clear();

    /// Возвращает индекс активной строки для позиции posMs.
    /// -1 если позиция до первой строки.
    Q_INVOKABLE int  activeLineIndex(qint64 posMs) const;

    /// Возвращает индекс активного слова в строке lineIndex для позиции posMs.
    /// -1 если строка без inline тайм-кодов.
    Q_INVOKABLE int  activeWordIndex(int lineIndex, qint64 posMs) const;

    /// Все строки как QVariantList для передачи в QML.
    /// Каждый элемент — QVariantMap: { "startMs": int, "text": string,
    ///                                  "words": [ { "startMs": int, "text": string } ] }
    Q_INVOKABLE QVariantList lines() const;

    /// Количество строк.
    Q_INVOKABLE int lineCount() const { return m_lines.size(); }

    /// true если файл успешно загружен и содержит строки.
    Q_INVOKABLE bool isValid() const { return !m_lines.isEmpty(); }

private:
    static qint64 parseTimestamp(const QString &s);   ///< "[mm:ss.xx]" → мс
    static qint64 parseInlineTs (const QString &s);   ///< "<mm:ss.xx>"  → мс

    QVector<LrcLine> m_lines;
};
