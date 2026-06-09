// modules/GameInfoModule/HtmlStripper.cpp
#include "HtmlStripper.h"
#include <QRegularExpression>

namespace HtmlStripper {

QString strip(const QString &html, int maxChars)
{
    QString text = html;

    // Удаляем целые блоки вместе с содержимым
    static const QStringList kBlockTags = {
        "script", "style", "nav", "header", "footer",
        "aside", "noscript", "svg", "form"
    };
    for (const QString &tag : kBlockTags) {
        QRegularExpression re(
            QString(R"(<%1[\s>][\s\S]*?</%1\s*>)").arg(tag),
            QRegularExpression::CaseInsensitiveOption
        );
        text.remove(re);
    }

    // Параграфы и заголовки → перенос строки
    text.replace(QRegularExpression(
        R"(<br\s*/?>|</(p|div|h[1-6]|li|tr|section|article)>)",
        QRegularExpression::CaseInsensitiveOption), "\n");

    // Убираем все оставшиеся теги
    text.remove(QRegularExpression(R"(<[^>]+>)"));

    // HTML entities
    text.replace("&amp;",  "&");
    text.replace("&lt;",   "<");
    text.replace("&gt;",   ">");
    text.replace("&nbsp;", " ");
    text.replace("&quot;", "\"");
    text.replace("&#39;",  "'");
    text.replace("&mdash;","—");
    text.replace("&ndash;","–");
    text.replace("&laquo;","«");
    text.replace("&raquo;","»");

    // Убираем декоративные символы которые мешают LLM
    text.remove(QChar(0x2122)); // ™
    text.remove(QChar(0x00AE)); // ®
    text.remove(QChar(0x00A9)); // ©
    // Emoji и прочие символы вне BMP (суррогатные пары в UTF-16)
    text.remove(QRegularExpression("[\\x{1F000}-\\x{1FFFF}]"));

    // Схлопываем пробелы и пустые строки
    text.replace(QRegularExpression(R"([^\S\n]+)"), " ");
    text.replace(QRegularExpression(R"(\n{3,})"), "\n\n");

    text = text.trimmed();

    // Обрезаем до maxChars по границе слова чтобы не рвать текст
    if (text.length() > maxChars) {
        text = text.left(maxChars);
        // Откатываемся до последнего пробела или переноса строки
        int cut = text.lastIndexOf(QRegularExpression("[ \n]"));
        if (cut > maxChars / 2)
            text = text.left(cut);
        text = text.trimmed();
    }

    return text;
}

} // namespace HtmlStripper
