#pragma once
#include <QXmlStreamReader>

namespace kobalt {
// Qt 5.2's XML reader expands entities without a limit. A short entity
// without references, or with one character reference such as an nbsp
// alias, cannot grow a document much. A reference to '&' or '%' would start
// another reference once the entity is declared, so it is refused.
inline bool unsafeEntities(const QXmlStreamReader &xml)
{
    for (const auto &entity : xml.entityDeclarations()) {
        const auto value = entity.value();
        const int references = value.count(QLatin1Char('&'));
        if (value.size() > 16)
            return true;
        if (references == 0)
            continue;
        if (references > 1 || !value.startsWith(QLatin1String("&#")))
            return true;
        const bool hex = value.size() > 2
                && (value.at(2) == QLatin1Char('x') || value.at(2) == QLatin1Char('X'));
        const int digits = hex ? 3 : 2;
        const int end = value.indexOf(QLatin1Char(';'));
        bool number = false;
        const uint code = end > digits
                ? value.mid(digits, end - digits).toString().toUInt(&number, hex ? 16 : 10)
                : 0;
        if (!number || code == '&' || code == '%')
            return true;
    }
    return false;
}
} // namespace kobalt
