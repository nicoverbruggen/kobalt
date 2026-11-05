#include "page_refresh_regions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QtGlobal>
#include <cmath>
#include <limits>

namespace kobalt {
namespace {
constexpr int maxRectangles = 128;
constexpr int maxMetadataBytes = 65536;

bool integerCoordinate(const QJsonValue &value, int &coordinate)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < std::numeric_limits<int>::min()
        || number > std::numeric_limits<int>::max() || std::trunc(number) != number)
        return false;
    coordinate = static_cast<int>(number);
    return true;
}

bool parseRegions(const QImage &image, const char *key, QVector<QRect> &regions)
{
    const QString metadata = image.text(QLatin1String(key));
    if (metadata.isNull())
        return false;
    const QByteArray bytes = metadata.toUtf8();
    if (bytes.size() > maxMetadataBytes)
        return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
        return false;
    const QJsonArray array = document.array();
    if (array.size() > maxRectangles)
        return false;
    for (const auto &entry : array) {
        if (!entry.isArray())
            return false;
        const QJsonArray values = entry.toArray();
        if (values.size() != 4)
            return false;
        int x, y, width, height;
        if (!integerCoordinate(values[0], x) || !integerCoordinate(values[1], y)
            || !integerCoordinate(values[2], width) || !integerCoordinate(values[3], height)
            || width <= 0 || height <= 0)
            return false;
        const qint64 left = qMax<qint64>(0, x);
        const qint64 top = qMax<qint64>(0, y);
        const qint64 right = qMin<qint64>(image.width(), qint64(x) + width);
        const qint64 bottom = qMin<qint64>(image.height(), qint64(y) + height);
        if (right > left && bottom > top)
            regions.append(QRect(int(left), int(top), int(right - left), int(bottom - top)));
    }
    return true;
}
} // namespace

bool pageImageAtPoint(const QImage &image, QPoint point)
{
    QVector<QRect> regions;
    if (image.isNull() || !parseRegions(image, "kobalt.image_regions", regions))
        return false;
    const quint64 area = quint64(image.width()) * image.height();
    const int minimumSide = qMin(image.width(), image.height()) * .08;
    for (const auto &rect : regions)
        if (rect.contains(point) && rect.width() >= minimumSide && rect.height() >= minimumSide
            && quint64(rect.width()) * rect.height() * 20 >= area)
            return true;
    return false;
}

QRect pageSingleImage(const QImage &image)
{
    QVector<QRect> regions;
    if (image.text("kobalt.image_only") != "true"
        || !parseRegions(image, "kobalt.image_regions", regions) || regions.size() != 1
        || !pageImageAtPoint(image, regions.first().center()))
        return {};
    return regions.first();
}

QRegion pageRefreshRegions(const QImage &image, bool colorDevice)
{
    if (image.isNull())
        return {};
    const QRegion fullPage(QRect(QPoint(0, 0), image.size()));
    QVector<QRect> imageRects, colorRects;
    if (!parseRegions(image, "kobalt.image_regions", imageRects)
        || (colorDevice && !parseRegions(image, "kobalt.colour_regions", colorRects)))
        return image.text("kobalt.animation_text_only") == "true" ? QRegion() : fullPage;

    QRegion regions;
    const quint64 pageArea = quint64(image.width()) * quint64(image.height());
    const quint64 minimumImageArea = pageArea / 10 + (pageArea % 10 != 0);
    for (const QRect &rect : imageRects)
        if (quint64(rect.width()) * quint64(rect.height()) >= minimumImageArea)
            regions += rect;
    if (colorDevice)
        for (const QRect &rect : colorRects)
            regions += rect;
    return regions;
}

} // namespace kobalt
