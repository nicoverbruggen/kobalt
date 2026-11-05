#pragma once

namespace kobalt::nickel_qt5 {
// A borrowed view of firmware 4's accepted showReader arguments. NativeOpening
// copies their values before the hook returns; this is not a native ABI struct.
struct OpeningContext
{
    const void *keys;
    void *library;
    const void *bookmark;
};
} // namespace kobalt::nickel_qt5
