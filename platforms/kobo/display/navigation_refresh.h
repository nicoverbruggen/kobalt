#pragma once
#include <QRegion>
#include <QVariant>
#include <QWidget>

namespace kobalt {
// A same-tab dismissal restores the page and deselects its tab in one frame.
// Only proven text-only pages may lower the entire frame to partial mode.
template <class Policies, class Submit>
bool submitNavigationDismissal(const Policies &policies, const QRegion &damage,
                               const QRegion &navigation, quint64 ordinary, quint64 repair,
                               quint64 modeMask, quint64 partialMode, quint64 fullMode,
                               quint64 immediateFlush, bool plain, Submit submit)
{
    if (policies.isEmpty() || damage.intersected(navigation).isEmpty()
        || damage.subtracted(navigation).isEmpty())
        return false;
    bool textOnly = true;
    for (const auto &entry : policies) {
        const auto waveform = entry.second & ~modeMask;
        const auto mode = entry.second & modeMask;
        if (!mode || (mode & (mode - 1))
            || (waveform != (ordinary & ~modeMask) && waveform != (repair & ~modeMask)))
            return false;
        textOnly &= waveform == (ordinary & ~modeMask);
    }
    const QRect combined = damage.boundingRect();
    if (textOnly) {
        const quint64 policy =
                (ordinary & ~modeMask) | (plain ? partialMode : fullMode) | immediateFlush;
        submit(QRegion(combined), Policies{ { combined, policy } });
    } else {
        // Keep distinct waveform requests intact. Nickel resolves the stronger
        // cleanup over the combined rectangle; never combine waveform bits.
        Policies retained = policies;
        for (auto &entry : retained)
            entry.second |= immediateFlush;
        submit(QRegion(combined), retained);
    }
    return true;
}

// The tabs contain monochrome controls. Exclude every visible sibling above
// them, including translucent overlays, before changing their refresh mode.
inline QRegion navigationRefreshRegion(const QWidget *window)
{
    QRegion region;
    bool found = false;
    for (auto *child : window->children()) {
        auto *widget = qobject_cast<QWidget *>(child);
        if (!widget || !widget->isVisibleTo(window))
            continue;
        if (widget->property("reader_navigation_bar").toBool()) {
            region = widget->geometry().intersected(window->rect());
            found = true;
        } else if (found) {
            region -= widget->mask().isEmpty() ? QRegion(widget->geometry())
                                               : widget->mask().translated(widget->pos());
        }
    }
    return region;
}

// Qt can merge a newly opened panel and its selected tab into one rectangle.
// Keep the panel's policy, but update only changed pixels in the tab bar.
template <class Policies>
Policies partialNavigationRefresh(const Policies &policies, const QRegion &damage,
                                  const QRegion &navigation, quint64 ordinary, quint64 modeMask,
                                  quint64 partialMode)
{
    Policies result;
    for (const auto &entry : policies) {
        const QRegion partial = (entry.second & ~modeMask) == (ordinary & ~modeMask)
                ? navigation.intersected(damage).intersected(entry.first)
                : QRegion();
        if (partial.isEmpty()) {
            result.append(entry);
            continue;
        }
        const QRegion remainder = QRegion(entry.first).subtracted(partial);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        const auto remainingRects = remainder.rects(), partialRects = partial.rects();
#else
        const auto &remainingRects = remainder, &partialRects = partial;
#endif
        for (const auto &rect : remainingRects)
            result.append({ rect, entry.second });
        for (const auto &rect : partialRects)
            result.append({ rect, (entry.second & ~modeMask) | partialMode });
    }
    return result;
}

template <class Policies, class Submit>
void submitNavigationRefresh(const Policies &policies, const QRegion &damage,
                             const QRegion &navigation, quint64 ordinary, quint64 modeMask,
                             quint64 partialMode, quint64 immediateFlush, Submit submit)
{
    QRegion partial = navigation.intersected(damage), coverage;
    for (const auto &entry : policies) {
        if ((entry.second & ~modeMask) == (ordinary & ~modeMask))
            coverage += entry.first;
        else
            partial -= entry.first;
    }
    partial &= coverage;
    if (partial.isEmpty()) {
        submit(damage, policies);
        return;
    }
    const auto selected =
            partialNavigationRefresh(policies, damage, partial, ordinary, modeMask, partialMode);
    // MTK chooses one mode for each damage rectangle even when its policy
    // table names smaller pieces. Submit the panel and tabs separately, or
    // the panel's full mode wins over the tab's partial mode again. Flush
    // each part now so the compositor cannot merge the queued calls either.
    for (const auto &region : { damage.subtracted(partial), partial }) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        const auto rects = region.rects();
#else
        const auto &rects = region;
#endif
        for (const auto &rect : rects) {
            Policies clipped;
            for (const auto &entry : selected) {
                const auto intersection = entry.first.intersected(rect);
                const auto policy = entry.second | immediateFlush;
                if (!intersection.isEmpty() && !clipped.contains({ intersection, policy }))
                    clipped.append({ intersection, policy });
            }
            submit(QRegion(rect), clipped);
        }
    }
}
} // namespace kobalt
