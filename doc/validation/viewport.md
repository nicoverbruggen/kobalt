# Viewport and orientation checks

Label `viewport`. Build `mac-tests`, then run `ctest --preset mac -L viewport` from `platforms/desktop/`. `ctest --preset mac -N -L viewport` lists its tests.

These checks cover manual and sensor orientation, orientation handoff at sleep and close, viewport changes, alternate layouts when rotating, full-width panels, safe areas and the Kobo touch-rotation guard.

## Sensors

Desktop builds are sensorless by default. Configure with `KOBALT_DESKTOP_SENSORS=ON` and a matching prepared Qt Sensors module to add the sensor checks: `platform.reader-orientation` then keeps the production debounce and checks sensor delivery behind the PIN screen, and `lifecycle.sleepRequestFreezesOrientationBeforeHandoff` and `lifecycle.closePublishesHeldOrientationBeforeBackgroundPaint` are registered. These sensor checks stay outside the fast lane.

## Focused runs and captures

Build `kobalt-alternate-layout-tests`, then run `ctest --preset mac -R '^engine.alternate-layout$'` for the alternate-layout check alone. Set `KOBALT_VIEWPORT_VIDEO` to an absolute output directory when you run `desktop.viewportChanges` to capture timestamped PNG frames of the headless desktop window. The recording uses real render timing, and the held-worker check runs after the recording ends.

## Touch rotation

`model.touch-rotation` compiles the production Qt5 input adapter against native Qt, without rebuilding the reader. Its device guard rejects the Clara HD, including firmware 4.38.23828 from [discussion #48](https://github.com/nicoverbruggen/kobalt/discussions/48), and keeps the correction for the Clara BW and Clara Colour. Unaffected devices keep the native transform in all four orientations. The check does not exercise a physical touch controller.

## Android emulator

Check portrait and landscape with and without a camera cutout. Rotate with Settings and a choice dialog open, then return to portrait. The reader must still fill its canvas in physical pixels after Qt delivers its screen-size events. Compare the Library and reader side insets after you close a book.

## Limits

These controlled checks do not establish physical Kobo or Android behavior, device latency, storage behavior or e-ink refresh quality.
