#pragma once
#include <QRegion>
#include <QTextEdit>
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

inline QRegion annotationRepairDamage(const QWidget *window, const QRegion &damage, bool turning)
{
    auto repairs = damage & annotationRepairRegions(window);
    if (repairs.isEmpty() || turning)
        return repairs;

    // The note stays above the page after Done closes the keyboard.
    // Keep pending cleanup until the opaque note viewport moves or closes.
    for (const auto *editor : window->findChildren<QTextEdit *>(QStringLiteral("rrNoteText"))) {
        const auto *viewport = editor->viewport();
        if (viewport->isVisible())
            repairs -= QRect(viewport->mapTo(window, QPoint()), viewport->size());
    }
    return repairs;
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
