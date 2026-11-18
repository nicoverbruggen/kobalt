#include "platforms/kobo/nickel-fixes/wifi-idle/wifi_idle.h"
#include "platforms/kobo/nickel-fixes/wifi-idle/nickelfix/sync/code_signature.h"
#include "platforms/kobo/nickel-fixes/wifi-idle/nickelfix/kepub-reader/detour.h"
#include <dlfcn.h>
#include <NickelHook.h>
#include <sys/reboot.h>
#include <unistd.h>

// NickelFix d735967 installs this adapter during plugin startup. If rollback
// fails, leave NickelHook's boot failsafe armed for the next boot.
extern "C" void ntf_patch_unsafe(const char *reason)
{
    nh_log("Wi-Fi idle initialization failed: %s", reason);
    nh_dump_log();
    sync();
    execl("/sbin/reboot", "reboot", static_cast<char *>(nullptr));
    reboot(RB_AUTOBOOT);
    _exit(1);
}

// Use the vendored NickelHook import installer only after preflight. A
// declarative optional hook would also redirect shutdown on unknown firmware.
extern "C" void *nh_dlhook(void *, const char *, void *);

namespace {
void (*original)(QObject *, int);

void batteryLevel(QObject *watchdog, int level)
{
    kobalt::nickel::evaluateInlineIdle(watchdog, level, original);
}
} // namespace

bool kobalt::nickel::installInlineIdle()
{
    using namespace nfx;
    const char *attempt = "_ZN16WirelessWatchdog13attemptToKillEv";

    // The hash covers the 192-byte Thumb slot, including both thresholds,
    // timestamp offsets and all conditional branches. Only references checked
    // against their actual destinations are normalized before hashing.
    const CodeReference calls[] = { { 14, "_ZN9QDateTime22currentMSecsSinceEpochEv" },
                                    { 86, "_ZN15WirelessManager14sharedInstanceEv" },
                                    { 90, "_ZNK15WirelessManager2upEv" },
                                    { 98, "_ZN6Device16getCurrentDeviceEv" },
                                    { 108, "_ZN8SettingsC2ERK6Deviceb" },
                                    { 124, "_ZN11DevSettings11forceWifiOnEv" },
                                    { 136, "_ZN8SettingsD2Ev" },
                                    { 148, attempt },
                                    { 154, "_ZN16BluetoothManager14sharedInstanceEv" },
                                    { 158, "_ZNK16BluetoothManager2upEv" },
                                    { 174, "_ZN8SettingsD2Ev" },
                                    { 178, "__cxa_end_cleanup" } };
    const CodeReference data[] = { { 188, "_ZTV11DevSettings" } };
    const CodeSignature sig = {
        192,   184,
        44,    "65601e27aa9c9e66cee015b53a7a8ffa402f2b76f69be8d817f0ec8d41698374",
        calls, sizeof(calls) / sizeof(*calls),
        data,  1
    };
    void *entry = dlsym(RTLD_DEFAULT, "_ZN16WirelessWatchdog14onBatteryLevelEi");
    const auto memory = ModuleMemory::containing(entry);
    const auto native = dlsym(RTLD_DEFAULT, attempt);
    const auto start = reinterpret_cast<uintptr_t>(entry) & ~uintptr_t(1);
    if (!memory.contains(reinterpret_cast<uintptr_t>(native) & ~uintptr_t(1), 2, Read | Execute)
        || !matchesCode(memory, start, sig, [](const char *s) {
               return reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, s));
           }))
        return false;

    // Confirm the inspected battery slot actually uses the redirected import.
    // A direct call with the same callee is insufficient: permissions would
    // otherwise run before the real heartbeat had been restored.
    void *lib = dlopen("libnickel.so.1.0.0", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
        return false;
    auto handler = reinterpret_cast<void *>(&kobalt_wifi_attempt);
    nativeAttempt = reinterpret_cast<decltype(nativeAttempt)>(native);
    void *prior = nh_dlhook(lib, attempt, handler);
    bool installed = false;
    if (prior) {
        nativeAttempt = reinterpret_cast<decltype(nativeAttempt)>(prior);
        void *trampoline = nullptr;
        installed = prior == native
                && callsSymbol(memory, start + 148, reinterpret_cast<uintptr_t>(handler))
                && ntf_detour_at(entry, reinterpret_cast<void *>(&batteryLevel), &trampoline,
                                 nullptr)
                        == 0;
        if (installed)
            original = reinterpret_cast<decltype(original)>(trampoline);
        else if (nh_dlhook(lib, attempt, prior) != handler)
            ntf_patch_unsafe("Cannot restore inline watchdog shutdown import");
    }
    dlclose(lib);
    return installed;
}
