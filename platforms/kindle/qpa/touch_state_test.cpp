#include "touch_state.h"

#include <cstdlib>
#include <iostream>
#include <limits>

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main()
{
    kindle::TouchState touch;
    touch.slot = 3;
    touch.tracking(100);
    check(touch.down && touch.primarySlot == 3, "first contact owns the pointer");
    touch.frameSent();
    check(touch.slot == 3, "protocol B slot persists across SYN_REPORT");
    touch.slot = 4;
    touch.tracking(101);
    check(!touch.acceptsPosition(), "second contact cannot move the primary pointer");
    touch.tracking(-1);
    check(touch.down, "second contact release cannot end the drag");
    touch.slot = 3;
    touch.tracking(-1);
    check(!touch.down && touch.sentDown && touch.primarySlot == 3, "primary release retains ownership until dispatched");
    touch.slot = 4;
    touch.tracking(102);
    check(!touch.down, "second contact cannot replace an undelivered release");
    touch.frameSent();
    touch.tracking(103);
    check(touch.down && touch.primarySlot == 4, "new contact can start after release");
    touch.contact(false);
    check(!touch.down, "BTN_TOUCH release works without tracking-id lift");
    touch.frameSent();
    touch.slot = 4;
    touch.contact(true, true);
    check(touch.primarySlot == -1, "protocol B BTN_TOUCH cannot claim a stale slot");
    touch.slot = 7;
    touch.position(true, 123);
    touch.position(false, 456);
    touch.tracking(104);
    check(touch.x == 123 && touch.y == 456, "coordinates before tracking id belong to their slot");
    touch.frameSent();
    touch.slot = 8;
    touch.position(true, 900);
    touch.position(false, 800);
    touch.tracking(105);
    check(touch.x == 123 && touch.y == 456, "secondary coordinates cannot move a drag");
    touch.slot = 7;
    touch.tracking(-1);
    touch.frameSent();
    touch.slot = 8;
    touch.tracking(-1);
    touch.tracking(106);
    touch.position(false, 801);
    check(touch.x == 900 && touch.y == 801, "new contact keeps its slot's omitted unchanged axis");
    touch.slot = kindle::TouchState::maximumSlots;
    touch.position(true, 10);
    check(touch.x == 900, "invalid slot cannot index outside the coordinate cache");
    check(kindle::axisPosition(10, 10, 110) == 0, "minimum maps to first pixel");
    check(kindle::axisPosition(110, 10, 110) == 1, "maximum maps to last pixel");
    check(kindle::axisPosition(-1, 10, 110) == 0, "low outlier is clamped");
    check(kindle::axisPosition(999, 10, 110) == 1, "high outlier is clamped");
    check(kindle::axisPosition(50, 10, 10) == 0, "degenerate axis cannot divide by zero");
    check(kindle::axisPosition(0, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()) > 0.49,
          "wide axis range cannot overflow");
    kindle::ContactGate gate;
    check(gate.deliver(true), "Awake contact was blocked");
    gate.setBlocked(true, true);
    check(!gate.deliver(true), "Sleep contact was delivered");
    gate.setBlocked(false, true);
    check(!gate.deliver(true) && !gate.deliver(false), "Held contact survived wake as a gesture");
    check(gate.deliver(true), "Fresh wake contact was not accepted");
    gate.setBlocked(true, false);
    check(!gate.deliver(true) && !gate.deliver(false), "Sleeping tap was delivered");
    gate.setBlocked(false, false);
    check(gate.deliver(true), "Completed sleeping tap blocked the next awake contact");
    return 0;
}
