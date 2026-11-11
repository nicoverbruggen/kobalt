#pragma once
#include <QJsonObject>
#include <functional>

namespace kobalt {
// Installed by the host before logging starts. Missing callbacks mean that
// the measurement is unavailable. Samples omit unavailable fields, never
// substitute zero. Startup and descriptor callbacks run on their caller;
// sample runs on the diagnostic worker. bootMilliseconds must be nonblocking.
// Providers must not call back into diagnostics or access GUI-owned objects.
struct DiagnosticSystem
{
    std::function<QJsonObject()> startup;
    std::function<QJsonObject(qint64)> sample;
    std::function<QJsonObject()> descriptors;
    std::function<qint64()> bootMilliseconds;
};

void setDiagnosticSystem(DiagnosticSystem system);
} // namespace kobalt
