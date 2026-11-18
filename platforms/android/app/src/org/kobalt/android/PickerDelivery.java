package org.kobalt.android;

import java.util.ArrayDeque;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Supplier;

/** Holds picker results until the native Library is ready. */
final class PickerDelivery {
    private final AtomicLong generation = new AtomicLong();
    private final ArrayDeque<Runnable> folders = new ArrayDeque<>();
    private Runnable book;
    private boolean ready;

    // The importer and Qt threads can start and check requests. Delivery
    // and readiness are confined to the Android UI thread.
    long beginBook() { return generation.incrementAndGet(); }
    boolean current(long request) { return generation.get() == request; }
    boolean ready() { return ready; }

    void book(long request, Runnable publish) {
        if (!current(request)) return;
        if (ready) publish.run();
        else book = () -> { if (current(request)) publish.run(); };
    }

    void folder(Runnable publish) {
        if (ready) publish.run();
        else folders.add(publish);
    }

    void markReady() {
        ready = true;
        while (!folders.isEmpty()) folders.remove().run();
        Runnable publish = book;
        book = null;
        if (publish != null) publish.run();
    }

    void destroy() {
        beginBook();
        ready = false;
        folders.clear();
        book = null;
    }

    static String label(String fallback, Supplier<String> read) {
        try {
            String label = read.get();
            if (label != null) return label;
        } catch (RuntimeException ignored) {
            // Display names are optional. Keep the selected URI and grant.
        }
        return fallback;
    }
}
