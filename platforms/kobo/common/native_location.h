#pragma once
#include <QString>

namespace kobalt {
// Older Kobo bookmarks prefix the spine path with the book ID and use ! for
// path separators. Only decode that form when it belongs to this publication.
inline QString publicationLocation(const QString &book, const QString &location)
{
    if (book.isEmpty() || !location.startsWith(book + '!'))
        return location;
    const int fragment = location.indexOf('#', book.size() + 1);
    auto path = location.mid(book.size() + 1, fragment < 0 ? -1 : fragment - book.size() - 1);
    path.replace('!', '/');
    return path + (fragment < 0 ? QString() : location.mid(fragment));
}

// Nickel stores "#", an empty chapter and anchor, for a downloaded book that
// has no position yet, such as a store book whose server sent none. It means
// the start of the book, like an empty value. Pass the decoded location.
inline bool unsetNativeLocation(const QString &location)
{
    return location.isEmpty() || location == QLatin1String("#");
}
} // namespace kobalt
