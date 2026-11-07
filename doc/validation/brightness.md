# Brightness checks

Label `brightness`. Build `mac-tests`, then run `ctest --preset mac -L brightness` from `platforms/desktop/`. `ctest --preset mac -N -L brightness` lists its tests.

These checks cover the brightness and warmth gestures, light feedback, saved light levels and the Light panel. Quick brightness and warmth swipes keep the final percentage visible for at least 1.5 seconds. Updates during a swipe still wait for a 150 ms pause. The desktop checks drive the production reader callbacks with a controlled light service. The Light shortcut for dark mode is in [Settings and controls](settings.md).

These native checks do not verify firmware light persistence or physical e-ink refresh.

The Light panel keeps compact described-row padding so its controls fit between the toolbar and navigation in phone landscape. The dark-mode shortcut gallery check verifies the panel bounds and complete control visibility.
