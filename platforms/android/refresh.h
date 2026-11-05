#pragma once
#include <QRegion>
#include <QWidget>
#include <QVariant>

namespace kobalt::android {
struct RefreshFrame
{
    QRegion screenPixels;
    QRegion textPixels;
    bool text = false;
    bool clean = false;
};

inline RefreshFrame takeRefreshFrame(QWidget *reader, QWidget *viewport, const QRegion &damage)
{
    const auto ratio = viewport->devicePixelRatioF();
    RefreshFrame frame;
    for (const auto &area : damage) {
        const auto top = viewport->mapToGlobal(area.topLeft());
        frame.screenPixels +=
                QRectF(QPointF(top) * ratio, QSizeF(area.size()) * ratio).toAlignedRect();
    }
    frame.text = reader->property("boox_text_frame").toBool();
    frame.clean = reader->property("reader_full_flash").toBool();
    if (frame.text) {
        frame.textPixels = frame.screenPixels;
    }
    reader->setProperty("reader_full_flash", false);
    reader->setProperty("boox_text_frame", false);
    return frame;
}

// A missing or paused Android surface must not consume the pending request.
// prepare runs synchronously so metadata reaches the Surface before Qt flushes it.
template <class Prepare>
bool prepareRefreshFrame(QWidget *reader, QWidget *viewport, const QRegion &damage, Prepare prepare)
{
    const auto frame = takeRefreshFrame(reader, viewport, damage);
    if (prepare(frame))
        return true;
    if (frame.clean)
        reader->setProperty("reader_full_flash", true);
    if (frame.text)
        reader->setProperty("boox_text_frame", true);
    return false;
}

} // namespace kobalt::android
