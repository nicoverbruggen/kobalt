# Viewport and orientation checks

Label `viewport`. Build `mac-tests`, then run `ctest --preset mac -L viewport` from `platforms/desktop/`. `ctest --preset mac -N -L viewport` lists its tests.

These checks cover manual and sensor orientation, orientation handoff at sleep and close, viewport changes, alternate layouts when rotating, full-width panels, safe areas and the Kobo touch-rotation guard.

`lifecycle.settingsOrientationHandoff` uses the production sensor adapter and Settings close cell. It checks that Settings holds a turn until dismissal, that closing Settings hides the reading controls and delivers the queued turn without another sample, and that portrait and landscape locks still reject the other axis. It also checks inverted landscape with a landscape lock. This check requires `KOBALT_DESKTOP_SENSORS=ON`.

`lifecycle.rotationRoundTripKeepsReadingResponsive` sends a return-to-portrait sensor reading while landscape reflow is held on the engine worker. It checks both landscape directions, one and two columns, delivery after reflow, the saved position and page input after the return. It requires `KOBALT_DESKTOP_SENSORS=ON` and does not reproduce firmware or physical sensor timing.

## Sensors

Desktop builds are sensorless by default. Configure with `KOBALT_DESKTOP_SENSORS=ON` and a matching prepared Qt Sensors module to add the sensor checks: `platform.reader-orientation` then keeps the production debounce and checks sensor delivery behind the PIN screen, and `lifecycle.sleepRequestFreezesOrientationBeforeHandoff` and `lifecycle.closePublishesHeldOrientationBeforeBackgroundPaint` are registered. These sensor checks stay outside the fast lane.

## Focused runs and captures

`desktop.viewportChanges` also checks that a page turn does not read the page's text for the transition placeholder and that a rotation does. The gallery's transition scene queues the engine hold after the rotation, so the sample arrives and the reflow waits.

Build `kobalt-alternate-layout-tests`, then run `ctest --preset mac -R '^engine.alternate-layout$'` for the alternate-layout check alone. Set `KOBALT_VIEWPORT_VIDEO` to an absolute output directory when you run `desktop.viewportChanges` to capture timestamped PNG frames of the headless desktop window. The recording uses real render timing, and the held-worker check runs after the recording ends.

## Touch rotation

`model.touch-rotation` compiles the production Qt5 input adapter against native Qt, without rebuilding the reader. Its device guard rejects the Clara HD, including firmware 4.38.23828 from [discussion #48](https://github.com/nicoverbruggen/kobalt/discussions/48), and keeps the correction for the Clara BW and Clara Colour. Unaffected devices keep the native transform in all four orientations. The check does not exercise a physical touch controller.

## Android emulator

Check portrait and landscape with and without a camera cutout. Rotate with Settings and a choice dialog open, then return to portrait. The reader must still fill its canvas in physical pixels after Qt delivers its screen-size events. Compare the Library and reader side insets after you close a book.

On Android 9 or 10, open Type Tester through the document picker. The loading screen must give way to a rendered page and log `Kobalt frame ready`. Turn a page, return to the Library with Back, and reopen the book. Repeat in landscape and at a tablet size. Legacy fullscreen flags must not retain stable system-bar insets while the opening handoff waits for hidden bars. Check a current Android version too.

## Limits

These controlled checks do not establish physical Kobo or Android behavior, device latency, storage behavior or e-ink refresh quality.
