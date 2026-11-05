#pragma once

namespace kobalt {
enum class PageAnimationSupport { Unavailable, Supported, Uncertain };

inline bool pageAnimationsAvailable(PageAnimationSupport support)
{
    return support != PageAnimationSupport::Unavailable;
}
} // namespace kobalt
