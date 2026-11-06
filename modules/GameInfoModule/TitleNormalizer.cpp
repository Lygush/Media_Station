// modules/GameInfoModule/TitleNormalizer.cpp
#include "TitleNormalizer.h"
#include <QRegularExpression>

namespace TitleNormalizer {

QString normalize(const QString &rawTitle)
{
    QString title = rawTitle.trimmed();

    // Подзаголовок после «: » здесь НЕ отрезается: «Star Wars Jedi: Fallen Order»
    // без него превращается в «Star Wars Jedi», «Batman: Arkham City» — в «Batman»,
    // и поиск находит другую игру. Вариант без подзаголовка — в searchVariants().

    // Мусор в конце (без учёта регистра)
    static const QStringList kJunk = {
        "Enhanced Edition", "GOTY", "Game of the Year Edition",
        "Remastered", "Complete Edition", "Gold Edition",
        "Director's Cut", "Definitive Edition", "Anniversary Edition",
        "Ultimate Edition", "Deluxe Edition", "Special Edition"
    };
    for (const QString &junk : kJunk) {
        QRegularExpression re(
            QString(R"(\s*[-–—:]?\s*\(?\s*%1\s*\)?\s*$)").arg(QRegularExpression::escape(junk)),
            QRegularExpression::CaseInsensitiveOption
        );
        title = title.remove(re).trimmed();
    }

    // Версии в скобках: (v2.1), (1.5), и т.п.
    title = title.remove(QRegularExpression(R"(\s*\(v?[\d\.]+\)\s*$)")).trimmed();

    // Римские цифры → арабские: только заглавными, только последним словом и
    // только из двух и более букв. Одиночные I/V/X не трогаем: «I Am Bread»,
    // «Mega Man X» (это не 10), «Final Fantasy X» Steam и так найдёт.
    static const QList<QPair<QString,QString>> kRoman = {
        {"VIII","8"},{"VII","7"},{"III","3"},{"XII","12"},{"XIII","13"},
        {"XI","11"},{"IX","9"},{"VI","6"},{"IV","4"},{"II","2"}
    };
    for (const auto &[roman, arabic] : kRoman) {
        const QRegularExpression re(QString(R"((?<=\s)%1$)").arg(roman));
        if (title.contains(re)) {
            title.replace(re, arabic);
            break;
        }
    }

    return title.trimmed();
}

QString withoutSubtitle(const QString &title)
{
    // Только «: » с пробелом — слитные названия («NieR:Automata») не трогаем
    const int colonIdx = title.indexOf(QLatin1String(": "));
    if (colonIdx > 0)
        return title.left(colonIdx).trimmed();
    return title;
}

QStringList searchVariants(const QString &rawTitle)
{
    const QString full = normalize(rawTitle);
    QStringList out{ full };
    const QString shortTitle = withoutSubtitle(full);
    if (!shortTitle.isEmpty() && shortTitle != full)
        out << shortTitle;
    return out;
}

} // namespace TitleNormalizer
