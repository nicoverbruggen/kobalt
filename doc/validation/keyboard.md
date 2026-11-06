# Widget keyboard checks

Label `keyboard`. Build `mac-tests`, then run `ctest --preset mac -L keyboard` from `platforms/desktop/`. `ctest --preset mac -N -L keyboard` lists its tests.

These checks cover the widget keyboard layouts, Shift and Caps Lock, accents, symbols, placement and lifetime. Desktop uses the widget keyboard. Kobo uses Nickel's keyboard through the native adapter. Android keeps its native keyboard. `component.kosync-ui`, in [KOSync](kosync.md), also types through the real widget keyboard in the account editors.

The [Pewter keyboard checks](kobo.md#native-allocations) exercise both Kobo adapters. They cover language changes queued before dismissal: the keyboard must stay hidden and use the chosen language when explicitly reopened.

The rounded-corner checks inspect the composited keyboard, because a capture of the isolated widget fills the exposed corners with that widget's own palette.

The gallery's six-inch keyboard captures ending in `-300ppi.png` render the screen directly at 1072 × 1448 pixels with 300 PPI metadata, without the desktop frame or preview scaling. Its IBM Plex Mono and Libron captures change only the character keys for comparison. They do not change the keyboard's default font.

These headless checks exercise native Qt widgets. Physical key touch targets, touch timing, e-ink refresh and Android system-keyboard delivery need platform validation.
