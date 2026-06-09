// modules/GameInfoModule/TitleNormalizer.h
#pragma once
#include <QString>

namespace TitleNormalizer {
    // "The Witcher 3: Wild Hunt" → "The Witcher 3"
    QString normalize(const QString &rawTitle);
}
