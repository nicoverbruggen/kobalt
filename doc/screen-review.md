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
4. When the reviewer is done, read the feedback:

   ```sh
   node platforms/review/review.mjs notes
   ```

   The command lists each screen that needs changes and each screen with a note. It gives the path of the phone image and of the reference image, the general note and each pin with its target image and position in that image’s pixels.
5. Fix the screens, capture them again and create the next round. The server does not need a restart. Reload the page to see the new round.

## Rounds

A round is a set of screens with their feedback. Each `round` command creates the next round in `build/review/rounds/`. Only the latest round takes feedback. Earlier rounds open read only.

A new round compares each phone image with the previous round:

- An unchanged image keeps its verdict, its note and its pins. The page says which round the verdict came from.
- A changed image starts without a verdict. The page shows the earlier verdict and notes below the note box, and the Previous round button shows the earlier image with its pins.
- A screen that is not in the previous round is new.

A scene-filtered capture contains only some screens. The round keeps the other screens, with their verdicts, from the previous round. Add `--fresh` to start a round with only the captured screens.

The round command copies each image once into `build/review/images/`, named by its SHA-256 checksum. Rounds share unchanged images. Delete `build/review/` to start again from round 1.

## Pairs and themes

By default, the review shows `phone` screens next to `six-inch` screens and `phone-landscape` screens next to `six-inch-landscape` screens. Without phone captures, it shows `six-inch` screens without a reference. To choose other devices, give `--pair` once for each pair, for example `--pair seven-inch=six-inch`. A pair without `=` shows the device without a reference.

A phone screen can have more pages than its reference. A later page is shown next to the last page of the reference.

The review uses the light theme. Add `--theme dark` or `--theme large` for the other gallery variants.

## The page

The page shows one screen at a time. Both images use the same scale, so their physical sizes compare directly.

| Key | Action |
| --- | --- |
| Left and right arrows | Previous and next screen in the filter, unless typing in a text box |
| Space | Mark the current screen seen and move to the next screen |
| R | Open the note box to request changes |
| P | Show the previous round's image, when the screen changed |
| Esc | Leave the note box |

There are no approve or reject buttons. Use Add note or press R for a general note.

Click either the phone image or the Kobo reference image to pin a note at that place. A note or pin marks the screen Needs changes automatically. After typing, press Tab or Esc to leave the input, then Space to move on. Each pin has its own note box below the general note. Remove a pin with its Remove button.

Filter the screens by gallery group, orientation and status. The status filter shows unseen screens, seen screens, screens that need changes, screens changed since the last round or screens with notes. Seen and Needs changes are separate flags: an annotated screen is also seen. Moving with either arrow or choosing another screen records the current screen as seen. The current screen stays visible when its status changes. The next arrow press moves to the next screen that matches. The sidebar shows the filtered screens with their verdicts. Its button at the bottom opens it or folds it into a narrow strip. The screen title shows the position in the filtered screens with Previous and Next buttons. The page uses the same light colours, type, header, controls and favicon as the screenshot gallery.

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
