#include "screensaver_compatibility.h"
#include "native_lifecycle.h"
#include "support/diagnostics.h"
#include "../toolchains/nickelhook-qt6/NickelHook.h"
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#else
#  include <link.h>
#endif

extern "C" __attribute__((visibility("default"))) void kobalt_power_off(void *, bool);

namespace kobalt {
namespace {
constexpr auto symbol = "_ZN22N3PowerWorkflowManager8powerOffEb";
constexpr auto replacement = "hook_N3PowerWorkflowManager_powerOff";

struct Inspection
{
    int found = 0;
    bool ready = true;
    void **reconnect = nullptr;
};

void inspect(const char *path, const void *base, Inspection &result)
{
    if (!path || !*path)
        return;
    // NOLOAD is essential: this check must not initialize another plugin.
    auto *handle = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (!handle)
        return;
    auto *mod = static_cast<nh *>(dlsym(handle, "NickelHook"));
    Dl_info owner{};
    if (!mod || !dladdr(mod, &owner) || owner.dli_fbase != base || !mod->info || !mod->info->name
        || std::strcmp(mod->info->name, "Nickel Screensaver")) {
        dlclose(handle);
        return;
    }
    ++result.found;
    bool ready = false;
    auto *stock = dlsym(RTLD_DEFAULT, symbol);
    auto *hook = dlsym(handle, replacement);
    auto *ours = reinterpret_cast<void *>(kobalt_power_off);
    auto *next = reinterpret_cast<void *>(nativePowerOff);
    Dl_info hookOwner{};
    if (stock && hook && dladdr(hook, &hookOwner) && hookOwner.dli_fbase == base && mod->hook) {
        for (auto *entry = mod->hook; entry->sym; ++entry) {
            if (std::strcmp(entry->sym, symbol) || !entry->sym_new
                || std::strcmp(entry->sym_new, replacement) || !entry->lib
                || std::strcmp(entry->lib, "libnickel.so.1.0.0") || !entry->out)
                continue;
            // Kobalt loaded last: its own hook already wraps Screensaver.
            // Screensaver loaded last: its older NickelHook discarded ours.
            // Insert our save gate into its call-through, leaving its image
            // preparation and stock power-view hook intact. Never reapply a
            // GOT hook blindly: that can create a cycle with a cooperating mod.
            if (next == hook && *entry->out == stock) {
                ready = true;
            } else if (next == stock && (*entry->out == stock || *entry->out == ours)) {
                result.reconnect = entry->out;
                ready = true;
            }
            break;
        }
    }
    result.ready = result.ready && ready;
    dlclose(handle);
}
} // namespace

bool connectScreensaverPowerOff()
{
    Inspection result;
#ifdef __APPLE__
    for (uint32_t i = 0; i < _dyld_image_count(); ++i)
        inspect(_dyld_get_image_name(i), _dyld_get_image_header(i), result);
#else
    struct Library
    {
        std::string path;
        const void *base;
    };

    std::vector<Library> libraries;
    // The callback holds a loader lock. Calling dlopen here can deadlock
    // against another thread loading a library under the other loader lock.
    // Own the names before returning; the loader's pointers can then expire.
    dl_iterate_phdr(
            [](dl_phdr_info *info, size_t, void *data) {
                if (info->dlpi_name && *info->dlpi_name)
                    static_cast<std::vector<Library> *>(data)->push_back(
                            { info->dlpi_name, reinterpret_cast<void *>(info->dlpi_addr) });
                return 0;
            },
            &libraries);
    for (const auto &library : libraries)
        inspect(library.path.c_str(), library.base, result);
#endif
    const bool ready = result.ready && result.found <= 1;
    if (ready && result.reconnect)
        *result.reconnect = reinterpret_cast<void *>(kobalt_power_off);
    if (result.found)
        diagnostic("screensaver-power-hook", { { "ready", ready }, { "instances", result.found } },
                   false);
    return ready;
}
} // namespace kobalt
