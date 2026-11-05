# Kobalt user guide

This guide explains how Kobalt works, what it does with your reading data, how it treats Wi-Fi and battery, and what it cannot do. For the technical details, see [Sync data](sync-data.md) and [Power mechanisms](power-mechanisms.md).

## How Kobalt works on a Kobo

Kobalt is a reader for EPUB books. On a Kobo it runs inside Kobo's own software, which Kobo calls Nickel. Kobo's software stays in charge of everything outside the book: the Home screen, your library, the store, sync, Wi-Fi, sleep and the dictionaries. Kobalt only replaces the screen you read on.

You can make Kobalt your default reader, or open a book with it from the book's long-press menu. When you close the book, you return to Kobo's own screens.

Kobalt keeps its own settings, bookmarks, highlights and notes in the `.adds/kobalt` folder on your Kobo. It also tells Kobo's software where you are in each book, so the Home screen, your library and Kobo's sync show the right progress.

## What Kobalt cannot do

- **Protected books.** Books with any kind of DRM open in Kobo's own reader instead. This includes protected Kobo store books and library loans. Kobalt contains nothing that can remove or work around DRM.
- **Other formats.** PDFs, comics, previews and other formats that are not EPUB also open in Kobo's own reader. You see "Opening in built-in reader" when this happens.
- **Highlights and notes in Kobo's cloud.** Kobalt keeps your bookmarks, highlights and notes on the device. They are not sent to Kobo or to a sync server, and they do not appear in Kobo's own reader.
- **Firmware.** Kobalt needs a supported Kobo software version: 4.38 or later 4.x with the Qt 5 package, or 5.18 or later with the Qt 6 package. Check your version before you install.

## Syncing your progress

Kobalt can sync in two ways:

- **Kobo's own sync** works for books that came through your Kobo account: books from the Kobo store, or from a server that replaces Kobo's, such as BookOrbit or Grimmory. You do not set anything up in Kobalt for this.
- **KOSync** is for your own books. You sign in to a KOSync server in Kobalt's settings. It works on Kobo, Android and the desktop app.

A book uses one of the two, never both.

### Kobo's own sync

When you read a book from your Kobo account, Kobalt saves the same reading information that Kobo's own reader saves:

- where you are in the book
- how far you are, in percent, in the book and in the current chapter
- how much time you have spent reading, and the estimated time left
- whether you are reading the book or have finished it
- your reading sessions: when you started and stopped, how far you got and how many pages you turned

Kobalt saves this on the device when you close the book, when the device goes to sleep, when you connect it to a computer and when you turn it off. It does not save on every page turn, so turning pages stays fast. Your Kobo sends the information to the server the next time it syncs. To sync now, go to the Home screen, tap the sync icon at the top and choose **Sync now**.

If you read the same book on another device and it is further along, Kobalt asks whether you want to go to that position. It never moves without asking.

Servers that replace Kobo's use the information in their own way. BookOrbit shows your reading sessions, with their length and how far you got. Grimmory uses your position and percentage and does not show sessions. If a server shows your progress but no reading sessions, the server may ask your Kobo for privacy consent that your account does not give. That is a setting on the server.

### KOSync

KOSync is a free sync system that KOReader and other readers also use. Kobalt can use it for any book that does not come from your Kobo account.

To set it up, open **Settings → Sync** and sign in with your KOSync server, user name and password. Create the account on your server first; Kobalt does not create accounts. Kobalt does not store your password, only a key made from it.

The **Automatic sync** row on the Sync page opens a sheet where you choose when Kobalt syncs:

- **While you read**: how often Kobalt sends your progress as you read. Choose every page, every minute, every 5, 15 or 30 minutes, every hour, or never. The default is every 15 minutes. Kobalt checks when you turn a page, so a book you leave open does not sync until you read on. If those syncs are on, it also syncs when the device sleeps or you close the book. Syncing more often uses more battery.
- **When you open a book**: Kobalt gets your latest progress. If another device is further along, it asks before moving.
- **When your device sleeps or wakes** (Kobo only): Kobalt sends your progress before sleep if you are online, and gets your latest progress when you wake the device.
- **When you close the book**: Kobalt sends your progress if you are online.

