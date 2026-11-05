# Brightness checks

Label `brightness`. Build `mac-tests`, then run `ctest --preset mac -L brightness` from `platforms/desktop/`. `ctest --preset mac -N -L brightness` lists its tests.

These checks cover the brightness and warmth gestures, light feedback, saved light levels and the Light panel. The desktop checks drive the production reader callbacks with a controlled light service. The Light shortcut for dark mode is in [Settings and controls](settings.md).

These native checks do not verify firmware light persistence or physical e-ink refresh.
