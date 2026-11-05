// A separately loaded old-style NickelHook plugin. Its call-through storage
// is deliberately hidden; compatibility must find it through the descriptor.
#include "platforms/kobo/toolchains/nickelhook-qt6/NickelHook.h"

namespace {
void (*next)(void *, bool) = nullptr;
int calls = 0;
nh_info info = { "Nickel Screensaver", nullptr, nullptr, nullptr, 0 };
nh_hook hooks[] = { { "_ZN22N3PowerWorkflowManager8powerOffEb",
                      "hook_N3PowerWorkflowManager_powerOff", "libnickel.so.1.0.0",
                      reinterpret_cast<void **>(&next), nullptr, true },
                    { nullptr, nullptr, nullptr, nullptr, nullptr, false } };
} // namespace

extern "C" {
__attribute__((visibility("default"))) nh NickelHook = { nullptr, &info, hooks, nullptr, nullptr };

__attribute__((visibility("default"))) void hook_N3PowerWorkflowManager_powerOff(void *manager,
                                                                                 bool low)
{
    ++calls;
    next(manager, low);
}

__attribute__((visibility("default"))) void fixtureReset(void (*callback)(void *, bool))
{
    next = callback;
    calls = 0;
}

__attribute__((visibility("default"))) int fixtureCalls()
{
    return calls;
}
}
