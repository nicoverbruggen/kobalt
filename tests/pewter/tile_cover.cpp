#include "platforms/kobo/common/native_tile_cover.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QPointer>
#include <QWidget>
#include <cstdio>
#include <cstdlib>

// The probe uses exported accessors, so these synthetic native types need no
// firmware layout. The SDK supplies the same Qt image and widget ABI.
class Content
{
public:
    QString book, image;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QString getId() const;
#else
    QString getId(const QString &) const;
#endif
    QString getImageId() const;
};
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
QString Content::getId() const
{
    return book;
}
#else
QString Content::getId(const QString &) const
{
    return book;
}
#endif
QString Content::getImageId() const
{
    return image;
}

class Volume : public Content
{
};

class Image
{
public:
    static bool isDefaultCoverImage(const QString &id);
};

bool Image::isDefaultCoverImage(const QString &id)
{
    return id == "default";
}

class VolumePixmapView final : public QWidget
{
    Q_OBJECT
public:
    Volume volume;
    const Volume &getVolume() const;
};

const Volume &VolumePixmapView::getVolume() const
{
    return volume;
}

namespace kobalt {
extern "C" void kobalt_tile_set_image(QWidget *, const QImage &, const QString &);
extern "C" void kobalt_tile_set_content(QWidget *, const void *, const QString &);
extern "C" void kobalt_tile_image_loaded(QWidget *, const QString &);
extern "C" void kobalt_tile_default_cover(QWidget *);
} // namespace kobalt

class DeleteCaptureFilter final : public QObject
{
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::DynamicPropertyChange
            && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()
                    == "_kobaltTileAssignment") {
            delete watched;
            return true;
        }
        return false;
    }
};

using namespace kobalt;

