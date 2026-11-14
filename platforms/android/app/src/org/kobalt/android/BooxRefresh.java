package org.kobalt.android;

import android.graphics.Rect;
import android.os.Build;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import java.lang.reflect.Method;
import org.lsposed.hiddenapibypass.HiddenApiBypass;

/** Attach BOOX refresh metadata to the buffer Qt is about to submit. */
final class BooxRefresh {
    private Method addEpdc;
    private int gu, regal, gc;
    private volatile boolean available;
    private volatile Target target;
    private static final class Target {
        final Surface surface;
        final int x, y, width, height;
        Target(SurfaceView view) {
            surface = view.getHolder().getSurface();
            int[] origin = new int[2];
            view.getLocationOnScreen(origin);
            x = origin[0]; y = origin[1]; width = view.getWidth(); height = view.getHeight();
        }
    }
    BooxRefresh() {
        if (!"ONYX".equalsIgnoreCase(Build.MANUFACTURER) && !"ONYX".equalsIgnoreCase(Build.BRAND)) return;
        try {
            // The second exemption covers only BOOX's added Surface method.
            if (!HiddenApiBypass.addHiddenApiExemptions("Landroid/onyx/", "Landroid/view/Surface;->addEpdc([I)V")) return;
            Class<?> modes = Class.forName("android.onyx.ViewUpdateHelper");
            gu = modes.getField("UI_GU_MODE").getInt(null);
            regal = modes.getField("UI_REGAL_MODE").getInt(null);
            gc = modes.getField("UI_GC_MODE").getInt(null);
            if (gu == 0 || regal == 0 || gc == 0 || gu == regal || gu == gc || regal == gc) return;
            addEpdc = Surface.class.getMethod("addEpdc", int[].class);
            available = true;
            Log.i("KobaltBOOX", "Refresh uses Surface.addEpdc for GU, REGAL and GC");
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
        if (!available || !active) { target = null; return; }
        try {
            SurfaceView view = surface(root);
            target = view != null && view.isAttachedToWindow() && view.getWidth() > 0 && view.getHeight() > 0
                    ? new Target(view) : null;
        } catch (RuntimeException | LinkageError error) {
            target = null;
            available = false;
            Log.w("KobaltBOOX", "Refresh surface unavailable", error);
        }
    }
    // Qt GUI thread, synchronously before paint/flush. Surface.addEpdc takes its
    // native lock and the firmware consumes this metadata when queuing the buffer.
    boolean prepare(int[] regions, boolean clean) {
        Target current = target;
        if (!available || current == null || !current.surface.isValid()) return false;
        if (regions == null || regions.length % 5 != 0) return false;
        try {
            int[] entries;
            if (clean) {
                entries = new int[]{0, 0, current.width, current.height, gc};
            } else {
                entries = new int[regions.length];
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
            }
            addEpdc.invoke(current.surface, (Object)entries);
            return true;
        } catch (ReflectiveOperationException | RuntimeException | LinkageError error) {
            available = false;
            target = null;
            Log.w("KobaltBOOX", "Refresh metadata failed", error);
            return false;
        }
    }
    void cancel() { target = null; }
}
