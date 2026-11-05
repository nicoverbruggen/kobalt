#pragma once
#include <QTime>

namespace kobalt {
// One window owns one light controller. A cancelled gesture restores its
// original value and automatic-brightness setting; a committed gesture
// persists through the host settings. Manual brightness changes disable
// automatic brightness on hosts that support it.
class LightControls
{
public:
    virtual ~LightControls() = default;
    virtual bool initialize() = 0;

    virtual bool hasAutomaticBrightness() const { return false; }

    virtual bool automaticBrightness() const { return false; }

    virtual bool setAutomaticBrightness(bool) { return false; }

    virtual bool hasWarmth() const = 0;
    virtual int warmth() const = 0;
    virtual int warmthMinimum() const = 0;
    virtual int warmthMaximum() const = 0;
    virtual int warmthStep() const = 0;

    virtual bool hasAutomaticWarmth() const { return hasWarmth(); }

    virtual bool automaticWarmth() const = 0;
    // Return the actual level, or a negative value on failure. Native levels may round.
    virtual int setWarmth(int value) = 0;
    virtual bool saveWarmth() = 0;
    virtual bool setAutomaticWarmth(bool enabled) = 0;
    virtual bool beginGesture() = 0;
    // Return the actual percentage, or a negative value on failure.
    virtual int setGestureLevel(int percent) = 0;
    virtual void finishGesture(bool save) = 0;
    virtual int brightness() const = 0;
    virtual bool gesturing() const = 0;
};
} // namespace kobalt
