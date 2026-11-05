#pragma once

#include <QString>

namespace kobalt {
// Nickel's local ContentIDs contain literal absolute paths. Do not URL-decode
// them: #, ? and percent sequences can be part of the filename. The caller
// checks the volume's format and protection metadata, then inspects the file.
inline QString localReaderBookPath(const QString &identity)
{
    return identity.startsWith("file:///") ? identity.mid(7) : QString();
}

// Compatible sync servers can use numeric or other string IDs. Preserve the
// ID as one filename inside Nickel's download directory, never as a URL.
inline QString downloadedReaderBookPath(const QString &identity)
{
    if (identity.isEmpty() || identity == "." || identity == ".." || identity.contains('/')
        || identity.contains('\\') || identity.contains(QChar(0))) {
        return {};
    }
    return QStringLiteral("/mnt/onboard/.kobo/kepub/") + identity;
}
} // namespace kobalt
