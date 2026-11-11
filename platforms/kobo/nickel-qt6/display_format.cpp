#include "display_format.h"
#include "platforms/kobo/display/image_format.h"
#include "platforms/kobo/display/framebuffer_driver.h"
#include "support/diagnostics.h"
#include <QApplication>
#include <QBackingStore>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <QWindow>
#include <qpa/qplatformscreen.h>
#include <dlfcn.h>

extern "C" void *nh_dlhook(void *, const char *, void *);

namespace kobalt {
namespace {
using SizeConstructor = void (*)(QImage *, const QSize &, QImage::Format);
using Resize = void (*)(QBackingStore *, const QSize &);
using BeginPaint = void (*)(QBackingStore *, const QRegion &);
SizeConstructor nextSize = nullptr;
Resize nextResize = nullptr;
BeginPaint nextBeginPaint = nullptr;
QPointer<QWidget> reader;
bool installed = false;
bool reported = false;
unsigned allocations = 0;
thread_local bool readerAllocation = false;

bool ownsStore(QBackingStore *store)
{
    return qApp && QThread::currentThread() == qApp->thread() && reader && store
            && reader->windowHandle() && store->window() == reader->windowHandle();
}

void sizeConstructor(QImage *image, const QSize &size, QImage::Format format)
{
    const bool change = readerAllocation && needsPremultipliedDisplay(format);
    nextSize(image, size, change ? QImage::Format_RGBA8888_Premultiplied : format);
    if (change && !image->isNull() && image->format() == QImage::Format_RGBA8888_Premultiplied) {
        // Initialize the fresh allocation so a first partial paint cannot expose
        // uninitialized transparent pixels outside its dirty region.
        image->fill(Qt::transparent);
        ++allocations;
    }
}

void resize(QBackingStore *store, const QSize &size)
{
    DisplayAllocationScope scope(readerAllocation, ownsStore(store) && installed);
    nextResize(store, size);
}

void beginPaint(QBackingStore *store, const QRegion &region)
{
    // Qt can resize here after a device-pixel-ratio change. Keep the scope around
    // the native call so its HiDPI wrapper is built from the new image format.
    DisplayAllocationScope scope(readerAllocation, ownsStore(store) && installed);
    nextBeginPaint(store, region);
}
} // namespace

void setDisplayFormatWindow(QWidget *widget)
{
    reader = widget;
    reported = false;
    allocations = 0;
}

void installDisplayFormatHook()
{
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;
    auto *screen = QGuiApplication::primaryScreen();
    if (!screen || !screen->handle())
        return;
    const auto format = screen->handle()->format();
    if (!needsPremultipliedDisplay(format) || !mtkFramebuffer(framebufferDriver())) {
        diagnostic("display-format-hook",
                   { { "status", "not-needed-or-unsupported" }, { "format", int(format) } });
        return;
    }
    if (qEnvironmentVariableIsSet("KOBALT_DISABLE_DISPLAY_FORMAT_HOOK")) {
        diagnostic("display-format-hook", { { "status", "disabled" } });
        return;
    }
    if (!displayFormatPaintCheck()) {
        diagnostic("display-format-hook", { { "status", "paint-check-failed" } });
        return;
    }
    void *backend = dlopen("/usr/local/Kobo/platforms/libkobo.so", RTLD_LAZY | RTLD_NOLOAD);
    void *widgets = dlopen("libQt6Widgets.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (backend && widgets) {
        // Only fresh allocations during this reader's native resize/paint calls
        // are eligible. Never hook the constructor that wraps framebuffer bytes.
        nextSize = reinterpret_cast<SizeConstructor>(
                nh_dlhook(backend, "_ZN6QImageC1ERK5QSizeNS_6FormatE",
                          reinterpret_cast<void *>(sizeConstructor)));
        nextResize = reinterpret_cast<Resize>(nh_dlhook(
                widgets, "_ZN13QBackingStore6resizeERK5QSize", reinterpret_cast<void *>(resize)));
        nextBeginPaint = reinterpret_cast<BeginPaint>(
                nh_dlhook(widgets, "_ZN13QBackingStore10beginPaintERK7QRegion",
                          reinterpret_cast<void *>(beginPaint)));
    }
    if (backend)
        dlclose(backend);
    if (widgets)
        dlclose(widgets);
    installed = nextSize && nextResize && nextBeginPaint;
    diagnostic("display-format-hook",
               { { "status", installed ? "installed" : "unavailable" },
                 { "scope", "reader-window" },
                 { "format", int(format) } });
}

void verifyDisplayFormat(QBackingStore *store)
{
    if (!ownsStore(store) || !installed)
        return;
    auto *device = store->paintDevice();
    if (!device || device->devType() != QInternal::Image)
        return;
    const auto format = static_cast<QImage *>(device)->format();
    if (reported && format == QImage::Format_RGBA8888_Premultiplied)
        return;
    const bool verified = allocations && format == QImage::Format_RGBA8888_Premultiplied;
    reported = true;
    // A missed initial allocation is not repaired by relabelling existing pixels.
    // Leave that window in the native format and stop future substitutions.
    if (!verified)
        installed = false;
    diagnostic("display-format-verified",
               { { "verified", verified },
                 { "scope", "reader-window" },
                 { "format", int(format) },
                 { "allocated", int(allocations) } });
}
} // namespace kobalt
