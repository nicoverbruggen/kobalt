# Dictionary checks

Label `dictionary`. Build `mac-tests`, then run `ctest --preset mac -L dictionary` from `platforms/desktop/`. `ctest --preset mac -N -L dictionary` lists its tests.

These checks cover word lookup, dictionary editing, dictionary book search, downloaded dictionaries, the dictionary chooser and dictionary rendering. `model.dictionary-store` uses controlled network replies and needs no live download. `component.ui`, in [Settings and controls](settings.md), checks the dictionary rows, the chooser, the editor, definition rendering and the keyboard refresh policy.

These checks use a controlled keyboard. They do not establish physical Kobo or Android keyboard and touch behavior.
