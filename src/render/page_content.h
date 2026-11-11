#pragma once

#include <QImage>
#include <QRect>
#include <QVector>

namespace kobalt {
// Missing or malformed regions are unknown. Known regions can be empty:
// the renderer inspected the page and found none of this kind.
// Coordinates returned by pageContent are clipped to the image bounds.
struct PageRegions
{
    bool known = false;
    QVector<QRect> rectangles;
};

struct PageContent
{
    PageRegions images, colours;
    bool textOnly = false, imageOnly = false;
};

PageContent pageContent(const QImage &image);
void setPageContent(QImage &image, const PageContent &content);
// Cheap predicates for animation and display policy, without parsing regions.
bool pageTextOnly(const QImage &image);
bool pageHasColour(const QImage &image);

// Stable identity of an extracted illustration, independent of page layout.
QString imageContentId(const QImage &image);
void setImageContentId(QImage &image, const QString &identity);
} // namespace kobalt
