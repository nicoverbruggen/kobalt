#include "platforms/kobo/common/screensaver_compatibility.h"
#include <QJsonObject>
#include <cstdio>
#include <dlfcn.h>
#include <link.h>

namespace {
using Callback = int (*)(dl_phdr_info *, size_t, void *);
bool enumerating = false;
int callbacks = 0, opens = 0, nestedOpens = 0;

struct Visitor
{
    Callback callback;
    void *data;
};
} // namespace

extern "C" int __real_dl_iterate_phdr(Callback, void *);
extern "C" void *__real_dlopen(const char *, int);

extern "C" int __wrap_dl_iterate_phdr(Callback callback, void *data)
{
    Visitor visitor{ callback, data };
    return __real_dl_iterate_phdr(
            [](dl_phdr_info *info, size_t size, void *context) {
                const auto &visitor = *static_cast<Visitor *>(context);
                ++callbacks;
                enumerating = true;
                const int result = visitor.callback(info, size, visitor.data);
                enumerating = false;
                return result;
            },
            &visitor);
}

extern "C" void *__wrap_dlopen(const char *path, int flags)
{
    ++opens;
    if (enumerating)
        ++nestedOpens;
    return __real_dlopen(path, flags);
}

// Only the compatibility check is under test; no native power manager is used.
extern "C" void kobalt_power_off(void *, bool) { }

namespace kobalt {
void (*nativePowerOff)(void *, bool) = nullptr;

void diagnostic(const char *, QJsonObject, bool) { }
} // namespace kobalt

// Checks that Screensaver discovery opens library handles only after `dl_iterate_phdr` returns.
// This prevents a loader-lock inversion with concurrent library loading.
int main()
{
    if (!kobalt::connectScreensaverPowerOff() || callbacks == 0 || opens == 0 || nestedOpens != 0) {
        std::fprintf(stderr, "Library inspection: callbacks=%d opens=%d opens inside callback=%d\n",
                     callbacks, opens, nestedOpens);
        return 1;
    }
    return 0;
}
