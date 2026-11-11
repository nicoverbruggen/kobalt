#pragma once
#include "platform/api/diagnostic_system.h"
#include <QString>

namespace kobalt::platform {
using DescriptorName = std::function<QString(const QString &)>;
// System paths keep their names. Other files are grouped by extension.
QString diagnosticDescriptorName(const QString &path);
DiagnosticSystem processDiagnosticSystem(DescriptorName name = {});
} // namespace kobalt::platform
