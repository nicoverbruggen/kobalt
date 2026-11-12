#pragma once
#include <QString>
#include <QVector>

namespace kobalt {
// A position in the source XHTML of one spine item. It does not depend on
// the font, the viewport, the pages or the rendering engine.
struct Location
{
    static constexpr int currentVersion = 1;
    int version = currentVersion;
    // The spine item, as its path in the publication.
    QString href;
    // Element indexes from <body>, one for each level. Only elements count.
    // Empty with a negative offset means the start of the spine item.
    QVector<int> path;
    // The number of code points in the element's text before the position,
    // not counting the characters of locationUncounted(). Negative means the
    // element itself.
    int offset = -1;
    // Which side of a gap of uncounted characters: the start of the text that follows
    // (true) or the end of the text before (false).
    bool after = true;

    bool valid() const { return version == currentVersion && !href.isEmpty(); }

    bool operator==(const Location &other) const
    {
        return version == other.version && href == other.href && path == other.path
                && offset == other.offset && after == other.after;
    }

    bool operator!=(const Location &other) const { return !(*this == other); }
};

// How well an engine found a location in its own document.
enum class Restoration {
    Exact, // The path and the offset resolved.
    Approximate, // The element resolved, the offset did not.
    Unresolved, // Nothing resolved. The caller keeps the location unchanged.
};

// The characters that Location::offset does not count: whitespace, which an
// engine may collapse or drop, and zero-width format characters, which
// CREngine drops in some places (a U+FEFF before a dash, for example).
constexpr bool locationUncounted(char32_t c)
{
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f' || c == 0x00AD
            || c == 0x200B || c == 0x2060 || c == 0xFEFF;
}
} // namespace kobalt