On a Kobo you also choose what happens while you read and Wi-Fi is off:

- **Wait** keeps your progress on the device until Wi-Fi is on again. This uses the least battery.
- **Turn on Wi-Fi** lets Kobalt turn Wi-Fi on, sync, and turn it off again.

The sync when you open a book and the sync after you wake the device can also turn Wi-Fi on. See [Battery and Wi-Fi](#battery-and-wi-fi) for when Kobalt turns it off again.

If Kobalt cannot connect three times in a row, it stops trying until you wake the device or tap **Sync now**. If you turn automatic sync off, a sync button appears in the reading toolbar, and Kobalt syncs only when you tap it.

KOSync receives only what it needs to find your place: an identifier for the book, your position, the percentage, and your device's name and identifier. It does not receive highlights, notes or reading time.

## Battery and Wi-Fi

Wi-Fi uses a lot of battery on an e-reader. Kobo's software turns it on for syncs and downloads, and a few seconds after every wake, and turns it off again after a while.

### When Wi-Fi goes off

Kobo's software turns Wi-Fi off:

- about 20 seconds after the device goes to sleep
- about 2 minutes after the last network activity, on Kobo software 5 and 6
- on Kobo software 4, only when 15 minutes have also passed since Kobo's software last started or last opened a book in its own reader. Because of this, Wi-Fi can stay on for about 15 minutes after a sync.

### When Kobalt turns Wi-Fi off

- If Kobalt turned Wi-Fi on to sync with KOSync, it turns Wi-Fi off when that sync ends. This is also true when the sync fails, or when you tap **Cancel sync**, close the book or put the device to sleep during the sync.
- If Wi-Fi was already on, a KOSync sync leaves it on. Kobalt turns it off only through the setting below; Kobo's own idle timeout can also turn it off.
- **Sign in**, **Sync now** and the sync button in the toolbar leave Wi-Fi as it is.

### Smart Wi-Fi power saving

This setting comes in beta 7. Beta 6 does not show it and does not turn Wi-Fi off for it.

To save battery, Kobalt can turn Wi-Fi off as soon as you open a book. With **Smart Wi-Fi power saving** on, Kobalt also turns Wi-Fi off at once when the device goes to sleep, after the sleep sync if KOSync syncs at sleep. Without it, Kobo's software does this about 20 seconds later. You find this setting under **Settings → System → Power management**. It is off by default.

Kobalt turns Wi-Fi off only when nothing needs it:

- Bluetooth is off. Kobalt leaves Wi-Fi alone while Bluetooth is on, or when it cannot tell.
- The book is not from your Kobo account. Kobo's sync needs Wi-Fi to send your progress for those books.
- When a book opens: automatic KOSync is off, or you have not set up KOSync. At sleep, the sleep sync has already ended, so automatic KOSync does not keep Wi-Fi on.
- Kobo's software is not syncing at that moment.

Turning Wi-Fi off takes a moment, so Kobalt does it while the book is still opening, before you can turn a page, or while the device goes to sleep.

While a book is open, the same setting also keeps Kobo's software from turning Wi-Fi on by itself, for example a few seconds after you wake the device. The rules are the same as when a book opens. KOSync's own syncs, **Sync now**, Kobo's sync and the browser still turn Wi-Fi on when they need it.

### Bluetooth

Kobalt never turns Bluetooth on or off. When you turn pages with a Bluetooth remote, Kobalt tells Kobo's software that the remote is in use, so Kobo's software keeps Bluetooth on. On Kobo software 4 this also keeps Wi-Fi on longer while you use the remote.

### Background activity

While you read, Kobalt reacts to signals from Kobo's software, for example when a sync finishes or the device wakes, instead of checking again and again. About once a minute it updates the clock and refreshes its estimate of your reading speed. It checks for a newer position from another device only after Kobo's software reports a sync. KOSync while reading follows the schedule you choose. While the device is asleep, none of this runs.
