package org.kobalt.android;

import java.util.ArrayList;
import java.util.List;
import org.junit.Test;
import static org.junit.Assert.*;

public class AndroidLifecycleTest {
    @Test public void earlyFolderResultsReachTheReadyLibraryOnce() {
        PickerDelivery picker = new PickerDelivery();
        List<String> received = new ArrayList<>();
        picker.folder(() -> received.add("early"));
        assertTrue(received.isEmpty());
        picker.markReady();
        picker.markReady();
        picker.folder(() -> received.add("ready"));
        assertEquals(List.of("early", "ready"), received);
    }

    @Test public void readinessDoesNotOpenASupersededImport() {
        PickerDelivery picker = new PickerDelivery();
        List<String> opened = new ArrayList<>();
        long first = picker.beginBook();
        picker.book(first, () -> opened.add("first"));
        long second = picker.beginBook();
        picker.markReady();
        picker.book(first, () -> opened.add("old callback"));
        assertTrue(opened.isEmpty());
        picker.book(second, () -> opened.add("second"));
        assertEquals(List.of("second"), opened);
    }

    @Test public void readinessOpensACurrentImportAndDestroyClearsPendingResults() {
        PickerDelivery picker = new PickerDelivery();
        List<String> received = new ArrayList<>();
        long first = picker.beginBook();
        picker.book(first, () -> received.add("first"));
        picker.markReady();
        assertEquals(List.of("first"), received);

        picker.destroy();
        long second = picker.beginBook();
        picker.book(second, () -> received.add("second"));
        picker.folder(() -> received.add("folder"));
        picker.destroy();
        assertFalse(picker.current(second));
        picker.markReady();
        assertEquals(List.of("first"), received);
    }

    @Test public void aMissingProviderLabelDoesNotRejectTheFolder() {
        assertEquals("Books", PickerDelivery.label("tree-id", () -> "Books"));
        assertEquals("tree-id", PickerDelivery.label("tree-id", () -> null));
        assertEquals("tree-id", PickerDelivery.label("tree-id", () -> {
            throw new SecurityException("Provider did not return a display name");
        }));
    }

    @Test public void anAmbiguousSurfaceSnapshotDoesNotSelectATarget() {
        BooxRefresh.SurfaceSearch<String> initial = new BooxRefresh.SurfaceSearch<>();
        assertNull(initial.only());
        assertTrue(initial.add("old surface"));
        assertEquals("old surface", initial.only());
        assertFalse(initial.add("new surface"));
        assertNull(initial.only());

        BooxRefresh.SurfaceSearch<String> subsequent = new BooxRefresh.SurfaceSearch<>();
        assertTrue(subsequent.add("new surface"));
        assertEquals("new surface", subsequent.only());
    }
}
