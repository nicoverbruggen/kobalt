#include "native_tile_cover.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QApplication>
#include <QPointer>
#include <QThread>
#include <QVariant>
#include <QWidget>
#include <dlfcn.h>

namespace kobalt {
void (*nativeTileSetImage)(QWidget *, const QImage &, const QString &);
void (*nativeTileSetContent)(QWidget *, const void *, const QString &);
void (*nativeTileImageLoaded)(QWidget *, const QString &);
void (*nativeTileDefaultCover)(QWidget *);

namespace {
bool &enabled()
{
    static bool value = false;
    return value;
}

template <class T>
T symbol(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

QString identity(const void *volume)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    static const auto get = symbol<QString (*)(const void *)>("_ZNK7Content5getIdEv");
    return get ? get(volume) : QString();
#else
    static const auto get =
            symbol<QString (*)(const void *, const QString &)>("_ZNK7Content5getIdERK7QString");
    return get ? get(volume, QString()) : QString();
#endif
}

QString imageIdentity(const void *volume)
{
    static const auto get = symbol<QString (*)(const void *)>("_ZNK7Content10getImageIdEv");
    return get ? get(volume) : QString();
}

const void *tileVolume(QWidget *widget)
{
    static const auto get =
            symbol<const void *(*)(const void *)>("_ZNK16VolumePixmapView9getVolumeEv");
    return get && widget->inherits("VolumePixmapView") ? get(widget) : nullptr;
}

bool guiThread()
{
    return qApp && QThread::currentThread() == qApp->thread();
}

bool clear(QWidget *widget)
{
    QPointer<QWidget> alive(widget);
    const auto assignment = widget->property("_kobaltTileAssignment").toULongLong() + 1;
    widget->setProperty("_kobaltTileAssignment", QVariant::fromValue<qulonglong>(assignment));
    if (!alive)
        return false;
    widget->setProperty("_kobaltTileImage", QVariant());
    if (!alive)
        return false;
    widget->setProperty("_kobaltTileBook", QVariant());
    if (!alive)
        return false;
    widget->setProperty("_kobaltTileImageId", QVariant());
    return !alive.isNull();
}
} // namespace

void initializeNativeTileCoverProbe()
{
    enabled() = nativeTileSetImage && nativeTileSetContent && nativeTileImageLoaded
            && nativeTileDefaultCover;
    diagnostic("tile-cover-probe-ready",
               qt::object({ { "enabled", enabled() },
                            { "set_image_hook", bool(nativeTileSetImage) },
                            { "set_content_hook", bool(nativeTileSetContent) },
                            { "image_loaded_hook", bool(nativeTileImageLoaded) },
                            { "default_cover_hook", bool(nativeTileDefaultCover) } }));
}

bool nativeTileCoverProbeEnabled()
{
    return enabled();
}

QImage nativeTileCoverProbeImage(const void *volume)
{
    if (!enabled() || !guiThread() || !volume)
        return {};
    const auto book = identity(volume), imageId = imageIdentity(volume);
    QImage result;
    int matches = 0;
    for (auto *widget : QApplication::allWidgets()) {
        if (!widget->isVisible() || !widget->inherits("VolumePixmapView"))
            continue;
        const auto *current = tileVolume(widget);
        if (!current || identity(current) != book || imageIdentity(current) != imageId
            || widget->property("_kobaltTileBook").toString() != book
            || widget->property("_kobaltTileImageId").toString() != imageId)
            continue;
        const auto image = widget->property("_kobaltTileImage").value<QImage>();
        if (image.isNull())
            continue;
        ++matches;
        result = image;
    }
    // The probe must not guess which widget supplied a duplicated book tile.
    if (matches != 1)
        result = {};
    diagnostic("tile-cover-probe-snapshot",
               qt::object({ { "matching_tiles", matches },
                            { "width", result.width() },
                            { "height", result.height() },
                            { "image_key", QString::number(result.cacheKey()) } }));
    return result;
}

extern "C" __attribute__((visibility("default"))) void
kobalt_tile_set_image(QWidget *widget, const QImage &image, const QString &tag)
{
    if (!enabled() || !guiThread()) {
        nativeTileSetImage(widget, image, tag);
        return;
    }
    QPointer<QWidget> alive(widget);
    if (!clear(widget))
        return;
    const auto assignment = widget->property("_kobaltTileAssignment").toULongLong();
    nativeTileSetImage(widget, image, tag);
    if (!alive || widget->property("_kobaltTileAssignment").toULongLong() != assignment)
        return;
    const auto *volume = tileVolume(widget);
    if (!volume)
        return;
    const auto book = identity(volume), imageId = imageIdentity(volume);
    static const auto isDefault =
            symbol<bool (*)(const QString &)>("_ZN5Image19isDefaultCoverImageERK7QString");
    const bool usable = !image.isNull() && !book.isEmpty() && !imageId.isEmpty() && isDefault
            && !isDefault(imageId) && !widget->property("_kobaltTileDefault").toBool();
    if (usable) {
        widget->setProperty("_kobaltTileBook", book);
        if (!alive)
            return;
        widget->setProperty("_kobaltTileImageId", imageId);
        if (!alive)
            return;
        widget->setProperty("_kobaltTileImage", image);
        if (!alive)
            return;
    }
    diagnostic("tile-cover-probe-assigned",
               qt::object({ { "class", QString::fromLatin1(widget->metaObject()->className()) },
                            { "usable", usable },
                            { "width", image.width() },
                            { "height", image.height() },
                            { "image_key", QString::number(image.cacheKey()) } }));
}

extern "C" __attribute__((visibility("default"))) void
kobalt_tile_set_content(QWidget *widget, const void *volume, const QString &type)
{
    QPointer<QWidget> alive(widget);
    const bool probing = enabled() && guiThread();
    if (probing) {
        const auto *current = tileVolume(widget);
        if (!current || identity(current) != identity(volume)
            || imageIdentity(current) != imageIdentity(volume)
            || widget->property("_kobaltTileType").toString() != type) {
            if (!clear(widget))
                return;
        }
        widget->setProperty("_kobaltTileType", type);
    }
    if (!alive)
        return;
    nativeTileSetContent(widget, volume, type);
}

extern "C" __attribute__((visibility("default"))) void kobalt_tile_image_loaded(QWidget *widget,
                                                                                const QString &id)
{
    if (enabled() && guiThread()) {
        const auto *volume = tileVolume(widget);
        if (volume && identity(volume) == id && !clear(widget))
            return;
    }
    nativeTileImageLoaded(widget, id);
}

extern "C" __attribute__((visibility("default"))) void kobalt_tile_default_cover(QWidget *widget)
{
    QPointer<QWidget> alive(widget);
    const bool probing = enabled() && guiThread();
    const bool previousDefault = probing && widget->property("_kobaltTileDefault").toBool();
    if (probing) {
        if (!clear(widget))
            return;
        widget->setProperty("_kobaltTileDefault", true);
    }
    if (!alive)
        return;
    nativeTileDefaultCover(widget);
    if (probing && alive)
        widget->setProperty("_kobaltTileDefault", previousDefault);
}
} // namespace kobalt
