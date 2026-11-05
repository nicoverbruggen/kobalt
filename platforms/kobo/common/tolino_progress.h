#pragma once
#include <QVariantMap>
#include <cmath>

namespace kobalt {
inline bool tolinoBookmarkMatches(const QVariantMap &bookmark, const QString &position, int page,
                                  int pages)
{
    bool currentValid = false, totalValid = false, fractionValid = false, modifiedValid = false;
    const int current = bookmark.value("currentPosition").toInt(&currentValid);
    const int total = bookmark.value("lastPosition").toInt(&totalValid);
    const double fraction = bookmark.value("progress").toDouble(&fractionValid);
    const auto modified = bookmark.value("modified").toLongLong(&modifiedValid);
    return pages > 0 && page >= 0 && page <= pages
            && bookmark.value("position").toString() == position && currentValid && totalValid
            && current == page && total == pages && fractionValid && std::isfinite(fraction)
            && qAbs(fraction - double(page) / pages) < 0.000001 && modifiedValid && modified > 0
            && bookmark.contains("deletedLocally") && !bookmark.value("deletedLocally").toBool();
}
} // namespace kobalt
