#pragma once
#include "platforms/kobo/common/native_symbol.h"
#include <QMetaType>

// Nickel types of firmware 5 and 6 that several adapters use.
namespace kobalt {
// The meta type that Nickel registered for one of its classes. `wrapper` is
// the mangled name of its QMetaTypeInterfaceWrapper<T>::metaType. The result
// is invalid, with size 0, when this firmware does not have it.
inline QMetaType nativeType(const char *wrapper)
{
    return QMetaType(symbol<const QtPrivate::QMetaTypeInterface *>(wrapper));
}

// QtPrivate::QMetaTypeInterfaceWrapper<Volume>::metaType
constexpr char volumeType[] = "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI6VolumeE8metaTypeE";

// Storage for a Volume that Nickel returns by value. The adapters compare
// its size with the registered size before they use it.
struct Volume
{
    void *words[3];
    Volume(const Volume &) = delete;

    ~Volume() { symbol<void (*)(void *)>("_ZN6VolumeD1Ev")(this); }
};

static_assert(sizeof(Volume) == 12);
} // namespace kobalt
