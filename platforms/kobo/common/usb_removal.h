#pragma once

namespace kobalt {
// A missing marker counts as removal only after an installed USB session.
class UsbRemoval
{
public:
    void begin(bool initialized, bool installed) { armed = initialized && installed; }

    bool finish(bool installed)
    {
        const bool removed = armed && !installed;
        armed = false;
        return removed;
    }

private:
    bool armed = false;
};
} // namespace kobalt
