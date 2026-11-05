#pragma once

#include "app/reader_window.h"

#include <QVector>

#include <functional>
#include <utility>

namespace kobalt::nickel {
// The coordinator owns completion until its shared drain finishes. Revocation
// or an ordinary close can destroy the window before its save callback runs.
inline void prepareUsbTransition(ReaderWindow *reader,
                                 QVector<std::function<void()>> &cleanupCallbacks,
                                 std::function<void()> release, std::function<void()> finished)
{
    cleanupCallbacks.append(std::move(finished));
    if (reader) {
        reader->prepareUsb(std::move(release));
    } else {
        release();
    }
}
} // namespace kobalt::nickel
