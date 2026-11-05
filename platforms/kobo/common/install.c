#include <stddef.h>
#include <resources.h>
#include "embedded_resources.h"
#include "startup_recovery.h"

// NickelHook calls this before loading Nickel libraries or installing hooks.
bool nh_startup_failed(void)
{
    if (!kobalt_startup_remove(KOBALT_CONFIG_DIR "/startup-session"))
        return false;
    // Failure to save the explanation must not prevent recovery.
    kobalt_startup_removed(KOBALT_CONFIG_DIR "/startup-removed.txt");
    return true;
}

const struct nh_resources nh_resources = {
    .config_dir = KOBALT_CONFIG_DIR,
    .uninstall_file = KOBALT_CONFIG_DIR "/uninstall-now",
    .files = nh_embedded_resources,
    .initialized_file = KOBALT_CONFIG_DIR "/.initialized",
};
