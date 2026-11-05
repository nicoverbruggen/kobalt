#pragma once
#include "platform/api/reader_host_services.h"
#include "support/strings.h"
#include <QJsonObject>
#include <QStringList>
#include <QVector>

namespace kobalt {
// What the reader tells the user when a benchmark run ends. A run fails when
// a scenario failed, nothing ran or the results could not be saved. `folder`
// holds the results, as the user finds it on a computer.
inline ReaderNotice benchmarkNotice(const QVector<QJsonObject> &results, bool saved, const QString &folder) {
    int completed = 0, skipped = 0, failed = 0;
    QString firstFailure, reason;
    for (const auto &result : results) {
        const auto status = result.value("status").toString();
        if (status == QLatin1String("completed")) {
            ++completed;
        } else if (status == QLatin1String("skipped")) {
            ++skipped;
        } else if (failed++ == 0) {
            firstFailure = result.value("id").toString();
            reason = result.value("reason").toString();
        }
    }
    QStringList parts;
    if (!saved)
        parts.append(strings::get("benchmark.not_saved"));
    else if (results.isEmpty())
        parts.append(strings::get("benchmark.no_scenarios"));
    else
        parts.append(strings::get("benchmark.counts").arg(completed).arg(skipped).arg(failed));
    if (failed && reason.isEmpty())
        parts.append(strings::get("benchmark.first_failure").arg(firstFailure));
    else if (failed)
        parts.append(strings::get("benchmark.first_failure_reason").arg(firstFailure, reason));
    parts.append(strings::get("benchmark.connect").arg(folder));
    const bool succeeded = saved && !results.isEmpty() && !failed;
    return {succeeded ? strings::get("benchmark.finished") : strings::get("benchmark.failed"),
            parts.join(QStringLiteral("\n\n")), QStringLiteral("rrBenchmarkNotice")};
}
}
