#include "platform/api/kobo_refresh.h"
#include "platform/api/loopback.h"
#include <cerrno>

namespace kobalt::platform {
// The desktop Qt backing store coalesces ordinary widget updates. Kobo waveform
// commands have no desktop equivalent; a device-specific adapter can add one.
void textRefresh(QWidget *) { }

void pageRefresh(QWidget *, bool, bool) { }

void setReaderRefreshWindow(QWidget *) { }
} // namespace kobalt::platform

namespace kobalt::platform {
// macOS provides loopback. Never attempt Kobo's privileged interface repair.
bool configureLoopback(int *error)
{
    if (error)
        *error = ENOTSUP;
    return false;
}
} // namespace kobalt::platform
