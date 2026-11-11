#include "page_content.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>
#include <limits>
#include <utility>

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

PageRegions parseRegions(const QImage &image, const char *key)
{
    QVector<QRect> regions;
    const QString metadata = image.text(QLatin1String(key));
    if (metadata.isNull())
        return {};
    const QByteArray bytes = metadata.toUtf8();
    if (bytes.size() > maxMetadataBytes)
        return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
        return {};
    const QJsonArray array = document.array();
    if (array.size() > maxRectangles)
        return {};
    for (const auto &entry : array) {
        if (!entry.isArray())
            return {};
        const QJsonArray values = entry.toArray();
        if (values.size() != 4)
            return {};
        int x, y, width, height;
        if (!integerCoordinate(values[0], x) || !integerCoordinate(values[1], y)
            || !integerCoordinate(values[2], width) || !integerCoordinate(values[3], height)
            || width <= 0 || height <= 0)
            return {};
        const qint64 left = qMax<qint64>(0, x);
        const qint64 top = qMax<qint64>(0, y);
        const qint64 right = qMin<qint64>(image.width(), qint64(x) + width);
        const qint64 bottom = qMin<qint64>(image.height(), qint64(y) + height);
        if (right > left && bottom > top)
            regions.append(QRect(int(left), int(top), int(right - left), int(bottom - top)));
    }
    return { true, std::move(regions) };
}

QString regionMetadata(const QVector<QRect> &regions)
{
    QJsonArray array;
    for (const QRect &rect : regions) {
        QJsonArray values;
        values.append(rect.x());
        values.append(rect.y());
        values.append(rect.width());
        values.append(rect.height());
        array.append(values);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

} // namespace

PageContent pageContent(const QImage &image)
{
    return { parseRegions(image, "kobalt.image_regions"),
             parseRegions(image, "kobalt.colour_regions"), pageTextOnly(image),
             image.text("kobalt.image_only") == "true" };
}

void setPageContent(QImage &image, const PageContent &content)
{
    image.setText("kobalt.image_regions",
                  content.images.known ? regionMetadata(content.images.rectangles) : QString());
    image.setText("kobalt.colour_regions",
                  content.colours.known ? regionMetadata(content.colours.rectangles) : QString());
    image.setText("kobalt.animation_text_only", content.textOnly ? "true" : "false");
    image.setText("kobalt.image_only", content.imageOnly ? "true" : "false");
}

bool pageTextOnly(const QImage &image)
{
    return image.text("kobalt.animation_text_only") == "true";
}

bool pageHasColour(const QImage &image)
{
    // Retain the display hint's conservative treatment of an unrecognized
    // nonempty colour record. Region-based refreshes validate it separately.
    const auto regions = image.text("kobalt.colour_regions");
    return !regions.isEmpty() && regions != "[]" && !pageTextOnly(image);
}

QString imageContentId(const QImage &image)
{
    return image.text("kobalt.image_id");
}

void setImageContentId(QImage &image, const QString &identity)
{
    image.setText("kobalt.image_id", identity);
}
} // namespace kobalt
