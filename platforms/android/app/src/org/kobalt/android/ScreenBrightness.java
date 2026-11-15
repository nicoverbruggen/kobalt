package org.kobalt.android;

import java.util.function.IntConsumer;
import java.util.function.IntSupplier;

/** App-window brightness. A negative saved level follows Android's brightness policy. */
final class ScreenBrightness {
    private int level;
    private final IntSupplier system;
    private final Runnable changed;
    private final IntConsumer save;

    ScreenBrightness(int saved, IntSupplier system, Runnable changed, IntConsumer save) {
        level = saved >= 0 && saved <= 100 ? saved : -1;
        this.system = system;
        this.changed = changed;
        this.save = save;
    }

    synchronized float override() { return level < 0 ? -1.0f : level / 100.0f; }

    synchronized int light(int operation, int type, int value) {
        if (type != 7) return -1;
        switch (operation) {
        case 0: return level < 0 ? Math.max(0, Math.min(100, system.getAsInt())) : level;
        case 1: return 100;
        case 2:
            if (value < 0 || value > 100) return -1;
            level = value;
            changed.run();
            return level;
        case 3: return 1; // Uses Android's window brightness policy.
        case 4: return level < 0 ? 1 : 0;
        case 5:
        case 7: // Restore a cancelled gesture without saving it.
            level = value != 0 ? -1 : light(0, type, 0);
            changed.run();
            if (operation == 5) save.accept(level);
            return 1;
        case 6:
            save.accept(level);
            return 1;
        default: return -1;
        }
    }
}
