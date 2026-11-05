#pragma once
#include "resource_cache.h"
#include <QMap>

namespace kobalt {
// Checks declarations the engine cannot honor, without resolving the CSS cascade.
struct LayoutCompatibility
{
    enum Verdict { Supported, VerticalWriting, RightToLeftProgression, Incomplete };

    Verdict verdict = Incomplete;
    int resources = 0;
    qint64 bytes = 0;
    // A deadline or resource failure cannot establish a stable book verdict.
    bool transientFailure = false;
    // The first issue found by the advisory inspector. Excerpts are bounded to
    // 240 characters and refer only to publication resources, not host files.
    QString issue = {}, resource = {}, excerpt = {};
    qint64 line = 0, column = 0;
    const char *reasonKey() const;
    QString reason() const;
};

LayoutCompatibility inspectLayout(ResourceCache &resources, const QMap<QString, qint64> &sizes);
} // namespace kobalt
