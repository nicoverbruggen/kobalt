#pragma once
#include <QString>
#include <QRegularExpression>
#include <QUrl>

// Firmware 4 stores a reading position as a chapter path and an optional
// element ID, for example "OEBPS/chapter1.xhtml#kobo.9.3". It has no text
// offset. The shared reader uses the same parts with percent-encoding and an
// optional ":offset". In a plain EPUB without IDs, Nickel stores a DOM point
// with its own offset instead, for example
// "OEBPS/chapter1.xhtml#point(/1/4/24/1:0)". Both sides keep that form as is.
namespace kobalt::nickel_qt5 {
inline bool domPoint(const QString &fragment)
{
    static const QRegularExpression format(
            QStringLiteral("^point\\(/1(?:/[1-9][0-9]*)*(?::[0-9]+)?\\)$"));
    return format.match(fragment).hasMatch();
}

// Nickel's sideloaded IDs contain literal paths, not percent-encoded URLs.
inline QString localBookPath(const QString &book)
{
    return book.startsWith("file:///") ? book.mid(7) : QString();
}

// Older Kobo bookmarks prefix the chapter path with the book and use ! for
// path separators. Firmware 4 chapter rows use a plain file path as that
// prefix. Only decode the form when it belongs to this publication.
inline QString chapter(const QString &book, const QString &raw)
{
    for (const auto &prefix : { book + '!', localBookPath(book) + '!' }) {
        if (prefix.size() < 2 || !raw.startsWith(prefix))
            continue;
        const int hash = raw.indexOf('#', prefix.size());
        auto path = raw.mid(prefix.size(), hash < 0 ? -1 : hash - prefix.size());
        path.replace('!', '/');
        return path + (hash < 0 ? QString() : raw.mid(hash));
    }
    return raw;
}

inline QString shared(const QString &raw)
{
    const int hash = raw.indexOf('#');
    const auto path = QString::fromLatin1(
            QUrl::toPercentEncoding(raw.left(hash < 0 ? raw.size() : hash), "/"));
    if (hash < 0)
        return path;
    const auto fragment = raw.mid(hash + 1);
    return path + '#'
            + (domPoint(fragment) ? fragment
                                  : QString::fromLatin1(QUrl::toPercentEncoding(fragment)));
}

// Nickel receives the element that contains the position. The exact offset
// stays in Kobalt's local checkpoint. Returns an empty string if unsupported.
inline QString stored(const QString &position)
{
    const int hash = position.indexOf('#');
    const auto path =
            QUrl::fromPercentEncoding(position.left(hash < 0 ? position.size() : hash).toUtf8());
    if (path.isEmpty() || path.contains('#'))
        return {};
    if (hash < 0)
        return path;
    auto id = position.mid(hash + 1);
    if (domPoint(id))
        return path + '#' + id;
    if (id.startsWith(QLatin1String("point(")))
        return {};
    const int colon = id.lastIndexOf(':');
    if (colon >= 0) {
        bool ok = false;
        if (id.mid(colon + 1).toInt(&ok) < 0 || !ok)
            return {};
        id.truncate(colon);
    }
    id = QUrl::fromPercentEncoding(id.toUtf8());
    if (id.isEmpty() || id.contains('#'))
        return {};
    return path + '#' + id;
}
} // namespace kobalt::nickel_qt5
