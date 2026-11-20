# KOSync checks

`desktop.settingsZenPage` opens Automatic sync from Zen before the Sync settings page has been visited. A signed-in account can enable automatic sync and change its schedule. It forces a schedule save to fail, checks that the switch returns to its saved position, and retries. It reads back the stored choices and verifies that the account controls stay unbuilt.

`desktop.fastKosyncChoice` opens a synced passage choice for another chapter while reading in Fast mode. It checks the direction, chapter name and whole-book page graph, and that the current position stays on screen.

Label `kosync`. Build `mac-tests`, then run `ctest --preset mac -L kosync` from `platforms/desktop/`. `ctest --preset mac -N -L kosync` lists its tests.

These checks cover KOSync accounts, automatic and manual sync, sync moments and intervals, Wi-Fi connection and release, the Wi-Fi power saver, the Wi-Fi status indicator, the sync client and the KOSync settings controls. They use a controlled network transport, stalled loopback servers and substituted native Wi-Fi calls. The widget keyboard in the account editors is covered in [Widget keyboard](keyboard.md).

| Test | Checks |
| --- | --- |
| `model.kosync` | The sync client: document identity, accounts, schedule, the opt-in document metadata, responses, retries and hostname lookups |
| `model.sync-coordinator` | Book-scoped attempts, late connection answers, hidden readiness, final-upload handoffs and the reader's document metadata in uploads, without a reader window |
| `desktop.syncChoiceView` | The choice's scales and their thresholds, the jump label, chapter starts and parents in nested contents, and the sync time text, with a long synthetic book |
| `desktop.kosyncChoice` | The real asynchronous forward, backward and longer forward choice: titles, actions and focus, the scale, view height, visible bounds and card fit across gallery viewports and text scales |
| `component.kosync-ui` | The KOSync Settings tab with the Send document metadata checkbox, the Automatic sync sheet, the password editor, late editor callbacks and the sync notice |
| `platform.native-sync-network` | The Kobo connection controller, Wi-Fi release, the Kobo side of the Wi-Fi power saver and the hold on Nickel's background connections |
| `lifecycle.kosyncAccountGating` | No sync work without a usable account, and the toolbar sync button |
| `lifecycle.kosyncSignInValidation` | Missing password, retries with retained drafts after a failed sign-in, unchanged account edits, and clearing the draft on sign-in and sign-out |
| `lifecycle.kosyncPasswordSleep` | Sleep dismisses the editor and clears its draft before wake |
| `lifecycle.kosyncBackgroundWifi` | Periodic syncs and their silent Wi-Fi connection |
| `lifecycle.kosyncWifiRelease` | Which syncs release their Wi-Fi request when they end |
| `lifecycle.kosyncMoments` | Each sync moment switched off |
| `lifecycle.kosyncNetwork` | The wait for a connection before HTTP |
| `lifecycle.kosyncOpening`, `lifecycle.kosyncFinal` | Opening, close and sleep syncs against a stalled server |
| `lifecycle.kosyncSleepSavesSyncTime` | Sleep saves the sync time, also when the sleep sync ends after the reader sleeps |
| `lifecycle.kosyncMissingProgress` | First uploads after absent progress, with no upload on route, authentication or server errors |
| `lifecycle.kosyncFailure` | Failed progress reads and uploads retain the specific error, fit its guidance and keep local progress |
| `lifecycle.kosyncManualToolbar`, `lifecycle.kosyncManualConnection` | Manual sync, its Syncing notice, and cancellation of the active request and deadline |
| `lifecycle.kosyncDialogResume` | A successful response waits under a native dialog, then resumes without another fetch when the host releases input |
| `lifecycle.wifiPowerSaver`, `lifecycle.wifiPowerSaverAtSleep` | When the reader asks the host to turn Wi-Fi off |
| `lifecycle.wifiStatus` | The Wi-Fi indicator in the status strip |

`session.reader-session`, in [Startup and reading](reading.md), checks the sync digests and position imports of an opened book.

The completed sync notice keeps its dismissal timer when its parent hides and returns. An explicit dismissal still cancels the timer. `component.kosync-ui` checks both cases.

The Kobo presentation reports resumed input after its last native dialog closes or is destroyed. It queues the notification until native restoration finishes and suppresses it if the reader hides, another dialog opens or the presentation is destroyed. `platform.native-dialog-overlay` checks dismissal, overlapping dialogs, destruction and teardown. The reader uses this notification to resume a deferred KOSync response.

