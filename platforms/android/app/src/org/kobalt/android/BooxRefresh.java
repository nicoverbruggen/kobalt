package org.kobalt.android;

import android.graphics.Rect;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import java.lang.reflect.Method;
import org.lsposed.hiddenapibypass.HiddenApiBypass;

/** Submit partial metadata before paint and request cleaning after the frame settles. */
final class BooxRefresh {
    private Method addEpdc;
    private Method refreshScreen, setWaveformAndScheme;
    private int gu, regal;
    // BOOX View.refreshScreen uses waveform flags, not ViewUpdateHelper's UI modes.
    private static final int FULL_GC16_WAIT = 32 | 64 | 2;
    private static final int CLEAN_DELAY_MS = 250;
    private final Object requests = new Object();
    private HandlerThread refreshThread;
    private Handler refreshWorker;
    private volatile int generation;
    private volatile boolean available;
    private volatile Target target;
    private static final class Target {
        final Surface surface;
        final SurfaceView view;
        final int x, y, width, height;
        Target(SurfaceView view) {
            this.view = view;
            surface = view.getHolder().getSurface();
            int[] origin = new int[2];
            view.getLocationOnScreen(origin);
            x = origin[0]; y = origin[1]; width = view.getWidth(); height = view.getHeight();
        }
    }
    BooxRefresh() {
        if (!"ONYX".equalsIgnoreCase(Build.MANUFACTURER) && !"ONYX".equalsIgnoreCase(Build.BRAND)) return;
        try {
            if (!HiddenApiBypass.addHiddenApiExemptions("Landroid/onyx/", "Landroid/view/Surface;->addEpdc([I)V",
                    "Landroid/view/View;->refreshScreen(", "Landroid/view/View;->setWaveformAndScheme(")) return;
            Class<?> modes = Class.forName("android.onyx.ViewUpdateHelper");
            gu = modes.getField("UI_GU_MODE").getInt(null);
            regal = modes.getField("UI_REGAL_MODE").getInt(null);
            if (gu == 0 || regal == 0 || gu == regal) return;
            addEpdc = Surface.class.getMethod("addEpdc", int[].class);
            refreshScreen = View.class.getMethod("refreshScreen", int.class, int.class, int.class, int.class, int.class);
            try {
                setWaveformAndScheme = View.class.getMethod("setWaveformAndScheme", int.class, int.class, int.class);
            } catch (ReflectiveOperationException error) {
                // Like KOReader, attempt the explicit refresh even without policy control.
                Log.w("KobaltBOOX", "System refresh policy control unavailable", error);
            }
            refreshThread = new HandlerThread("KobaltBOOXRefresh");
            refreshThread.start();
            refreshWorker = new Handler(refreshThread.getLooper());
            available = true;
            Log.i("KobaltBOOX", "Cleaning refresh uses View.refreshScreen, delay=250ms mode=98");
        } catch (ReflectiveOperationException | RuntimeException | LinkageError error) {
            Log.w("KobaltBOOX", "Refresh controls unavailable", error);
        }
    }
    boolean available() { return available; }
    private SurfaceView surface(View root) {
        if (!root.isShown()) return null;
        if (root.getClass().getName().equals("org.qtproject.qt.android.QtSurface")) return (SurfaceView)root;
        SurfaceView found = null;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)root;
            for (int i = 0; i < group.getChildCount(); ++i) {
                SurfaceView child = surface(group.getChildAt(i));
                if (child != null) {
                    if (found != null) throw new IllegalStateException("Multiple Qt surfaces");
                    found = child;
                }
            }
        }
        return found;
    }
    // Android UI thread publishes immutable geometry; Qt never reads View state.
    void publish(View root, boolean active) {
        if (!available || !active) { cancel(); return; }
        try {
            SurfaceView view = surface(root);
            target = view != null && view.isAttachedToWindow() && view.getWidth() > 0 && view.getHeight() > 0
                    ? new Target(view) : null;
        } catch (RuntimeException | LinkageError error) {
            cancel();
            available = false;
            Log.w("KobaltBOOX", "Refresh surface unavailable", error);
        }
    }
    // Partial metadata must precede Qt's flush. Cleaning uses the separate View API
    // after the same 250 ms settling delay used by KOReader's Onyx controller.
    boolean prepare(int[] regions, boolean clean) {
        Target current = target;
        if (!available || current == null || !current.surface.isValid()) return false;
        if (regions == null || regions.length % 5 != 0) return false;
        if (clean) return queueClean(current);
        try {
            int[] entries = new int[regions.length];
            int count = 0;
            for (int i = 0; i < regions.length; i += 5) {
                int kind = regions[i + 4];
                if (kind != 0 && kind != 1) return false;
                // Entries use left/top/right/bottom, in physical screen pixels.
                Rect area = new Rect(regions[i], regions[i + 1], regions[i + 2], regions[i + 3]);
                area.offset(-current.x, -current.y);
                if (!area.intersect(0, 0, current.width, current.height)) continue;
                entries[count++] = area.left;
                entries[count++] = area.top;
                entries[count++] = area.right;
                entries[count++] = area.bottom;
                entries[count++] = kind == 1 ? regal : gu;
            }
            if (count == 0) return false;
            if (count != entries.length) entries = java.util.Arrays.copyOf(entries, count);
            addEpdc.invoke(current.surface, (Object)entries);
            return true;
        } catch (ReflectiveOperationException | RuntimeException | LinkageError error) {
            available = false;
            cancel();
            Log.w("KobaltBOOX", "Refresh metadata failed", error);
            return false;
        }
    }
    private boolean current(Target requested, int epoch) {
        Target active = target;
        return available && generation == epoch && active != null && active.view == requested.view
                && active.surface == requested.surface && active.width == requested.width
                && active.height == requested.height && active.x == requested.x && active.y == requested.y
                && active.surface.isValid();
    }
    private boolean queueClean(Target requested) {
        synchronized (requests) {
            final int epoch = generation;
            if (!current(requested, epoch)) return false;
            // Coalesce page/menu bursts into one cleaning of the latest rendered frame.
            refreshWorker.removeCallbacksAndMessages(null);
            refreshWorker.post(() -> {
                if (!current(requested, epoch) || setWaveformAndScheme == null) return;
                try {
                    // Same system-update suppression as KOReader's Qualcomm controller.
                    setWaveformAndScheme.invoke(null, 5, 1, 0);
                } catch (ReflectiveOperationException | RuntimeException | LinkageError error) {
                    Log.w("KobaltBOOX", "System refresh policy control failed", error);
                }
            });
            boolean queued = refreshWorker.postDelayed(() -> {
                if (!current(requested, epoch)) return;
                try {
                    refreshScreen.invoke(requested.view, requested.x, requested.y,
                            requested.width, requested.height, FULL_GC16_WAIT);
                    Log.i("KobaltBOOX", "Cleaning refresh invoked: View.refreshScreen mode=98 "
                            + requested.width + "x" + requested.height);
                } catch (ReflectiveOperationException | RuntimeException | LinkageError error) {
                    available = false;
                    cancel();
                    Log.w("KobaltBOOX", "Cleaning refresh failed", error);
                }
            }, CLEAN_DELAY_MS);
            if (queued) Log.i("KobaltBOOX", "Cleaning refresh queued: View.refreshScreen");
            return queued;
        }
    }
    void cancel() {
        synchronized (requests) {
            target = null;
            ++generation;
            if (refreshWorker != null) refreshWorker.removeCallbacksAndMessages(null);
        }
    }
    void close() {
        available = false;
        cancel();
        if (refreshThread != null) refreshThread.quitSafely();
    }
}
