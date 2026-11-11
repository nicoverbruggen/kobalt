#pragma once
#include "style_sample.h"
#include <QByteArray>
#include <QList>

namespace kobalt {
// Identifies a sample by everything that decides its pixels: the whole
// request, the font inputs and the build's rendering policy.
QByteArray styleSampleKey(const EngineFontInputs &fonts, const StyleSampleRequest &request);

// Rendered Styling samples of one open book, kept in memory so that opening a
// sample again does not lay it out again. GUI thread only. Portrait and
// landscape have their own entries and their own size limit, so a rotation
// and back keeps both sets. When an orientation is over its limit, the sample
// used longest ago leaves first. A page that is grey in every pixel, as on the
// light and dark themes, is held with one byte per pixel.
class StyleSampleCache
{
public:
    static constexpr qint64 defaultLimit = 6 * 1024 * 1024;

    explicit StyleSampleCache(qint64 limit = defaultLimit) : limit(limit) { }

    // True with the sample in `sample` when the orientation has `key`.
    bool find(bool landscape, const QByteArray &key, StyleSample *sample);
    // Ignores an invalid sample and one that is larger than the limit.
    void insert(bool landscape, const QByteArray &key, const StyleSample &sample);
    void clear();

    int count(bool landscape) const { return entries[landscape].size(); }

    // Bytes of image data held for the orientation.
    qint64 bytes(bool landscape) const;

private:
    struct Entry
    {
        QByteArray key;
        StyleSample sample;
        qint64 bytes;
    };

    // Most recently used first.
    QList<Entry> entries[2];
    qint64 limit;
};
} // namespace kobalt
