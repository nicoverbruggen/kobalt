#pragma once

namespace kobalt {
enum class PageAnimationSupport { Unavailable, Supported, Uncertain };

inline bool pageAnimationsAvailable(PageAnimationSupport support)
{
    return support != PageAnimationSupport::Unavailable;
}

inline bool pageAnimationsEnabled(PageAnimationSupport support, bool dark)
{
    // Uncertain is the older i.MX backend. Keep its saved preference, but
    // suppress the sweep in dark mode regardless of the board revision.
    return pageAnimationsAvailable(support)
            && !(dark && support == PageAnimationSupport::Uncertain);
}
} // namespace kobalt
