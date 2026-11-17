#pragma once
#include <dlfcn.h>
#include <unistd.h>

namespace kobalt {
// NickelHook temporarily renames a loaded plugin while its failsafe is armed.
inline bool nickelFixInstalled(const char *path = "/usr/local/Kobo/imageformats/libnickelfix.so")
{
    if (access(path, F_OK) == 0)
        return true;
    void *loaded = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (!loaded)
        return false;
    dlclose(loaded);
    return true;
}
} // namespace kobalt
