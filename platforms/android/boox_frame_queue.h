#pragma once
#include "platforms/android/refresh.h"

namespace kobalt::android {
// This firmware merges all entries with the same mode into one rectangle.
// Give each region its own buffer instead of letting distant strips join.
// On the tested BOOX firmware, even a disjoint GU header and REGAL body in
// one buffer become a whole-screen driver update.
class BooxFrameQueue
{
public:
    void reset()
    {
        ui = {};
        text = {};
        clean = false;
    }

    bool pending() const { return clean || !ui.isEmpty() || !text.isEmpty(); }

    QRect nextArea() const
    {
        if (!text.isEmpty()) {
            return text.boundingRect();
        }
        QRect largest;
        for (const auto &area : ui) {
            if (qint64(area.width()) * area.height() > qint64(largest.width()) * largest.height()) {
                largest = area;
            }
        }
        return largest;
    }

    template <class Submit>
    bool prepare(const RefreshFrame &frame, Submit submit)
    {
        // Newer pixels replace the policy of overlapping pending pixels.
        ui = (ui - frame.screenPixels) | (frame.screenPixels - frame.textPixels);
        text = (text - frame.screenPixels) | frame.textPixels;
        clean = clean || frame.clean;
        return present(submit);
    }

    template <class Submit>
    bool present(Submit submit)
    {
        if (!pending()) {
            return false;
        }
        RefreshFrame frame;
        frame.clean = clean;
        frame.screenPixels = clean ? ui | text : QRegion(nextArea());
        if (!text.isEmpty() && !clean) {
            frame.text = true;
            frame.textPixels = frame.screenPixels;
        }
        if (!submit(frame)) {
            return false;
        }
        if (clean) {
            reset();
        } else {
            ui -= frame.screenPixels;
            text -= frame.screenPixels;
        }
        return true;
    }

private:
    QRegion ui, text;
    bool clean = false;
};
} // namespace kobalt::android