static void require(bool condition, const char *description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", description);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    nativeTileSetImage = [](QWidget *, const QImage &, const QString &) { };
    nativeTileSetContent = [](QWidget *widget, const void *volume, const QString &) {
        static_cast<VolumePixmapView *>(widget)->volume = *static_cast<const Volume *>(volume);
    };
    nativeTileImageLoaded = [](QWidget *, const QString &) { };
    nativeTileDefaultCover = [](QWidget *widget) {
        QImage generated(40, 60, QImage::Format_RGB32);
        generated.fill(Qt::gray);
        kobalt_tile_set_image(widget, generated, "generated");
    };
    initializeNativeTileCoverProbe();
    require(nativeTileCoverProbeEnabled(), "all hooks enable the probe");
    Volume book;
    book.book = "edition-a";
    book.image = "cover-a";
    Volume other = book;
    other.book = "edition-b";
    QImage cover(80, 120, QImage::Format_RGB32);
    cover.fill(Qt::blue);
    QImage retained;
    {
        VolumePixmapView tile;
        kobalt_tile_set_content(&tile, &book, "thumbnail");
        kobalt_tile_set_image(&tile, cover, "cover-a");
        tile.show();
        app.processEvents();
        retained = nativeTileCoverProbeImage(&book);
        require(retained.cacheKey() == cover.cacheKey(), "snapshot shares the assigned image");
        require(nativeTileCoverProbeImage(&other).isNull(), "different edition rejected");
        tile.hide();
        require(nativeTileCoverProbeImage(&book).isNull(), "hidden tile rejected");
        tile.show();
        app.processEvents();
        kobalt_tile_image_loaded(&tile, "another-edition");
        require(!nativeTileCoverProbeImage(&book).isNull(), "unrelated invalidation ignored");
        kobalt_tile_image_loaded(&tile, "edition-a");
        require(nativeTileCoverProbeImage(&book).isNull(), "native invalidation clears capture");
        kobalt_tile_set_image(&tile, cover, "cover-a");
        tile.volume.image = "new-cover";
        require(nativeTileCoverProbeImage(&book).isNull(), "changed image identity rejected");
        tile.volume = book;
        kobalt_tile_set_content(&tile, &book, "thumbnail");
        require(!nativeTileCoverProbeImage(&book).isNull(), "unchanged content keeps capture");
        kobalt_tile_set_content(&tile, &book, "another-size");
        require(nativeTileCoverProbeImage(&book).isNull(), "image type change clears capture");
        kobalt_tile_set_image(&tile, cover, "cover-a");
        kobalt_tile_default_cover(&tile);
        require(nativeTileCoverProbeImage(&book).isNull(), "generated placeholder rejected");
        tile.volume.image = "default";
        kobalt_tile_set_image(&tile, cover, "default");
        require(nativeTileCoverProbeImage(&tile.volume).isNull(), "default image ID rejected");
        tile.volume = book;
        kobalt_tile_set_image(&tile, cover, "cover-a");
        {
            VolumePixmapView duplicate;
            kobalt_tile_set_content(&duplicate, &book, "thumbnail");
            kobalt_tile_set_image(&duplicate, cover, "cover-a");
            duplicate.show();
            app.processEvents();
            require(nativeTileCoverProbeImage(&book).isNull(), "ambiguous visible tiles rejected");
        }
        require(!nativeTileCoverProbeImage(&book).isNull(),
                "destroyed duplicate no longer ambiguous");
        kobalt_tile_set_image(&tile, QImage(), "cover-a");
        require(nativeTileCoverProbeImage(&book).isNull(), "null assignment clears capture");
    }
    require(retained.pixel(0, 0) == qRgb(0, 0, 255), "snapshot survives tile destruction");
    {
        VolumePixmapView tile;
        kobalt_tile_set_content(&tile, &book, "thumbnail");
        tile.show();
        app.processEvents();
        QImage newer(160, 240, QImage::Format_RGB32);
        newer.fill(Qt::green);
        static const QImage *replacement = nullptr;
        static bool reentered = false;
        replacement = &newer;
        nativeTileSetImage = [](QWidget *widget, const QImage &, const QString &) {
            if (!reentered) {
                reentered = true;
                kobalt_tile_set_image(widget, *replacement, "cover-a");
            }
        };
        kobalt_tile_set_image(&tile, cover, "cover-a");
        require(nativeTileCoverProbeImage(&book).cacheKey() == newer.cacheKey(),
                "reentrant image assignment must keep the newer image");
        nativeTileSetImage = [](QWidget *, const QImage &, const QString &) { };
    }
    {
        VolumePixmapView tile;
        kobalt_tile_set_content(&tile, &book, "thumbnail");
        tile.show();
        app.processEvents();
        int depth = 0;
        static int *activeDepth = nullptr;
        activeDepth = &depth;
        nativeTileDefaultCover = [](QWidget *widget) {
            if ((*activeDepth)++ == 0) {
                kobalt_tile_default_cover(widget);
                QImage generated(40, 60, QImage::Format_RGB32);
                generated.fill(Qt::gray);
                kobalt_tile_set_image(widget, generated, "generated");
            }
            --*activeDepth;
        };
        kobalt_tile_default_cover(&tile);
        require(nativeTileCoverProbeImage(&book).isNull(),
                "nested default generation must not admit a placeholder");
    }
    {
        nativeTileDefaultCover = [](QWidget *widget) { delete widget; };
        auto *tile = new VolumePixmapView;
        kobalt_tile_default_cover(tile);
    }
    {
        nativeTileSetImage = [](QWidget *widget, const QImage &, const QString &) {
            delete widget;
        };
        auto *tile = new VolumePixmapView;
        kobalt_tile_set_image(tile, cover, "cover-a");
    }
    {
        VolumePixmapView tile;
        kobalt_tile_set_content(&tile, &book, "thumbnail");
        tile.show();
        app.processEvents();
        nativeTileSetImage = [](QWidget *widget, const QImage &, const QString &) {
            kobalt_tile_image_loaded(widget, "edition-a");
        };
        kobalt_tile_set_image(&tile, cover, "cover-a");
        require(nativeTileCoverProbeImage(&book).isNull(),
                "invalidation during assignment must not restore an old capture");
    }
    {
        nativeTileSetImage = [](QWidget *, const QImage &, const QString &) { };
        DeleteCaptureFilter filter;
        QPointer<VolumePixmapView> tile = new VolumePixmapView;
        tile->installEventFilter(&filter);
        kobalt_tile_set_image(tile, cover, "cover-a");
        require(tile.isNull(), "capture tolerates deletion by a property event filter");
    }
    nativeTileSetContent = nullptr;
    initializeNativeTileCoverProbe();
    require(!nativeTileCoverProbeEnabled(), "missing invalidation hook disables the probe");
    std::puts("PASS tile cover ownership, identity, invalidation and fallback");
    return 0;
}

#include "tile_cover.moc"
