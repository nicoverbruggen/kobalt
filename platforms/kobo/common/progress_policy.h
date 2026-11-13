#pragma once
#include "reading/reading_progress.h"
#include "reading/native_sync_policy.h"
#include <QRegularExpression>

namespace kobalt::nickel {
// Nickel may remove a saved KEPUB character offset without changing its date.
// The reverse transition is a new position and still needs a choice.
inline bool acceptsPositionNormalization(const ReadingProgress &saved,
                                         const ReadingProgress &incoming)
{
    if (!saved.valid || !incoming.valid || saved.book != incoming.book
        || saved.status != incoming.status || saved.modified != incoming.modified)
        return false;
    static const QRegularExpression position(
            QStringLiteral("\\A([^#]+#kobo\\.[0-9]+\\.[0-9]+):([0-9]+)\\z"));
    const auto match = position.match(saved.location);
    bool validOffset = false;
    const int offset = match.captured(2).toInt(&validOffset);
    if (!match.hasMatch() || !validOffset || offset <= 0)
        return false;
    const auto span = match.captured(1);
    return incoming.location == span || incoming.location == span + QLatin1String(":0");
}

// Every supported native book can record activity when its position cannot
// be mapped. The adapter preserves the bookmark and sync flag and sends no
// bookmark upload for an activity-only save.
inline bool allowsUnmappedPosition(const QString &identity)
{
    return !identity.isEmpty();
}

inline NativeSyncPolicy nativeSyncPolicy(const QString &identity, bool koboCloud = false)
{
    return { koboCloud || !identity.startsWith("file:///"),
             koboCloud ? QStringLiteral("kosync.kobo_cloud")
                       : QStringLiteral("kosync.native_sync") };
}
} // namespace kobalt::nickel
