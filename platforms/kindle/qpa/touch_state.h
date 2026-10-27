#pragma once

#include <algorithm>
#include <array>

namespace kindle {

struct ContactGate {
    bool blocked = false, waitingForLift = false;
    void setBlocked(bool value, bool contactDown) {
        if (value && !blocked)
            waitingForLift = contactDown;
        blocked = value;
    }
    bool deliver(bool contactDown) {
        const bool allowed = !blocked && !waitingForLift;
        if (blocked && contactDown)
            waitingForLift = true;
        if (!contactDown)
            waitingForLift = false;
        return allowed;
    }
};

// The prototype exposes one finger as a pointer. Other slots cannot steal a drag.
struct TouchState {
    static constexpr int maximumSlots = 64;
    struct Position { int x = 0, y = 0; };
    std::array<Position, maximumSlots> positions{};
    int slot = 0;
    int primarySlot = -1;
    int x = 0;
    int y = 0;
    bool down = false;
    bool sentDown = false;
    bool dropped = false;
    bool skipPacket = false;

    void tracking(int id)
    {
        if (slot < 0 || slot >= maximumSlots) return;
        if (id >= 0 && primarySlot < 0) {
            primarySlot = slot;
            down = true;
            x = positions[slot].x;
            y = positions[slot].y;
        } else if (id < 0 && primarySlot == slot) {
            down = false;
            // Keep ownership until the release frame has been delivered.
        }
    }

    void contact(bool value, bool slotted = false)
    {
        if (!value) down = false;
        // BTN_TOUCH precedes ABS_MT_SLOT. Only TRACKING_ID can choose the
        // owning slot in protocol B; the current slot may be an old contact.
        else if (!slotted && primarySlot < 0) {
            primarySlot = slot;
            down = true;
        } else if (!slotted && primarySlot == slot) {
            down = true;
        }
    }

    void position(bool horizontal, int value)
    {
        if (skipPacket || slot < 0 || slot >= maximumSlots) return;
        auto &position = positions[slot];
        (horizontal ? position.x : position.y) = value;
        if (primarySlot == slot) {
            x = position.x;
            y = position.y;
        }
    }

    bool acceptsPosition() const { return !skipPacket && (primarySlot < 0 || slot == primarySlot); }

    void frameSent()
    {
        sentDown = down;
        if (!down) primarySlot = -1;
        skipPacket = false;
    }
};

inline double axisPosition(int value, int minimum, int maximum)
{
    if (maximum <= minimum) return 0;
    return std::clamp((double(value) - minimum) / (double(maximum) - minimum), 0.0, 1.0);
}

} // namespace kindle
