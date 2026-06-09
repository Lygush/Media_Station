// modules/GameInfoModule/TitleNormalizer.cpp
#include "TitleNormalizer.h"
#include <QRegularExpression>

namespace TitleNormalizer {

QString normalize(const QString &rawTitle)
{
    QString title = rawTitle.trimmed();

    // Убираем subtitle после двоеточия только если после него идёт пробел
    // (т.е. "The Witcher 3: Wild Hunt" → "The Witcher 3")
    // но не трогаем слитные названия ("NieR:Automata", "Prey:2017")
    const int colonIdx = title.indexOf(':');
    if (colonIdx > 0 && colonIdx < title.length() - 1) {
        if (title[colonIdx + 1] == ' ')
            title = title.left(colonIdx).trimmed();
    }

    // Мусор в конце (без учёта регистра)
    static const QStringList kJunk = {
        "Enhanced Edition", "GOTY", "Game of the Year Edition",
        "Remastered", "Complete Edition", "Gold Edition",
        "Director's Cut", "Definitive Edition", "Anniversary Edition",
        "Ultimate Edition", "Deluxe Edition", "Special Edition"
    };
    for (const QString &junk : kJunk) {
        QRegularExpression re(
            QString(R"(\s*[-–—]?\s*\(?\s*%1\s*\)?\s*$)").arg(QRegularExpression::escape(junk)),
            QRegularExpression::CaseInsensitiveOption
        );
        title = title.remove(re).trimmed();
    }

    // Версии в скобках: (v2.1), (1.5), и т.п.
    title = title.remove(QRegularExpression(R"(\s*\(v?[\d\.]+\)\s*$)")).trimmed();

    // Римские цифры → арабские (только отдельное слово)
    static const QList<QPair<QString,QString>> kRoman = {
        {"VIII","8"},{"VII","7"},{"VI","6"},{"IX","9"},{"IV","4"},
        {"III","3"},{"II","2"},{"XI","11"},{"X","10"},{"I","1"},{"V","5"}
    };
    for (const auto &[roman, arabic] : kRoman) {
        QRegularExpression re(
            QString(R"(\b%1\b)").arg(roman),
            QRegularExpression::CaseInsensitiveOption
        );
        title = title.replace(re, arabic);
    }

    return title.trimmed();
}

} // namespace TitleNormalizer
