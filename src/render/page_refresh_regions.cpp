#include "page_refresh_regions.h"
#include "page_content.h"

namespace kobalt {

bool pageImageAtPoint(const QImage &image, QPoint point)
{
    const auto regions = pageContent(image).images;
    if (image.isNull() || !regions.known)
        return false;
    const quint64 area = quint64(image.width()) * image.height();
    const int minimumSide = qMin(image.width(), image.height()) * .08;
    for (const auto &rect : regions.rectangles)
        if (rect.contains(point) && rect.width() >= minimumSide && rect.height() >= minimumSide
            && quint64(rect.width()) * rect.height() * 20 >= area)
            return true;
    return false;
}

QRect pageSingleImage(const QImage &image)
{
    const auto content = pageContent(image);
    const auto &regions = content.images;
    if (!content.imageOnly || !regions.known || regions.rectangles.size() != 1
        || !pageImageAtPoint(image, regions.rectangles.first().center()))
        return {};
    return regions.rectangles.first();
}

QRegion pageRefreshRegions(const QImage &image, bool colorDevice)
{
    if (image.isNull())
        return {};
    const QRegion fullPage(QRect(QPoint(0, 0), image.size()));
    const auto content = pageContent(image);
    if (!content.images.known || (colorDevice && !content.colours.known))
        return content.textOnly ? QRegion() : fullPage;

    QRegion regions;
    const quint64 pageArea = quint64(image.width()) * quint64(image.height());
    const quint64 minimumImageArea = pageArea / 10 + (pageArea % 10 != 0);
    for (const QRect &rect : content.images.rectangles)
        if (quint64(rect.width()) * quint64(rect.height()) >= minimumImageArea)
            regions += rect;
    if (colorDevice)
        for (const QRect &rect : content.colours.rectangles)
            regions += rect;
    return regions;
}

} // namespace kobalt
