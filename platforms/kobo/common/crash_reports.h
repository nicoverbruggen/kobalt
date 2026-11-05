#pragma once
#include <QString>

namespace kobalt {
// The newest reports that installCrashReports keeps.
inline constexpr int crashReportLimit = 20;
// Installs handlers for fatal signals in the whole process. Each crash writes
// one report to directory. The previous handlers still run afterwards, so the
// host keeps its own crash handling. When mount is not empty, a report is
// written only while that path is a mount point. A crash while the storage is
// exported over USB then cannot write to the root file system. Call once, on
// the main thread. Only that thread gets an alternate signal stack, so a stack
// overflow in another thread is not reported.
void installCrashReports(const QString &directory, const QString &mount = {});
} // namespace kobalt
