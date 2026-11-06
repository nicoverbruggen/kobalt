#pragma once
#include <QImage>
#include <QRegion>
#include <cstring>

namespace kobalt {
// Typing changes the key and its editor in the same completed paint. One
// partial update keeps them together without driving the unchanged gap in
// full mode. Opening or restyling the entire keyboard keeps its normal policy.
inline bool keyboardEditDamage(const QRegion &damage, const QRect &keyboard, const QRect &editor)
{
    return !damage.isEmpty() && !keyboard.isEmpty() && !editor.isEmpty()
            && !QRegion(keyboard).subtracted(damage).isEmpty()
            && damage.subtracted(QRegion(keyboard) + editor).isEmpty();
}

// Nickel can add a mode-only keyboard entry alongside Qt's text policy.
// Complete its waveform so it does not bypass the reader's damage splitting.
inline quint64 completeKeyboardWaveform(const QRect &damage, const QRect &keyboard, quint64 policy,
                                        quint64 ordinary, quint64 modeMask)
{
    const auto mode = policy & modeMask;
    if (!damage.isEmpty() && !keyboard.isEmpty() && keyboard.contains(damage) && policy == mode
        && mode && !(mode & (mode - 1)))
        return (ordinary & ~modeMask) | mode;
    return policy;
}

// Mode variants can describe the same keyboard pixels in one Qt flush. Keep
// the strongest requested mode so a new full update cannot match a partial one.
template <class Policies>
quint64 commonKeyboardPolicy(const Policies &policies, quint64 modeMask, quint64 fullMode,
                             quint64 heuristicMode)
{
    if (policies.isEmpty())
        return 0;
    const quint64 waveform = quint64(policies[0].second) & ~modeMask;
    quint64 modes = 0;
    for (const auto &entry : policies) {
        const quint64 policy = entry.second, mode = policy & modeMask;
        if ((policy & ~modeMask) != waveform || !mode || (mode & (mode - 1)))
            return 0;
        modes |= mode;
    }
    const auto mode = modes & fullMode ? fullMode : modes & heuristicMode ? heuristicMode : modes;
    return waveform | mode;
}

class KeyboardRefreshCache
{
public:
    void clear()
    {
        pixels = {};
        bounds = {};
        submitted = {};
        lastPolicy = 0;
    }

    bool unchanged(const QImage *image, const QRegion &damage, const QRect &keyboard,
                   quint64 policy, bool maySkip)
    {
        if (!image || image->depth() != 32 || keyboard.isEmpty()
            || !image->rect().contains(keyboard)) {
            clear();
            return false;
        }
        const QRegion inside = damage.intersected(keyboard).translated(-keyboard.topLeft());
        if (bounds != keyboard || pixels.format() != image->format()) {
            bounds = keyboard;
            pixels = image->copy(keyboard);
            lastPolicy = policy;
            submitted = inside;
            return false;
        }
        if (policy != lastPolicy) {
            submitted = {};
        }
        const bool covered = inside.subtracted(submitted).isEmpty();
        bool same = true;
        for (const auto &part :
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
             inside.rects()
#else
             inside
#endif
        ) {
            const int bytes = part.width() * 4;
            for (int y = part.top(); y <= part.bottom(); ++y) {
                const auto *source = image->constScanLine(y + keyboard.top())
                        + (part.left() + keyboard.left()) * 4;
                auto *previous = pixels.scanLine(y) + part.left() * 4;
                if (std::memcmp(source, previous, bytes) != 0) {
                    same = false;
                    std::memcpy(previous, source, bytes);
                }
            }
        }
        const bool skip = maySkip && damage.boundingRect().isValid()
                && keyboard.contains(damage.boundingRect()) && policy == lastPolicy && covered
                && same;
        submitted += inside;
        lastPolicy = policy;
        return skip;
    }

private:
    QImage pixels;
    QRect bounds;
    QRegion submitted;
    quint64 lastPolicy = 0;
};
} // namespace kobalt
