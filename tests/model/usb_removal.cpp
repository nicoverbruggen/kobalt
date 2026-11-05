#include "platforms/kobo/common/usb_removal.h"
#include <cstdlib>

void check(bool value)
{
    if (!value)
        std::abort();
}

// Checks that a USB session that removed Kobalt's marker reports the removal once, and that other sessions do not.
int main()
{
    kobalt::UsbRemoval removal;
    check(!removal.finish(false));
    removal.begin(false, true); // First installation has no receipt yet.
    check(!removal.finish(false));
    removal.begin(true, false); // An already absent marker is not a new removal.
    check(!removal.finish(false));
    removal.begin(true, true);
    check(!removal.finish(true)); // Connecting without removal does not restart.
    check(!removal.finish(false)); // Ordinary scans cannot reuse a finished session.
    removal.begin(true, true);
    check(removal.finish(false));
    check(!removal.finish(false)); // Duplicate completion restarts only once.
    removal.begin(true, true);
    check(removal.finish(false)); // A later independent USB session still works.
}
