#pragma once
#include "reading/sleep_screen.h"
#include <QStringList>
class QImageWriter;

namespace kobalt::storage {
// Picks one usable transparent PNG from the supplied directories.
QImage loadSleepOverlay(const QStringList &directories);
// The PNG and JPEG file names in the wallpaper directories, sorted, without
// duplicates. It reads the directories: call it off the GUI thread while a
// reader can take input, or while the device goes to sleep.
QStringList sleepWallpapers(const QStringList &directories);
// Decodes the named wallpaper, scaled to cover `bound` within the image limit.
// An empty name, or a name that is no longer there, picks a usable file at
// random. The same thread rule as sleepWallpapers() applies.
QImage loadSleepWallpaper(const QStringList &directories, const QString &name, QSize bound);

// A disposable PNG, or metadata referring to the native full-size cover.
// Both are atomically replaced and bounded. The token identifies Nickel's
// committed last-read record, not an application release.
bool writeSleepSnapshot(const QString &path, const sleep::Snapshot &, const QString &token);
sleep::Snapshot readSleepSnapshot(const QString &path, const QString &token, sleep::Mode mode);
void removeSleepSnapshot(const QString &path);
bool currentSleepCover(const sleep::Snapshot &);
// Capability arguments let native checks exercise the older PNG quality API.
bool configureSleepSnapshotCodec(QImageWriter &, bool supportsCompression, bool supportsQuality,
                                 QJsonObject &measurement);
#ifdef KOBALT_SCENARIO_PLAYER
bool measureSleepPersistence(const QString &path, const sleep::Snapshot &, const QString &token,
                             QJsonObject &measurement);
#endif
} // namespace kobalt::storage
