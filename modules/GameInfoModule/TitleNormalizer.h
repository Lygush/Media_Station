// modules/GameInfoModule/TitleNormalizer.h
#pragma once
#include <QString>
#include <QStringList>

namespace TitleNormalizer {
    // Убирает «издания» и версии в конце, II/III/… в конце → 2/3/…
    // Подзаголовок сохраняется: "The Witcher 3: Wild Hunt GOTY" → "The Witcher 3: Wild Hunt"
    QString normalize(const QString &rawTitle);

    // "The Witcher 3: Wild Hunt" → "The Witcher 3"
    QString withoutSubtitle(const QString &title);

    // Варианты для поиска по порядку: полное название, затем без подзаголовка
    // (только если по полному ничего не нашлось).
    QStringList searchVariants(const QString &rawTitle);
}
