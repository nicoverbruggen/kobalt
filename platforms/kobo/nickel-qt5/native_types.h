#pragma once
#include "platforms/kobo/common/native_symbol.h"

// Nickel types of firmware 4 that several adapters use.
namespace kobalt {
// Storage for a Volume that Nickel returns by value. The adapters compare
// its size with the size of the registered "Volume" meta type before they
// use it.
struct Volume
{
    void *words[2];
    Volume(const Volume &) = delete;

    ~Volume() { symbol<void (*)(void *)>("_ZN6VolumeD1Ev")(this); }
};

static_assert(sizeof(Volume) == 8);
} // namespace kobalt
