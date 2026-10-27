#include "platform/api/kobo_refresh.h"
#include <QApplication>
#include <QWidget>
#include <QWindow>
#include <cerrno>

namespace kobalt::platform {
void textRefresh(QWidget *) {}
void pageRefresh(QWidget *, bool clean, bool) {
    if (!clean)
        return;
    // The visible canvas owns the native window; reader widgets are embedded
    // in a graphics proxy. The Kindle QPA consumes this at its next flush.
    for (auto *surface : QApplication::topLevelWidgets()) {
        if (surface->objectName() == QStringLiteral("kindleWindow") && surface->windowHandle()) {
            surface->windowHandle()->setProperty("kobalt.fullRefresh", true);
            surface->update();
            break;
        }
    }
}
void setReaderRefreshWindow(QWidget *) {}
}
namespace kobalt::nickel4 {
bool configureLoopback(int *error) {
    if (error)
        *error = ENOTSUP;
    return false;
}
}
