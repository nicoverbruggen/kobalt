#include "reader_timing.h"
#include "support/diagnostics.h"

namespace kobalt {
void ActionTiming::add(const char *stage, const QElapsedTimer &timer) {
    const auto key = QString::fromLatin1(stage) + "_ms";
    stages[key] = stages.value(key).toDouble() + timer.nsecsElapsed() / 1e6;
}
void reportBookClose(const ActionTiming &timing) {
    if (!timing.started.isValid() || !diagnosticsActive())
        return;
    auto fields = timing.stages;
    double sync = 0;
    for (const auto *stage : {"checkpoint_ms", "native_ms", "session_ms", "profile_ms"})
        sync += fields.value(QLatin1String(stage)).toDouble();
    fields["sync_ms"] = sync;
    fields["total_ms"] = timing.started.nsecsElapsed() / 1e6;
    diagnostic("book-close", fields, false);
}
}
