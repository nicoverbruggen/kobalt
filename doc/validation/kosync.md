# KOSync checks

Label `kosync`. Build `mac-tests`, then run `ctest --preset mac -L kosync` from `platforms/desktop/`. `ctest --preset mac -N -L kosync` lists its tests.

These checks cover KOSync accounts, automatic and manual sync, sync moments and intervals, Wi-Fi connection and release, the Wi-Fi power saver, the Wi-Fi status indicator, the sync client and the KOSync settings controls. They use a controlled network transport, stalled loopback servers and substituted native Wi-Fi calls. The widget keyboard in the account editors is covered in [Widget keyboard](keyboard.md).

| Test | Checks |
| --- | --- |
| `model.kosync` | The sync client: document identity, accounts, schedule, responses, retries and hostname lookups |
| `component.kosync-ui` | The KOSync Settings tab, the Automatic sync sheet, the password editor and the sync notice |
| `platform.native-sync-network` | The Kobo connection controller, Wi-Fi release, the Kobo side of the Wi-Fi power saver and the hold on Nickel's background connections |
| `lifecycle.kosyncAccountGating` | No sync work without a usable account, and the toolbar sync button |
| `lifecycle.kosyncBackgroundWifi` | Periodic syncs and their silent Wi-Fi connection |
| `lifecycle.kosyncWifiRelease` | Which syncs release their Wi-Fi request when they end |
| `lifecycle.kosyncMoments` | Each sync moment switched off |
| `lifecycle.kosyncNetwork` | The wait for a connection before HTTP |
| `lifecycle.kosyncOpening`, `lifecycle.kosyncFinal` | Opening, close and sleep syncs against a stalled server |
| `lifecycle.kosyncSleepSavesSyncTime` | Sleep saves the sync time, also when the sleep sync ends after the reader sleeps |
| `lifecycle.kosyncFailure` | Failed progress reads and uploads retain the specific error, fit its guidance and keep local progress |
| `lifecycle.kosyncManualToolbar`, `lifecycle.kosyncManualConnection` | Manual sync and its Syncing notice |
| `lifecycle.wifiPowerSaver`, `lifecycle.wifiPowerSaverAtSleep` | When the reader asks the host to turn Wi-Fi off |
| `lifecycle.wifiStatus` | The Wi-Fi indicator in the status strip |

`session.reader-session`, in [Startup and reading](reading.md), checks the sync digests and position imports of an opened book.

The completed sync notice keeps its dismissal timer when its parent hides and returns. An explicit dismissal still cancels the timer. `component.kosync-ui` checks both cases.

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
