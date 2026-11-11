# Screen review

Screen review is a local web page for reviewing UI changes one screen at a time. It shows each phone screen next to the 6-inch e-reader screen with the same orientation. The reviewer moves through screens and pins notes to places in either image. Moving on marks the screen as seen. A seen screen without annotations is approved; notes or pins mean Needs changes. The developer reads the notes, fixes the screens and creates the next round.

Screen review uses the captures of the screenshot gallery. It does not replace the gallery and does not change how the gallery captures screens. The tool is `platforms/review/review.mjs`. It requires Node.js and no packages.

## Workflow

Run these commands from the repository root.

1. Capture the screens. Run `cmake --build --preset mac-screenshots` from `platforms/desktop/`. To capture only some screens, set `KOBALT_GALLERY_SCENE` as described in [Building](building.md#headless-tests-and-screenshots).
2. Create a round from the captures:

   ```sh
   node platforms/review/review.mjs round
   ```

   For a scene-filtered capture, add `--source build/screenshots/mac-scenes`.
3. Start the server and give the reviewer the link:

   ```sh
   node platforms/review/review.mjs serve
   ```

   The page is at `http://localhost:8765/`. Use `--port` to change the port. The server accepts connections from this computer only.
4. When every screen is seen, the reviewer presses Finish review, the primary button at the right end of the toolbar. Read the feedback:

   ```sh
   node platforms/review/review.mjs notes
   ```

   The command lists each screen that needs changes and each screen with a note. It gives the path of the phone image and of the reference image, the general note and each pin with its target image and position in that image’s pixels. Its second line says whether the reviewer finished the round and when. A change after Finish review reopens the round.
5. Fix the screens, capture them again and create the next round. A note that needs no change, such as a question, gets a reply instead:

   ```sh
   node platforms/review/review.mjs reply phone/zen-menu/1 "No. The paper covers only the footer strip."
   ```

   The reply waits in `build/review/replies.json` for the next round. `notes` lists the screens that have one. The server does not need a restart. The open page notices the new round and offers it. After a change to the review tool itself, restart the server.

## Queue screens for the reviewer

A round can hold many screens that need no attention. Queue the ones the reviewer should look at, each with an optional question:

```sh
node platforms/review/review.mjs ask text-presets "Should the value sit on the heading's baseline?"
```

SCREEN is a screen name, which queues all its pages and orientations, or one item id such as `phone/text-presets/1`. Asking again replaces the question. `ask --clear` empties the queue. The queue belongs to the latest round, in `rounds/<round>/queue.json`, and a new round starts without one.

The page marks a queued screen Queued for you and shows its question above the notes. Its status filter has Queued for you, and the first time the reviewer opens a round with a queue, the page selects that filter. After that, the reviewer's own filter choice stays. The reviewer answers in the general note or with pins, as for any screen. `notes` lists each queued screen with its question, whether it was seen and the reviewer's notes.

## Rounds

A round is a set of screens with their feedback. Each `round` command creates the next round in `build/review/rounds/`. Only the latest round takes feedback. Earlier rounds open read only. While the page is open and visible, it checks every 5 seconds for a newer round. A round older than the newest one shows a banner below the header with an Open button for the newest round, which keeps the current screen. When the open round is superseded, the page also turns read only at once, so notes do not fail to save.

A new round compares each phone image with the previous round:

- An unchanged image keeps its verdict, its note and its pins. The page says which round the verdict came from. Changing the note or a pin makes them the current round's own.
- A screen with a reply starts unseen and without notes. The page shows the reviewer's notes in grey and the reply below them, under Answered. Moving on marks it as seen, and a new note asks again. An answer carries over to later rounds until the reviewer has seen it.
- A changed image starts without a verdict. The page shows the earlier verdict and notes at the bottom of the pinned notes, and comparing with the previous round shows the earlier images with their pins.
- A screen that is not in the previous round is new.

A scene-filtered capture contains only some screens. The round keeps the other screens, with their verdicts, from the previous round. Add `--fresh` to start a round with only the captured screens.

The round command copies each image once into `build/review/images/`, named by its SHA-256 checksum. Rounds share unchanged images. Delete `build/review/` to start again from round 1.

## Before and after

A review can compare each screen with the same screen before the feature, as well as with the previous round. Capture the screens of the unmodified base first, before you change them:

```sh
cmake -DCASES="screens;standbySettings" -P platforms/review/baseline.cmake
```

`CASES` lists the gallery test functions to capture at the base, as `KOBALT_GALLERY_CASE` does. `SCENES` selects single scenes instead, but only after a full gallery run in that export. The script exports the commit where the branch left `develop` with `git archive` into `build/review-base/<commit>/`, without changing Git state. It captures them there with the shared dependency cache and records their captures as the Before set in `build/review/baseline.json`. Pass `-DBASE=<commit>` for another base and `-DQT=<prefix>` for another Qt. A later run for the same commit reuses its build. A screen that the feature adds or renames has no base capture under its own name. Map it to the base screen it replaces with `MATCH`, for example `-DMATCH="zen-menu=menu;zen-chapters=chapters"`. Remove `build/review-base/` with `platforms/cleanup.cmake` when the feature is done. A base taken from `develop` records that ref; after a rebase onto a newer `develop`, the Compare menu warns that the originals are stale until you capture the base again. The `baseline` command takes the same ref with `--base-ref`.

The underlying command takes any capture folder: `node platforms/review/review.mjs baseline --source DIR`, with `--match SCREEN=BEFORE` and `--commit SHA`. Every round then offers Original in Compare with for each screen with a base image. The original images of both devices carry the base commit in their captions.

## Pairs and themes

By default, the review shows `phone` screens next to `six-inch` screens and `phone-landscape` screens next to `six-inch-landscape` screens. Without phone captures, it shows `six-inch` screens without a reference. To choose other devices, give `--pair` once for each pair, for example `--pair seven-inch=six-inch`. A pair without `=` shows the device without a reference.

A phone screen can have more pages than its reference. A later page is shown next to the last page of the reference.

The review uses the light theme. Add `--theme dark` or `--theme large` for the other gallery variants.

## The page

The page shows one screen at a time. Both images use the same scale, so their physical sizes compare directly. They fill the window height. On windows wider than 1100 pixels, the notes sit in a sidebar on the right, the counterpart of the screen list on the left, so a note does not move or shrink the images. Its header shows where the screen belongs and the counts. Pinned notes fill the middle and scroll on their own. For a changed screen, the previous round's verdict and notes sit at the bottom of that area in small grey text, under a line such as Round 9 · needs changes · 2 notes; click that line to fold them. The note for the screen stays at its bottom and grows with its text, with a Shortcuts link that opens a list of all keys; ? opens it too. Its button at the bottom, or N, folds it into a narrow strip and opens it again; the page remembers the choice. Pinning a note or pressing R opens it. On narrower windows the notes sit below the images. The header holds the screen title, its position, Previous and Next and the filters in one row. Below 1600 pixels wide, the header shows only the logo before the title.

A status bar under the images, between the two sidebars, controls what the stage shows. It holds two pop-up menus, Devices and Compare, then Hold to compare and the zoom. Each menu button names its current settings; Esc or a click elsewhere closes an open menu. In Devices, a checkbox for each image, named after its device, shows or hides it, and the other image takes the space. One image always stays; the page remembers the choice. Bezel is in the same menu. The stage always shows the latest images. In the Compare menu, Compare with chooses a baseline: None, Original (the screen before the feature) or the previous round. The choice stays from screen to screen and across reloads; a screen without that baseline shows it greyed out. Hold to compare, or holding C, shows the baseline of every shown device in place of the latest images until you let go. The frames get an accent outline and the captions name the baseline while you hold. A hidden device stays hidden; when the baseline has no image for a shown device, the stage says so. Pins stay on the latest images; holding on the previous round shows that round's pins in grey. Differences, in the same menu or D, tints the latest pixels that differ from the baseline in red, for images of the same device and size. Side by side, in the same menu or S, puts the baseline before the latest image of each shown device instead, all at one scale; the page remembers the choice. Hold to compare is off while side by side is on. Comparing with the previous round side by side shows its pins in grey on its own images. Bezel draws a device frame around each image, with the same Kobo Clara, Kobo Libra and phone geometry as the screenshot gallery's viewer; the page remembers the choice, and B toggles it. The frame surrounds the image, so pins keep their place. The zoom moves in steps: 50%, 67%, 75%, 90%, 100%, 125%, 150% and 200%, with Fit, the size that fits the window, in its place among them. Use the slider, its − and + buttons, the − and = keys, or scroll while holding Alt (Option on Mac). At 100%, one image pixel is one CSS pixel. Above Fit, the images scroll. Zooming keeps the middle of the view in place, or the spot under the pointer for Alt and scroll. Z switches between Fit and 100%. Pins work the same way at any zoom.

| Key | Action |
| --- | --- |
| Left and right arrows | Previous and next screen in the filter, unless typing in a text box |
| Space | Mark the current screen seen and move to the next screen |
| R | Go to the note for this screen |
| C (hold) | Show the chosen baseline instead of the latest images |
| D | Tint what differs from the baseline |
| ? | Show all keyboard shortcuts |

Letter shortcuts also work while a checkbox, radio button or the zoom slider has focus; those keep only their arrow keys, Space and Enter.
| S | Show the baseline next to the latest images |
| Z | Switch the zoom between Fit and 100% |
| N | Fold or open the notes sidebar |
| B | Show or hide the device bezel |
| − and = | Zoom out or in by one step |
| Alt and scroll | Zoom around the pointer |
| Esc | Leave the note box |

There are no approve or reject buttons. Type in the note box, or press R, for a general note.

Click either the phone image or the Kobo reference image to pin a note at that place. A note or pin marks the screen Needs changes automatically. After typing, press Tab or Esc to leave the input, then Space to move on. Each pin has its own note box below the general note. Remove a pin with its Remove button.

Filter the screens by gallery group, orientation and status. The status filter shows unseen screens, seen screens, screens that need changes, screens changed since the last round, screens with notes or screens that differ from the chosen baseline. In the screen list, ≠ marks the screens that differ from the chosen baseline: an image whose checksum differs, or a device that the baseline lacks. Seen and Needs changes are separate flags: an annotated screen is also seen. Moving with either arrow or choosing another screen records the current screen as seen. The current screen stays visible when its status changes. The next arrow press moves to the next screen that matches. The sidebar shows the filtered screens with their verdicts. Its button at the bottom opens it or folds it into a narrow strip. Drag the inner edge of either sidebar to resize it; the page remembers both widths, and a double-click on the edge restores the default. The folded strip shows a map of the filtered screens: one short tick per screen, with a space between groups. A faint tick is unseen and a grey tick is seen. A screen that needs changes has a longer blue tick, and the current screen has the longest. Click a tick to go to its screen. On a long review the ticks move closer together, so the whole review fits. The header shows the screen title and its position in the filtered screens, with Previous and Next buttons. The side column shows the counts of seen, needs-changes and unseen screens. The page uses the same light colours, type, header, controls and favicon as the screenshot gallery.

The page saves each change at once. Typing saves after a short pause. Opening or reloading the page always selects the latest round, even when the address names an older round. It keeps the screen from the address when that screen exists in the latest round. Use the Round selector to inspect earlier rounds during the session.

## Files

| Path | Content |
| --- | --- |
| `build/review/rounds/NNN/round.json` | The screens of the round, their images, references and status |
| `build/review/rounds/NNN/feedback.json` | Seen flags, derived verdicts, notes and pins, by screen |
| `build/review/images/` | The images of all rounds |

A screen is identified by device, gallery screen and page, for example `phone/library-sort/1`. Pins record `target` as `subject` or `reference`. Pin positions are fractions of that image’s width and height. Older pins without a target refer to the phone image. Older rejected verdicts appear as Needs changes. Existing feedback is read without rewriting it.

## Tool checks

Run `node --test platforms/review/review.test.mjs` from the repository root. The checks use isolated feedback under `build/` and remove it afterward. For browser checks, use a separate test round and server so test notes do not enter the reviewer’s round.
