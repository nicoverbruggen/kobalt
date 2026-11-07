# Settings and controls checks

`desktop.settingsStandby` checks supported and unsupported hosts, the default state, confirmation, cancellation, unanswered-warning dismissal, persistence after reopening and re-enabling standby. The gallery includes the System settings pages and the standby confirmation.

Label `settings`. Build `mac-tests`, then run `ctest --preset mac -L settings` from `platforms/desktop/`. `ctest --preset mac -N -L settings` lists its tests.

These checks cover Settings drafts, apply and persistence, appearance previews and their failures, the Light shortcut for dark mode, shared controls, borders, themes and the acknowledgment and license pages. `component.ui` renders the shared controls in pixels in both themes and at 212 and 300 PPI. Its border checks use fractional scales and pixel offsets, because rounding a strip width separately from its boundaries gives uneven borders. The `lifecycle.appearance*` checks drive previews and their failures in the production reader.

Dark-mode saves from Settings and from the Light shortcut run on the profile writer thread. These headless checks do not establish physical touch delivery, e-ink refresh quality or device storage latency.

Centered choice and confirmation dialogs use the shared `PopupCard` heading for title and description spacing. Its plain text wraps into balanced lines using the same font measurements as painting. Explicit paragraph breaks stay intact. Long prose keeps ordinary wrapping. The component checks cover orphan-line avoidance, line width, paragraph preservation and text without word spaces. Described option rows reserve extra vertical space when their supporting text determines the row height; rows without descriptions retain their size.
