#pragma once
#include <QList>
#include <QPair>
#include <QRect>
#include <QRegion>

namespace kobalt {
// The policy bits that merging MTK updates reads. Qt 5 and Qt 6 spell them
// differently, so each adapter passes its own.
struct UpdateBits
{
    quint64 waveforms = 0; // every waveform bit
    quint64 cleanups = 0; // waveforms that flash or clean images
    quint64 modes = 0; // every update-mode bit
    quint64 full = 0, partial = 0;
    quint64 text = 0; // the text waveform for updates whose waveforms differ
    quint64 kept = 0; // flags that any part carries over, such as dark mode
};

// MTK refreshes each rectangle of each flush as its own screen update. One
// paint therefore became several updates: one per panel, one per row of a
// rounded mask, or a panel and its tabs. The panel shows them one after the
// other. Merges the updates of one paint into one update over their bounding
// rectangle. Partial mode drives only pixels that changed, so the unchanged
// pixels between the parts stay as they are. Full mode remains only when the
// parts already cover the whole rectangle. A flash or image cleanup keeps its
// own updates: it must refresh exactly its area with its waveform.
template <class Flags>
QList<QPair<QRegion, Flags>> singleUpdate(const QList<QPair<QRegion, Flags>> &calls,
                                          const UpdateBits &bits)
{
    int entries = 0;
    for (const auto &call : calls)
        entries += call.second.size();
    if (calls.size() <= 1 && entries <= 1)
        return calls;
    QRegion damage;
    quint64 waveform = 0, kept = 0;
    bool sameWaveform = true, allFull = true;
    for (const auto &call : calls) {
        damage += call.first;
        for (const auto &entry : call.second) {
            if (entry.second & bits.cleanups)
                return calls;
            const quint64 part = entry.second & bits.waveforms;
            if (!waveform)
                waveform = part;
            else if (part != waveform)
                sameWaveform = false;
            kept |= entry.second & bits.kept;
            allFull = allFull && (entry.second & bits.modes) == bits.full;
        }
    }
    if (damage.isEmpty())
        return calls;
    const QRect bounds = damage.boundingRect();
    const bool covered = QRegion(bounds).subtracted(damage).isEmpty();
    const quint64 mode = allFull && covered ? bits.full : bits.partial;
    const quint64 policy = (sameWaveform && waveform ? waveform : bits.text) | mode | kept;
    return { { QRegion(bounds), Flags{ { bounds, policy } } } };
}
} // namespace kobalt
