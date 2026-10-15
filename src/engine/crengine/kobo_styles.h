#pragma once
#include <QByteArray>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace kobalt {
// Remove only the complete known injection. A reused marker can contain custom CSS.
inline QByteArray withoutKoboStyleHacks(const QByteArray &bytes) {
    if (!bytes.contains("kobostylehacks")) {
        return bytes;
    }
    const auto text = QString::fromUtf8(bytes);
    // Kobo's injection is UTF-8. Preserve other encodings and invalid input verbatim.
    if (text.toUtf8() != bytes) {
        return bytes;
    }
    static const QRegularExpression original(QStringLiteral(
        "^\\s*div#book-inner\\s+p\\s*,\\s*div#book-inner\\s+div\\s*\\{\\s*font-size\\s*:\\s*1\\.0em\\s*;\\s*\\}"
        "\\s*a\\s*\\{\\s*color\\s*:\\s*black\\s*;\\s*\\}"
        "\\s*a:link\\s*,\\s*a:visited\\s*,\\s*a:hover\\s*,\\s*a:active\\s*\\{\\s*color\\s*:\\s*blue\\s*;\\s*\\}"
        "\\s*div#book-inner\\s+\\*\\s*\\{\\s*margin-top\\s*:\\s*0\\s*!important\\s*;"
        "\\s*margin-bottom\\s*:\\s*0\\s*!important\\s*;\\s*\\}\\s*$"));
    QXmlStreamReader xml(text);
    QList<QPair<int, int>> removals;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QStringLiteral("style") ||
            !(xml.namespaceUri().isEmpty() || xml.namespaceUri() == QStringLiteral("http://www.w3.org/1999/xhtml"))) {
            continue;
        }
        const auto attributes = xml.attributes();
        const bool marked = attributes.value("id") == QStringLiteral("kobostylehacks") ||
                            attributes.value("class")
                                .toString()
                                .split(QRegularExpression("\\s+"))
                                .contains(QStringLiteral("kobostylehacks"));
        if (!marked) {
            continue;
        }
        const int start = int(xml.characterOffset());
        const auto css = xml.readElementText();
        const int end = int(text.lastIndexOf('<', int(xml.characterOffset()) - 1));
        if (!xml.hasError() && end >= start && original.match(css).hasMatch()) {
            removals.append({start, end - start});
        }
    }
    if (xml.hasError() || removals.isEmpty()) {
        return bytes;
    }
    auto cleaned = text;
    for (int i = removals.size() - 1; i >= 0; --i) {
        cleaned.remove(removals[i].first, removals[i].second);
    }
    return cleaned.toUtf8();
}
}
