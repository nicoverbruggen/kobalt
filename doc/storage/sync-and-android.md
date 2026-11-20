# Sync and Android storage

## KOSync account

`kosync.json` holds the optional KOSync account, apart from book data and caches. Its schema is 1.

- It holds the server URL (HTTP or HTTPS), the username, the MD5 key derived from the password, a persistent device ID and the automatic-sync preference.
- The password itself is never stored. The key still gives access to the account.
- A missing file means signed out. An empty server URL is valid only when signed out. Signing out clears the key.
- Writes are atomic and ask for owner-only permissions where the file system allows it. Kobo's FAT profile stays readable over USB. This is a profile file, not a system credential store.
- Invalid or unsupported files block writes and stay as they are. Profile preparation moves invalid JSON and unsupported schemas into `backups/`.
- Copying the file also copies the device ID. Sign in on each device instead.

Optional fields keep schema 1 and survive account edits.

| Field | Values | Default | Notes |
| --- | --- | --- | --- |
| `last_sync` | UTC ISO 8601 | none | See [Last sync time](#last-sync-time). |
| `device_name` | text, at most 128 UTF-16 code units, no control characters | platform name | Renaming keeps the device ID and `last_sync`. |
| `match_method` | `binary`, `filename` | `binary` | An unsupported value blocks writes. A change clears `last_sync` and keeps the account and device ID. |
| `send_metadata` | Boolean | `false` | Adds the file name, title and authors to progress uploads. |
| `manual_explanation` | Boolean | `true` | Set to false after the user confirms the toolbar explanation. |
| `sync_interval_minutes` | `0`, `1`, `5`, `15`, `30`, `60`, `-1` | `15` | How often an open book syncs a changed position. `0` is every page. `-1` is never. |
| `wake_wifi` | Boolean | `false` | Lets periodic syncs turn on Wi-Fi. |
| `sync_on_open`, `sync_on_sleep_wake`, `sync_on_close` | Boolean | `true` | Turn each sync moment on or off. |

- Invalid values for every field except `match_method` use the default and do not block writes.
- Accounts saved before `sync_interval_minutes` existed sync every 15 minutes while reading. Before, they synced on every page.

### Last sync time

- `last_sync` is the last successful progress exchange for this account on this device. Failed and cancelled exchanges do not change it.
- Changing the server, username or `match_method` clears it.
- A successful exchange updates it in memory. Sleep, closing the book (also for USB and power-off) and account or settings edits save it.
- Sleep writes nothing if no exchange succeeded since the last write. A sync that ends while the device sleeps queues its own write and does not wait.
- A crash can lose the time. It is informational only.
- A delayed write keeps newer account settings and never brings back a removed or changed account.

## Android preferences

Android keeps these in private SharedPreferences. They change no book, settings or database version. Android removes them when the app is uninstalled.

- `book-filenames` keeps the source file name of imported and cached books. KOSync `filename` matching uses it instead of the internal hashed name. Books imported before this existed must be imported again. A missing name turns off filename matching for that book only.
- `screen-brightness` holds an integer `level` from 0 to 100. Missing, invalid or negative values follow the system. Completed gestures, slider changes and Follow system are written on a separate worker. Cancelled gestures are not saved. It controls normal Android brightness, not the BOOX frontlight.

### Android resource folders

- `user-folders` SharedPreferences hold the chosen document-tree URIs under `dictionaries`, `fonts` and `custom-pins`, each with a `-label` key for its display name. A missing key means no folder.
- Read access persists through Android's document provider. Clearing a resource folder never removes a Library folder's access.
- `user-folders/` in app data holds disposable working copies, one folder per kind, apart from downloaded dictionaries and saved data.
- A worker scan records provider identity, relative names, sizes and dates in `.inventory`. A changed snapshot replaces the folder through `-pending` and `-previous` siblings.
- A failed read keeps the previous snapshot. An interrupted swap is recovered at the next refresh when the active folder is missing.
- Dictionary offset indexes and `.ifo.kobalt-ready` markers exist only in these copies. Dictionary IDs are `folder:` and the relative base name.
- Changing a folder keeps saved choices. Choices that are no longer available use the normal automatic fallback.

### Android deferred KOSync

- WorkManager keeps a small pause snapshot in its private database: the document digest, local position and percentage, optional metadata, the device name, the last remote position and time seen, the last uploaded position, and the account identity and policy.
- The snapshot holds a SHA-256 of the account key, never the key itself.
- The worker reads the private `kosync.json` and stops when the credentials or policy changed. A changed `last_sync` alone is allowed.
- Returning to the app cancels pending work through a generation number in private SharedPreferences.
- A successful deferred sync stores a time and the acknowledged position in private preferences, per account and document.
- Each new pause copies that acknowledged position into its job. A retry never uses a later acknowledged position to overwrite newer server progress.
