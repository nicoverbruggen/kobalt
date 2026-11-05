#include "platform/api/kobo_refresh.h"
#include "platform/api/loopback.h"
#include <cerrno>
#include <QWidget>
#include <QVariant>

namespace kobalt::platform {
// The canvas attaches these requests to the Surface before painting and flushing.
void textRefresh(QWidget *) { }

void pageRefresh(QWidget *widget, bool clean, bool partialText)
{
    if (!widget)
        return;
    // Reader maintenance runs again before the proxy's queued canvas paint.
    // A call without a text request must not cancel that pending frame's request.
    // prepareRefreshFrame consumes it when the Surface accepts the metadata.
    if (partialText && widget->palette().color(QPalette::Window).lightness() > 127)
        widget->window()->setProperty("boox_text_frame", true);
    if (clean) {
        widget->window()->setProperty("reader_full_flash", true);
        widget->update();
    }
}

void setReaderRefreshWindow(QWidget *) { }
} // namespace kobalt::platform

namespace kobalt::platform {
// Android provides loopback. Never attempt Kobo's privileged interface repair.
bool configureLoopback(int *error)
{
    if (error)
        *error = ENOTSUP;
    return false;
}
} // namespace kobalt::platform
