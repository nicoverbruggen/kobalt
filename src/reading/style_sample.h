#pragma once
#include "engine/crengine/engine_document.h"
#include <QImage>
#include <QJsonObject>
#include <QJsonValue>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class QObject;

namespace kobalt {
// Styling previews lay out a short bundled sample with the reader's current
// settings. One request varies one setting through its options; the others
// keep their current values, because page-break settings change each other's
// results.
struct StyleSampleRequest
{
    QJsonObject settings;
    QString key;
    QVector<QJsonValue> values;
    // The reading page size. The sample paginates exactly as a book would.
    QSize page;
    // Each page is scaled to fit this size after drawing.
    QSize thumbnail;
    // A full-resolution crop of the first page, from the start of its text,
    // for settings whose effect is too small to see in a scaled page. It ends
    // after the last whole line. Empty when the setting changes page breaks.
    QSize crop;
    QPoint cropOrigin;
    bool marginGuides = false;
    // Page spacing only: the reading area within the page, and the size of a
    // full-resolution view of its bottom edge on the page that changes.
    QRect readingArea;
    QSize detail;
    // Set when the result is no longer wanted. The render stops at its next
    // step, so a closed Settings panel does not delay the book behind it.
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct StyleSample
{
    // The spread shown, as the first of two page indices.
    int page = 0;
    // False when every option lays out and draws the sample the same way.
    bool differs = true;
    // One pair of scaled pages per option, in request order.
    QVector<QVector<QImage>> spreads;
    // One crop per option when the request asked for crops.
    QVector<QImage> crops;
    // Text edges in crop pixels, or an empty rectangle when no guides are needed.
    QRect cropReadingArea;
    // Extra paragraph gaps within each crop, excluding ordinary line leading.
    QVector<QVector<QRect>> cropGaps;
    // The reading area on a scaled page of the spread.
    QRectF readingArea;
    // One bottom-edge view per option when the request asked for one, with
    // the reading area and each option's added spare space in its pixels.
    // Spare space is height that the first option leaves to text.
    QVector<QImage> details;
    QRect detailArea;
    QVector<QVector<QRect>> spare;

    bool valid() const { return !spreads.isEmpty(); }
};

// Lays out the sample on the shared engine worker and calls `finished` on
// the GUI thread, unless `context` was destroyed first.
void renderStyleSample(EngineFontInputs fonts, StyleSampleRequest request, QObject *context,
                       std::function<void(StyleSample)> finished);
// Reading settings without the book-specific keys a sample cannot use.
QJsonObject styleSampleSettings(QJsonObject state);
// Page-break previews use one fixed page, font and size, so the sample shows
// a single line at a page edge and a gap whatever the reader's typography.
// Only the theme and the page-break choices come from `reading`.
QJsonObject styleSamplePageSettings(const QJsonObject &reading);
QSize styleSamplePageSize();
QRect styleSampleReadingArea();
} // namespace kobalt
