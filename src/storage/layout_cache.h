#pragma once
#include "publication/layout_compatibility.h"
#include <QJsonArray>

namespace kobalt {
// Only completed inspection verdicts survive an authorized reading session.
// The caller supplies a freshly computed digest, never a filename or timestamp.
class LayoutCache
{
public:
    // Cached books skip parsing, so keep their verdicts for as many books.
    static constexpr int maxEntries = 20, maxBytes = 8192;
    LayoutCache(QString path, QByteArray policy);
    bool lookup(const QByteArray &digest, LayoutCompatibility *result);
    bool store(const QByteArray &digest, const LayoutCompatibility &result);

private:
    QString path_;
    QByteArray policy_;
    QJsonArray entries_;
    bool save();
};
} // namespace kobalt