The interval clock uses one origin for each book opening. Failed and backward host samples freeze elapsed time until that source recovers. If the source is unavailable at opening, the fallback remains selected for that opening. The host chooses suspend semantics: Kobo includes kernel standby. The moment checks cover failure and recovery without an early periodic sync.

## Progress API contract

The client uses the KOReader v1 API with HTTP or HTTPS, `Accept: application/vnd.koreader.v1+json`, `x-auth-user` and `x-auth-key`. The key is the MD5 of the password. A configured path prefix applies to each endpoint. Requests do not follow redirects or disable TLS checks. `model.kosync` checks HTTP account persistence, prefixed authentication, progress reads and uploads, plus a real loopback HTTP sign-in and redirect rejection.

| Request | Successful response |
| --- | --- |
| `GET /users/auth` | HTTP 200 with `authorized: "OK"`, or Grimmory's single `username` field matching the requested username |
| `GET /syncs/progress/:document` | HTTP 200 with a progress object, or an absent-progress response below |
| `PUT /syncs/progress` | HTTP 200 with an object whose `document` matches the uploaded digest, or Grimmory's exact `{"status":"progress updated"}` object |

A progress object contains a nonempty string `progress` of at most 4096 characters, a numeric `percentage` from 0 to 1 and a string `device`. Optional `device_id` must be a string. Optional `timestamp` must be null or a finite number from 0 to 10^12. Null means the timestamp is unavailable. The client keeps the position string unchanged. Uploads send `document`, `progress`, `percentage`, `device` and `device_id`. With Send document metadata on, they also send `metadata` with the string fields `filename`, `title` and `authors`, as KOReader does since [koreader/koreader#15306](https://github.com/koreader/koreader/pull/15306). Empty fields are left out. No KOSync request reports whether a server stores this object.

An absent-progress response is HTTP 200 with `{}` or `[]`, or HTTP 404 with the exact JSON object `{"status":"not found"}`. Grimmory's HTTP 404 object is also accepted when it contains only numeric `status: 404`, `message: "No progress found for user and book"` and a nonempty string `timestamp`. Whitespace does not matter. These 404 forms are accepted only for a progress GET, with no transport error other than Qt's `ContentNotFoundError`. They must fit the 16 KiB response limit and finish before the deadline. Extra fields, other JSON shapes, malformed JSON, HTML, empty bodies and other HTTP errors remain failures. The same object cannot authenticate an account or acknowledge an upload. Grimmory's missing-book and missing-route errors remain failures.

Grimmory returns the username after successful authentication and a status message after saving progress. KOReader checks HTTP 200 for these operations without requiring the reference server's success fields. Kobalt recognizes both response formats. It rejects a different username, contradictory authorization and a mismatched document in an upload acknowledgement. The comparison uses Grimmory's [authentication and progress service](https://github.com/grimmory-tools/grimmory/blob/402e89b4452f8e2b17ab95f16c1621c003516cd2/backend/src/main/java/org/booklore/service/koreader/KoreaderService.java) and [controller](https://github.com/grimmory-tools/grimmory/blob/402e89b4452f8e2b17ab95f16c1621c003516cd2/backend/src/main/java/org/booklore/controller/KoreaderController.java). The regression checks replay these response formats through the production client; they do not connect to a deployed Grimmory server.

An existing Grimmory book-progress row can have no KOSync state. HTTP 200 with null `progress`, `percentage` and `timestamp`, a document digest and string device fields also means absent progress. A percentage or timestamp without a usable position remains an error. Kobalt does not overwrite that remote state automatically.

The reference server returns an empty successful response for absent progress. The public Techy-Notes server source returns the structured 404. KOReader's API description lists HTTP 200 and 401 for progress reads, and its client treats 404 as a failed pull. Its upload path runs independently of the pull, so it can still create progress. Kobalt checks remote progress before each upload to protect remote changes. It therefore needs to recognize the structured missing-record response explicitly. Conflict choices and retry budgets stay the same.

Sources, pinned for this comparison: [reference server](https://github.com/koreader/koreader-sync-server/blob/46ef6b84a3930b3f0265efac74899f2256c158f1/app/controllers/1/syncs_controller.lua), [KOReader API description](https://github.com/koreader/koreader/blob/9040116701c888889bd35233b23b83283be66745/plugins/kosync.koplugin/api.json), [KOReader upload and pull](https://github.com/koreader/koreader/blob/9040116701c888889bd35233b23b83283be66745/plugins/kosync.koplugin/main.lua), and [Techy-Notes server source](https://github.com/jadehawk/koreader-sync/blob/3d8948333ff6379b082bf86ca4c89aac943e5eb1/src/index.tsx). These sources establish implementation behavior, not the version deployed by a public service. KoSync.eu's missing-record response has not been verified.

`model.kosync` checks absent-progress responses, first upload, malformed and ambiguous 404 responses, endpoint isolation and transport failures. `lifecycle.kosyncMissingProgress` runs Sync now through the production reader and verifies that absent progress permits an upload while route, authentication and server errors do not. Existing reconciliation checks retain remote conflict protection.

The choice check supplies controlled HTTP replies to the production reader. It checks the view's height as well as its visibility: a widget can be fully visible while its content is cut inside it. The view is added to a card that is already shown, so the check also catches a view that the card measured before it was shown. The gallery uses the same case for `kosync-choice-forward` and `kosync-choice-backward`, five pages apart, and `kosync-choice-far`, seventeen pages ahead on the track. The backward scene uses a typical device name; the other two use a long one that wraps. Run `KOBALT_GALLERY_CASE=kosyncChoice` with the screenshot preset for these scenes before a full gallery has indexed them. The Trevelyan fixture is short, so the gallery does not show the whole-book scale; `desktop.syncChoiceView` covers it with a synthetic book of 300 chapters in 15 Books.

## Captures

Set `KOBALT_KOSYNC_MANUAL_CAPTURE` to an absolute PNG path to capture the Syncing notice in `lifecycle.kosyncManualConnection`. Set `KOBALT_KOSYNC_CHOICE_CAPTURE` to an absolute output prefix to capture the forward and backward progress-choice dialogs in the [real server check](#real-server-check).

## Diagnostics

Kobo always writes bounded sync diagnostics. Read `.adds/kobalt/diagnostics/events.jsonl` after you reproduce a failure. Sync events record native connection preparation and completion, radio state and signal strength, request error codes, HTTP status and timing, and deadline and TLS stages. On Qt5, lookup events separate DNS resolution from HTTP and TLS, and `lookup_attempt` tells the two lookup attempts apart. A reader-wait event records once why a successful response must wait for the reader. Sync events exclude URLs, authentication, response bodies and book IDs.

`kosync-failure` adds a fixed `code`, a safe `phase` (`sign-in`, `read-position` or `upload-position`) and plain-language `guidance`. Transport failures include the numeric Qt `network_error`; HTTP failures include `http_status`. Request timing and attempt fields describe the existing retry budget. `kosync-retry-scheduled` records the next delay. Lookup completion includes `timed_out`, so a lookup deadline is separate from a resolver error. A request aborted by its deadline reports `kosync.timeout_error`; an explicit user cancellation suppresses the result callback and produces no failure event. A connection interrupted outside that cancellation path reports `kosync.interrupted`.

Examples of safe failure fields are `phase=sign-in code=kosync.auth_error http_status=401`, `phase=read-position code=kosync.dns_error network_error=3` and `phase=upload-position code=kosync.route_error http_status=404`. These are categories, not proof of a particular server implementation. Authentication guidance includes checking account existence and KOSync x-auth compatibility. Route, redirect and response-format guidance asks the user to check the endpoint and path prefix. TLS guidance keeps verification enabled and asks the user to check the certificate, hostname and device date. Server errors ask for a later retry or administrator investigation. TLS events keep at most 16 numeric error codes and report the omitted count. No response text, headers, redirect targets, certificate text or Qt error strings enter these fields.

For a local diagnostic test, use the matching `qt5-diagnostic` or `qt6-diagnostic` package and confirm its distinct version in About. Reproduce with Sign in or Sync now, note the time, close the book and connect USB. Collect `events.jsonl`, its rotated files and `session-context.json` from `.adds/kobalt/diagnostics/`. Collect promptly because the log is bounded. Share the diagnostics folder only, not the account or profile database. `model.kosync` uses controlled failures with hostile credentials, response bodies, cookies, error text and redirect targets to check categories, guidance and redaction. The lifecycle checks verify that deadline messages reach the reader and that retries, cancellation and saved progress keep their existing behavior.

## Qt5 hostname lookup

On Qt5, each request resolves its hostname on a separate worker before HTTP. This bypasses Qt5's shared asynchronous lookup queue and fills Qt's hostname cache. The request keeps its original URL and TLS verification. The first lookup has a one-second limit. A failed lookup gets one more attempt within the remaining request budget. This recovery is separate from HTTP retries and never replays an upload after a transport failure. libc cannot cancel a running lookup, so at most two lookups can finish in the background after cancellation. Close and sleep do not wait for them. Application shutdown joins the workers before Qt's resolver cache is destroyed. The operating system still controls how long a running lookup takes.

## Real server check

Build `mac-tests`, then run `bash tests/support/run-kosync-server.sh --pull` from the repository root. Later runs can omit `--pull`. This optional check needs Docker, curl and OpenSSL. The runner starts the pinned official KOSync image on a random localhost port with a temporary CA and server certificate. It runs `model.kosync` in server mode, then the `https-*` rows of `lifecycle.kosyncFinal`. The close and sleep rows check that the sync time is on storage afterwards. Together they check authentication, progress exchange between two clients, conflict detection and the progress-choice dialog, final syncs at close and sleep in both matching modes, manual sync, and a wake sync through a loopback relay that refuses connections for the first second after wake. Only the test processes trust the temporary CA. The runner removes the container, accounts, Redis data, certificates and test profiles when it ends. The ordinary native suite stays offline.

## Limits

These checks do not exercise physical radio drivers, real firmware dialogs or Nickel. They do not establish exact position compatibility with KOReader or physical Kobo and Android behavior. That needs a separate check with the same EPUB on both readers.

`kosync-network-unavailable` records a fixed `kosync.no_connection` code, guidance and a `preparation`, `periodic` or `final` stage when no connection is available. This event does not imply an HTTP request was sent. A connection preparation deadline uses `kosync.connection_timeout` in `kosync-deadline`. The manual connection, network, moments and background Wi-Fi lifecycle checks cover these messages and the absence of HTTP traffic.

The sync coordinator owns request cancellation, deadlines, waiting responses, connection release and the final-upload handoff for one opened book. The reader supplies progress and readiness values and retains scheduling, lifecycle eligibility and presentation. A final attempt replaces an older progress read and waits for any older upload under the same two-second deadline. Cancelling or destroying the coordinator drops late connection answers. A repeated connection answer cannot restart the read, and a second attempt is refused while one is active. Observer callbacks may cancel or destroy the coordinator; no further request or final callback reaches that owner. A reader refresh resumes a waiting response using the settled local position without repeating the fetch. A busy visible reader also has a one-second fallback check. A hidden reader waits for notification.

## Android pause and retries

Android's Gradle unit test `org.kobalt.android.KosyncWorkTest` checks retained remote comparisons, changed remote positions from this and other devices, consecutive worker uploads, absent progress, malformed responses and exclusive foreground/worker ownership. Run `./gradlew testDebugUnitTest` in the generated Android build after building the APK. These checks do not establish Android scheduler timing, process-death recovery or OEM battery behavior. WorkManager uses a connected-network constraint and exponential backoff; Android 12 and newer use expedited work, with ordinary scheduling when its quota is exhausted. Android 9–11 use ordinary scheduling without a foreground service or notification. The Java checks inspect the actual work requests for both API ranges. They also check the same narrowly accepted Grimmory missing-progress rows and upload acknowledgements as the foreground client, including rejection of ambiguous responses.

A headless Android 36 ARM64 runtime check uses the production debug APK, the checked-in EPUB and a stateful local HTTP fixture. It checks pause and screen-lock uploads, a later changed position after a previous worker upload, offline work followed by automatic upload after connectivity returns, and a queued retry after background process death and a real emulator reboot. Recovery must complete without opening a reader activity. A newer remote position must permit a GET but no PUT. The boot ID, process IDs, WorkManager work ID/state and server traffic provide the evidence. This does not cover OEM battery policies, physical radios, a PIN-protected user before unlock, force-stop, or other Android API levels.

The runtime checks caught two gaps: a later pause did not remember the preceding worker upload, and a queued retry could remain stranded after reboot while WorkManager's optional receiver was disabled. Each new snapshot now captures its acknowledged worker position, and the permanently enabled `KosyncBootReceiver` initializes WorkManager after boot without starting Qt. Duplicate Qt pause-state callbacks retain the existing job instead of cancelling an active attempt.
