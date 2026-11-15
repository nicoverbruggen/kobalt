package org.kobalt.android;

import org.junit.Test;
import static org.junit.Assert.*;

public class ScreenBrightnessTest {
    @Test public void followsSystemUntilManualAdjustment() {
        int[] system = { 36 }, saved = { -99 }, changes = { 0 };
        ScreenBrightness light = new ScreenBrightness(-1, () -> system[0],
                () -> ++changes[0], value -> saved[0] = value);
        assertEquals(-1.0f, light.override(), 0.0f);
        assertEquals(36, light.light(0, 7, 0));
        system[0] = 81;
        assertEquals(81, light.light(0, 7, 0));
        assertEquals(-1, light.light(0, 6, 0)); // No warmth control on an ordinary phone.
        assertEquals(21, light.light(2, 7, 21));
        assertEquals(0.21f, light.override(), 0.0001f);
        assertEquals(0, light.light(4, 7, 0));
        assertEquals(-99, saved[0]); // Drag updates do not write preferences.
        light.light(6, 7, 0);
        assertEquals(21, saved[0]);
        ScreenBrightness reopened = new ScreenBrightness(saved[0], () -> system[0], () -> {}, value -> {});
        assertEquals(21, reopened.light(0, 7, 0));
        light.light(5, 7, 1);
        assertEquals(-1, saved[0]);
        assertEquals(-1.0f, light.override(), 0.0f);
        assertEquals(1, light.light(4, 7, 0));
        light.light(5, 7, 0);
        assertEquals(81, saved[0]);
        assertEquals(3, changes[0]);
    }

    @Test public void cancellationRestoresSystemWithoutSaving() {
        int[] writes = { 0 };
        ScreenBrightness light = new ScreenBrightness(-1, () -> 42, () -> {}, value -> ++writes[0]);
        light.light(2, 7, 65);
        light.light(2, 7, 42);
        light.light(7, 7, 1);
        assertEquals(-1.0f, light.override(), 0.0f);
        assertEquals(0, writes[0]);
    }

    @Test public void invalidValuesDoNotBecomeOverrides() {
        for (int saved : new int[] { -100, -1, 101, Integer.MAX_VALUE }) {
            ScreenBrightness light = new ScreenBrightness(saved, () -> 40, () -> {}, value -> fail());
            assertEquals(-1.0f, light.override(), 0.0f);
            assertEquals(-1, light.light(2, 7, -1));
            assertEquals(-1, light.light(2, 7, 101));
            assertEquals(-1.0f, light.override(), 0.0f);
        }
        for (int value : new int[] { 0, 100 }) {
            ScreenBrightness light = new ScreenBrightness(value, () -> 40, () -> {}, ignored -> {});
            assertEquals(value / 100.0f, light.override(), 0.0f);
        }
    }
}
