#pragma once
#include <dlfcn.h>

namespace kobalt {
// Finds a function or object of the running firmware by its mangled name.
// Returns null when this firmware does not have it. Keep the full mangled
// name at the call site, so a search for it finds every use.
template <class T>
T symbol(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
} // namespace kobalt
