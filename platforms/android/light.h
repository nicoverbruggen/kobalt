#pragma once
#include "platform/api/light_controls.h"
#include <QtMath>
#include <functional>
#include <utility>

namespace kobalt::android {
// Device calls return raw levels, or a negative value on failure.
class Light final : public LightControls
{
public:
    using Device = std::function<int(int operation, int type, int value)>;

    explicit Light(Device device) : device(std::move(device)) { }

    ~Light() override { finishGesture(false); }

    bool initialize() override { return read(7) >= 0 && maximum(7) > 0; }

    bool hasWarmth() const override { return read(6) >= 0 && maximum(6) > 0; }

    bool hasAutomaticWarmth() const override { return false; }

    int warmth() const override { return read(6); }

    int warmthMinimum() const override { return 0; }

    int warmthMaximum() const override { return maximum(6); }

    int warmthStep() const override { return 1; }

    bool automaticWarmth() const override { return false; }

    int setWarmth(int value) override { return device(2, 6, value); }

    bool saveWarmth() override { return read(6) >= 0; }

    bool setAutomaticWarmth(bool) override { return false; }

    bool beginGesture() override
    {
        if (original >= 0)
            return true;
        original = read(7);
        return original >= 0;
    }

    int setGestureLevel(int percent) override
    {
        const int limit = maximum(7);
        if (original < 0 || limit <= 0)
            return -1;
        const int actual = device(2, 7, qRound(qBound(0, percent, 100) * limit / 100.0));
        return actual < 0 ? -1 : qRound(actual * 100.0 / limit);
    }

    void finishGesture(bool save) override
    {
        const int previous = std::exchange(original, -1);
        if (!save && previous >= 0)
            device(2, 7, previous);
    }

    int brightness() const override
    {
        const int value = read(7), limit = maximum(7);
        return value < 0 || limit <= 0 ? -1 : qRound(value * 100.0 / limit);
    }

    bool gesturing() const override { return original >= 0; }

private:
    Device device;
    int original = -1;

    int read(int type) const { return device(0, type, 0); }

    int maximum(int type) const { return device(1, type, 0); }
};
} // namespace kobalt::android
