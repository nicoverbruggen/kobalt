#pragma once
#include <QString>

namespace kobalt {
// Retained by the book even after native services are released.
struct NativeSyncPolicy
{
    bool enabled = false;
    QString descriptionKey;
};
} // namespace kobalt
