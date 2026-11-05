#pragma once
#include <QRegion>
#include <QVariant>
#include <QWidget>

namespace kobalt {
inline QRegion annotationRepairRegions(const QWidget *window)
{
    return window->property("reader_annotation_repairs").value<QRegion>()
            | window->property("reader_palette_repairs").value<QRegion>()
            | window->property("reader_annotation_regions").value<QRegion>()
            | window->property("reader_palette_regions").value<QRegion>();
}

// Active color remains eligible for later damage. Outgoing color is repaired once.
inline void consumeAnnotationRepairs(QWidget *window, const QRegion &submitted)
{
    for (const auto *key : { "reader_annotation_repairs", "reader_palette_repairs" }) {
        window->setProperty(
                key, QVariant::fromValue(window->property(key).value<QRegion>() - submitted));
    }
}
} // namespace kobalt
