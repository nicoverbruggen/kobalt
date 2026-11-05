#pragma once
#include "reading/reading_bridge.h"

namespace kobalt::nickel {
// Session analytics reads the Volume, not PluginState.percent. Prepare only
// the detached event value: the normal progress save still owns persistence.
template <class Volume, class Bookmark, class Percent>
void prepareSessionVolume(Volume &volume, const ReadingActivity &activity, Bookmark bookmark,
                          Percent percent)
{
    if (!activity.nativePosition.isEmpty())
        bookmark(volume, activity.nativePosition);
    // updateBookmark can change the percentage too. Apply the reader's native
    // percentage last, with the same unfinished-book cap as progress writeback.
    percent(volume, qBound(0.0, activity.percent, 99.0));
}
} // namespace kobalt::nickel
