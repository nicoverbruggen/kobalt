#include "native_progress.h"
#include "platforms/kobo/common/native_lifecycle.h"
#include "storage/record_writer.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include <QCoreApplication>

namespace kobalt {
QVariantMap NativeProgress::persisted(QString *error) const
{
    const auto query = rowQuery(false, error);
    if (!query)
        return {};
    const auto result = query();
    if (result.isEmpty() && error)
        *error = strings::get("error.cannot_verify_persisted_nickel_progress");
    return result;
}

ReadingProgress NativeProgress::read(QString *error) const
{
    invalidatePoll();
    return decode(persisted(error), error);
}

bool NativeProgress::poll(bool requested, ReadingProgress &result)
{
    const auto storageGeneration = NativeLifecycle::stockStorageGeneration();
    if (!storageGeneration || transitioning)
        return false;
    if (pollReady) {
        pollReady = false;
        if (pollStorage == storageGeneration) {
            result = polled;
            return true;
        }
        requested = true;
    }
    if (!requested || pollPending)
        return false;

    // Resolve firmware handles on Nickel's thread. The worker receives only the
    // database path, connection name and the values it needs for its query.
    const auto query = rowQuery(true, nullptr);
    if (query) {
        static storage::RecordWriter polls(storage::RecordWriter::Delivery::All);
        const QPointer<NativeProgress> guard(this);
        const auto generation = pollGeneration;
        pollPending = polls.tryPost([guard, generation, storageGeneration, query] {
            QVariantMap row;
            if (NativeLifecycle::stockStorageGeneration() == storageGeneration) {
                mainthread::BlockingOperation operation("nickel-progress-poll");
                row = query();
            }
            qt::invoke(
                    QCoreApplication::instance(),
                    [guard, generation, storageGeneration, row] {
                        if (!guard)
                            return;
                        guard->pollPending = false;
                        if (guard->pollGeneration == generation && !guard->transitioning
                            && NativeLifecycle::stockStorageGeneration() == storageGeneration) {
                            guard->polled = guard->decode(row, nullptr);
                            guard->pollReady = true;
                            guard->pollStorage = storageGeneration;
                        } else {
                            guard->changed = true;
                        }
                        if (guard->changeAvailable)
                            guard->changeAvailable();
                    },
                    Qt::QueuedConnection);
            return true;
        });
        if (pollPending)
            return false;
    }
    result = decode({}, nullptr);
    return true;
}
} // namespace kobalt
