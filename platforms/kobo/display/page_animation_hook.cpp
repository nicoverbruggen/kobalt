#include "page_animation_hook.h"
#include "page_animation.h"
#include "support/diagnostics.h"
#include <dlfcn.h>
#include "framebuffer_driver.h"

extern "C" void *nh_dlhook(void *handle, const char *symbol, void *target);

namespace kobalt {
AnimationPanel pageAnimationPanel()
{
    return animationPanel(qgetenv("PRODUCT"), framebufferDriver());
}

PageAnimationSupport pageAnimationSupport()
{
#ifdef KOBALT_PAGE_ANIMATIONS
    return animationSupport(framebufferDriver(), true, nextAnimationIoctl != nullptr);
#else
    return PageAnimationSupport::Unavailable;
#endif
}

void installPageAnimationHook()
{
#ifdef KOBALT_PAGE_ANIMATIONS
    // Older NickelHook mods, including released NickelDissolve builds, replace
    // a GOT entry without keeping its previous hook. Installing during plugin
    // discovery lets a later mod silently disconnect Kobalt. Wait until the
    // reader is created, after startup plugin discovery. Our loader preserves
    // the current entry, so stock reader updates still reach the other mod.
    // Install once: repeatedly inserting ourselves could create a hook cycle.
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;
    if (pageAnimationPanel() == AnimationPanel::None)
        return;
    void *library = dlopen("/usr/local/Kobo/platforms/libkobo.so", RTLD_LAZY | RTLD_NOLOAD);
    if (library) {
        nextAnimationIoctl = reinterpret_cast<PageAnimation::Ioctl>(
                nh_dlhook(library, "ioctl", reinterpret_cast<void *>(kobalt_animation_ioctl)));
        dlclose(library);
    }
    diagnostic("animation-hook", { { "installed", bool(nextAnimationIoctl) } }, false);
#endif
}
} // namespace kobalt
