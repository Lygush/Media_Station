// modules/GameInfoModule/HtmlStripper.h
#pragma once
#include <QString>

namespace HtmlStripper {
    // Очищает HTML → чистый читаемый текст, обрезает до maxChars символов
    QString strip(const QString &html, int maxChars = 16000);
}
