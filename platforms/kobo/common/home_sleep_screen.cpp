#include "home_sleep_screen.h"
#include "last_read_book.h"
#include "native_ui.h"
#include "window_presentation.h"
#include "app/reader/views/sleep_screen.h"
#include "storage/sleep_snapshot.h"
#include "support/diagnostics.h"
#include "support/strings.h"
#include <QFile>
#include <QImageReader>
#include <QSqlQuery>
#include <QApplication>
#include <QWidget>
#include <QEvent>
#include <QScreen>
#include <QJsonArray>
#include <QSqlDatabase>
#include <dlfcn.h>
#include <future>
#include <atomic>
#include <QElapsedTimer>

namespace kobalt::nickel {
namespace {
QString databasePath()
{
    const auto device = reinterpret_cast<const void *(*)()>(
            dlsym(RTLD_DEFAULT, "_ZN6Device16getCurrentDeviceEv"));
    const auto name = reinterpret_cast<const QString &(*)(const void *)>(
            dlsym(RTLD_DEFAULT, "_ZNK6Device9getDbNameEv"));
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const auto connection = reinterpret_cast<QSqlDatabase (*)(QString, bool)>(
            dlsym(RTLD_DEFAULT, "_ZN15DatabaseAdapter12dbConnectionE7QStringb"));
#else
    const auto connection = reinterpret_cast<QSqlDatabase (*)(const QString &, bool)>(
            dlsym(RTLD_DEFAULT, "_ZN15DatabaseAdapter12dbConnectionERK7QStringb"));
#endif
    const auto *current = device ? device() : nullptr;
    if (!current || !name || !connection)
        return {};
    const auto native = connection(name(current), false);
    if (!native.isValid() || !native.isOpen())
        return {};
    return native.databaseName();
}

// Nickel's cache of full-screen covers, found by the book's image id. Nickel
// names the two folders after Qt 4's string hash of the id.
QString nativeCoverPath(const QString &imageId)
{
    if (imageId.isEmpty())
        return {};
    uint hash = 0;
    for (const char byte : imageId.toUtf8()) {
        hash = (hash << 4) + static_cast<unsigned char>(byte);
        hash ^= (hash & 0xf0000000) >> 23;
        hash &= 0x0fffffff;
    }
    return QStringLiteral("/mnt/onboard/.kobo-images/%1/%2/%3 - N3_FULL.parsed")
            .arg(hash & 0xff)
            .arg((hash & 0xff00) >> 8)
            .arg(imageId);
}

struct NativeBook
{
    QString title, author, imageId;
    // Nickel's percentage, as My Books shows it; -1 when unknown.
    int percent = -1;
};

// The title, author and cover id Nickel keeps for a book, read-only.
NativeBook readNativeBook(const QString &path, const QString &identity)
{
    NativeBook book;
    if (path.isEmpty() || identity.isEmpty())
        return book;
    static std::atomic<unsigned> serial{ 0 };
    const auto connectionName = QStringLiteral("kobalt-home-book-%1").arg(++serial);
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        database.setDatabaseName(path);
        database.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare("SELECT Title, Attribution, ImageId, ___PercentRead FROM content "
                          "WHERE ContentID = ? AND ContentType = 6 LIMIT 1");
            query.addBindValue(identity);
            if (query.exec() && query.next()) {
                book.title = query.value(0).toString();
                book.author = query.value(1).toString();
                book.imageId = query.value(2).toString();
                bool valid = false;
                const int percent = query.value(3).toInt(&valid);
                if (valid && !query.value(3).isNull())
                    book.percent = qBound(0, percent, 100);
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return book;
}

// Whether Nickel's own reader is showing a book. Its sleep screen stays Nickel's.
bool nativeReading()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (widget->isVisible() && widget->inherits("MainWindowView"))
            for (auto *child : widget->findChildren<QWidget *>())
                if (child->isVisible() && child->inherits("ReadingView"))
                    return true;
    return false;
}

// What Nickel shows right now, such as Home or My Books. Reading page uses it
// outside the reader, the way it uses the book's page inside it.
QImage nativeScreen()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (widget->isVisible() && widget->inherits("MainWindowView"))
            return widget->grab().toImage().convertToFormat(QImage::Format_RGB32);
    return {};
}

// Decodes no larger than the screen needs.
QImage readCover(const QString &path, QSize screen)
{
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 20 * 1024 * 1024)
        return {};
    QImageReader reader(&file);
    const QSize size = reader.size();
    if (size.isEmpty() || size.width() > 8192 || size.height() > 8192)
        return {};
    if (size.width() > screen.width() || size.height() > screen.height())
        reader.setScaledSize(size.scaled(screen, Qt::KeepAspectRatio));
    return reader.read();
}

QString readToken(const QString &path, const QString &book = {}, QJsonObject *measurement = nullptr)
{
    if (path.isEmpty())
        return {};
    QElapsedTimer total, stage;
    total.start();
    stage.start();
    QString token;
    static std::atomic<unsigned> serial{ 0 };
    const auto connectionName = QStringLiteral("kobalt-home-sleep-%1").arg(++serial);
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        database.setDatabaseName(path);
        database.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
        const bool opened = database.open();
        if (measurement)
            (*measurement)["token_open_ms"] = stage.nsecsElapsed() / 1e6;
        stage.restart();
        if (opened)
            token = lastReadBookToken(database, book);
        if (measurement)
            (*measurement)["token_query_ms"] = stage.nsecsElapsed() / 1e6;
        stage.restart();
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    if (measurement) {
        (*measurement)["token_close_ms"] = stage.nsecsElapsed() / 1e6;
        (*measurement)["token_ms"] = total.nsecsElapsed() / 1e6;
    }
    return token;
}
} // namespace

HomeSleepScreen::HomeSleepScreen(storage::Paths paths, QObject *parent)
    : QObject(parent),
      paths(std::move(paths)),
      presentation(makeWindowPresentation(nullptr, nativeReaderSleepScreenAvailable()))
{
    qApp->installEventFilter(this);
    observeNativeBattery(this, [this] { updateBattery(); });
}

void HomeSleepScreen::remember(const sleep::Snapshot &next, const QString &identity)
{
    clear();
    save(next, identity);
}

void HomeSleepScreen::save(const sleep::Snapshot &next, const QString &identity)
{
#ifdef KOBALT_SCENARIO_PLAYER
    saveMeasurement.reset();
    QElapsedTimer prepare;
    prepare.start();
#endif
    // A failed write must leave the stock screen, rather than a stale page
    // from an earlier reading session.
    invalidated = true;
    storage::removeSleepSnapshot(paths.sleepScreen());
    remembered = {};
    rememberedToken.clear();
    if (next.image.isNull() || next.mode == sleep::Mode::Stock)
        return;
#ifdef KOBALT_SCENARIO_PLAYER
    const double invalidateMs = prepare.nsecsElapsed() / 1e6;
#endif
    // Resolve Nickel's connection on its owning thread. The worker receives
    // only the filename and opens its own read-only connection for committed data.
    const auto database = databasePath();
    if (database.isEmpty())
        return;
    remembered = next;
    // Lookup and persistence run off the GUI thread. clear() waits before the
    // cache is used or removed, including USB export and power-off.
#ifdef KOBALT_SCENARIO_PLAYER
    const double prepareMs = prepare.nsecsElapsed() / 1e6;
    saveMeasurement = std::make_shared<QJsonObject>();
    pendingSave = std::async(std::launch::async,
                             [path = paths.sleepScreen(), next, database, identity,
                              measurement = saveMeasurement, prepareMs, invalidateMs] {
                                 QJsonObject tokenTimes;
                                 const auto token = readToken(database, identity, &tokenTimes);
                                 const bool saved = !token.isEmpty()
                                         && storage::measureSleepPersistence(path, next, token,
                                                                             *measurement);
                                 for (auto it = tokenTimes.begin(); it != tokenTimes.end(); ++it)
                                     (*measurement)[it.key()] = it.value();
                                 (*measurement)["prepare_ms"] = prepareMs;
                                 (*measurement)["invalidate_ms"] = invalidateMs;
                                 (*measurement)["saved"] = saved;
                                 return Saved{ saved, token };
                             });
#else
    pendingSave =
            std::async(std::launch::async, [path = paths.sleepScreen(), next, database, identity] {
                const auto token = readToken(database, identity);
                return Saved{ !token.isEmpty() && storage::writeSleepSnapshot(path, next, token),
                              token };
            });
#endif
}
#ifdef KOBALT_SCENARIO_PLAYER
QJsonObject HomeSleepScreen::takeSaveMeasurement()
{
    QElapsedTimer wait;
    wait.start();
    finishSave();
    auto result = saveMeasurement ? *saveMeasurement : QJsonObject();
    result["drain_ms"] = wait.nsecsElapsed() / 1e6;
    saveMeasurement.reset();
    return result;
}
#endif
void HomeSleepScreen::finishSave()
{
    if (!pendingSave.valid())
        return;
    const auto saved = pendingSave.get();
    if (saved.ok) {
        invalidated = false;
        rememberedToken = saved.token;
    } else {
        remembered = {};
        diagnostic("sleep-snapshot-save-failed", {}, kobalt::DiagnosticSeverity::Error);
    }
}

void HomeSleepScreen::forget()
{
    clear();
    invalidated = true;
    remembered = {};
    rememberedToken.clear();
    storage::removeSleepSnapshot(paths.sleepScreen());
}

bool HomeSleepScreen::preparePrivateScreen(bool powerOff, const QString &openingIdentity)
{
    auto identity = openingIdentity;
    QJsonObject bookState;
    if (identity.isEmpty()) {
        const auto token = readToken(databasePath());
        const auto fields = QJsonDocument::fromJson(token.toUtf8()).array();
        identity = fields.isEmpty() ? QString() : fields.at(0).toString();
    }
    if (!identity.isEmpty()) {
        const auto state = storage::readBookState(paths, identity, paths.bookState(identity));
        bookState = state.object;
        const bool readable = state.status == storage::ReadStatus::Ok
                || state.status == storage::ReadStatus::Missing;
        if (readable && !state.object.value("hide_book_on_sleep_screen").toBool(false))
            return false;
    }
    // A failed or ambiguous lookup cannot safely opt a private book back into
    // Nickel's cover. Use the neutral screen until its choice can be read.
    clear();
    const auto *screen = QGuiApplication::primaryScreen();
    const auto preferences = storage::readPreferences(paths.preferences()).object;
    sleep::Snapshot source;
    const auto token = readToken(databasePath(), identity);
    // Only a verified private snapshot can supply anonymous statistics. Native
    // covers and old reading-page caches must never become a private backdrop.
    if (!token.isEmpty() && token == rememberedToken && remembered.privateBook)
        source = remembered;
    if (!source.privateBook && !token.isEmpty()) {
        const auto saved =
                storage::readSleepSnapshot(paths.sleepScreen(), token, sleep::Mode::Plain);
        if (saved.privateBook)
            source = saved;
    }
    const auto finished = bookState.value("finished_position").toString();
    const bool completed =
            !finished.isEmpty() && finished == bookState.value("position").toString();
    if (completed || bookState.value("percent").isDouble()) {
        source.percent = completed ? 100 : qBound(0, bookState.value("percent").toInt(), 100);
        source.progress = strings::get("sleep.percent_read").arg(source.percent);
    }
    source.place = sleep::place(preferences, bookState.value("sleep_positions").toObject(),
                                sleep::look(preferences, powerOff));
    snapshot = sleep::privateSnapshot(screen ? screen->size() : QSize(800, 1200), preferences,
                                      source, false, powerOff);
    if (source.privateBook && source.design == sleep::Design::Overlay
        && sleep::design(preferences) == sleep::Design::Overlay && !source.image.isNull())
        snapshot.image = source.image;
    // The neutral screen draws its status with Kobalt's fonts too.
    if (prepareFonts)
        prepareFonts();
    poweredOff = powerOff;
    compose();
    return true;
}

bool HomeSleepScreen::prepareHomeScreen(bool powerOff, const QString &openingIdentity)
{
    if (preparePrivateScreen(powerOff, openingIdentity))
        return true;
    clear();
    if (!presentation->supportsSleepScreen())
        return false;
    const auto database = databasePath();
    auto identity = openingIdentity;
    if (identity.isEmpty()) {
        const auto fields = QJsonDocument::fromJson(readToken(database).toUtf8()).array();
        identity = fields.isEmpty() ? QString() : fields.at(0).toString();
    }
    if (identity.isEmpty() || nativeReading())
        return false;
    // Kobalt keeps state only for the books it opened.
    const auto state = storage::readBookState(paths, identity, paths.bookState(identity));
    if (state.status != storage::ReadStatus::Ok
        || state.object.value("book").toString() != identity)
        return false;
    const auto preferences = storage::readPreferences(paths.preferences());
    if (preferences.status != storage::ReadStatus::Ok)
        return false;
    auto look = sleep::look(preferences.object, powerOff);
    if (look.mode == sleep::Mode::Stock)
        return false;
    // Outside the reader, Nickel's own screen is the page. Without it, the
    // cover stands in.
    QImage page;
    if (look.mode == sleep::Mode::Page || look.mode == sleep::Mode::Overlay) {
        page = nativeScreen();
        if (page.isNull())
            look.mode = sleep::Mode::Cover;
    }
    const auto *screen = QGuiApplication::primaryScreen();
    QSize size = screen ? screen->size() : QSize(800, 1200);
    size = QSize(qMin(size.width(), size.height()), qMax(size.width(), size.height()));
    const auto book = readNativeBook(database, identity);
    sleep::Snapshot shot;
    shot.orientation = Qt::PortraitOrientation;
    shot.dark = preferences.object.value("dark_mode").toBool(false);
    shot.design = look.design;
    shot.tile = look.tile;
    shot.place = sleep::place(preferences.object, state.object.value("sleep_positions").toObject(),
                              look);
    shot.dimPage = false;
    const auto finished = state.object.value("finished_position").toString();
    const bool completed =
            !finished.isEmpty() && finished == state.object.value("position").toString();
    // Nickel's percentage matches My Books, also after reading in Nickel.
    if (completed || book.percent >= 0 || state.object.value("percent").isDouble()) {
        shot.percent = completed    ? 100
                : book.percent >= 0 ? book.percent
                                    : qBound(0, state.object.value("percent").toInt(), 100);
        shot.progress = strings::get("sleep.percent_read").arg(shot.percent);
    }
    shot.details.title = book.title;
    shot.details.author = book.author;
    shot.details.font = preferences.object.value("default_font_face").toString();
    shot.details.greeting = preferences.object.value(sleep::greetingKey).toString();
    shot.mode = look.mode;
    if (!page.isNull()) {
        shot.image = std::move(page);
        shot.dimPage = true;
        if (look.mode == sleep::Mode::Overlay) {
            const auto overlay =
                    storage::loadSleepOverlay({ QStringLiteral(KOBALT_CONFIG_DIR "/screensaver"),
                                                QStringLiteral("/mnt/onboard/.adds/screensaver") });
            if (!overlay.isNull()) {
                reader::dimSleepPage(shot.image, shot.dark);
                sleep::drawOverlay(shot.image, overlay);
                shot.dimPage = false;
            }
        }
    } else if (look.mode == sleep::Mode::Cover) {
        const auto cover = readCover(nativeCoverPath(book.imageId), size);
        if (!cover.isNull()) {
            shot.image = sleep::coverBackground(cover, size);
            shot.coverArea = sleep::coverArea(cover.size(), size);
        }
    } else if (look.mode == sleep::Mode::Wallpaper) {
        const auto custom = sleep::custom(preferences.object);
        const auto &layer = powerOff && !custom.same ? custom.off : custom.sleep;
        shot.image = reader::sleepWallpaperBackground(
                storage::loadSleepWallpaper({ QStringLiteral(KOBALT_CONFIG_DIR "/wallpapers") },
                                            layer.wallpaper, size),
                size);
    }
    // A missing cover or wallpaper leaves the status on blank paper.
    if (shot.image.isNull()) {
        shot.mode = sleep::Mode::Plain;
        shot.image = QImage(size, QImage::Format_RGB32);
        shot.image.fill(shot.dark ? Qt::black : Qt::white);
    }
    if (prepareFonts)
        prepareFonts();
    snapshot = std::move(shot);
    poweredOff = powerOff;
    compose();
    diagnostic("home-sleep-screen",
               { { "design", sleep::value(snapshot.design) },
                 { "mode", sleep::value(snapshot.mode) },
                 { "power_off", powerOff } });
    return !snapshot.image.isNull();
}

void HomeSleepScreen::prepare(bool enabled, bool powerOff)
{
    if (enabled && preparePrivateScreen(powerOff))
        return;
    clear();
    if (!enabled || invalidated || !presentation->supportsSleepScreen())
        return;
    const auto preferences = storage::readPreferences(paths.preferences());
    if (preferences.status != storage::ReadStatus::Ok)
        return;
    const auto look = sleep::look(preferences.object, powerOff);
    const auto mode = look.mode;
    const auto token = readToken(databasePath());
    if (!token.isEmpty() && token == rememberedToken && remembered.mode == mode
        && (remembered.coverPath.isEmpty() || storage::currentSleepCover(remembered)))
        snapshot = remembered;
    else
        snapshot = storage::readSleepSnapshot(paths.sleepScreen(), token, mode);
    snapshot.tile = look.tile;
    snapshot.design = look.design;
    snapshot.place = sleep::place(preferences.object, {}, look);
    poweredOff = powerOff;
    compose();
}

void HomeSleepScreen::preparePowerOff(sleep::Snapshot next)
{
    clear();
    shutdownBackdrop = next;
    snapshot = std::move(next);
    poweredOff = true;
    compose();
}

void HomeSleepScreen::persistPowerOff(const QString &identity)
{
    if (poweredOff && !shutdownBackdrop.image.isNull()) {
        save(shutdownBackdrop, identity);
        // Nickel powers off once the shutdown continues.
        finishSave();
    }
    shutdownBackdrop = {};
}

bool HomeSleepScreen::eventFilter(QObject *object, QEvent *event)
{
    if (poweredOff && event->type() == QEvent::Show && object->inherits("AbstractPowerView"))
        present();
    return false;
}

void HomeSleepScreen::compose()
{
    if (snapshot.image.isNull())
        return;
    reader::portraitSleepSnapshot(snapshot);
    const auto battery = nativeReaderBattery();
    const auto status = poweredOff ? reader::SleepStatus::PoweredOff
            : battery.fullyCharged ? reader::SleepStatus::Charged
            : battery.charging     ? reader::SleepStatus::Charging
                                   : reader::SleepStatus::Sleeping;
    snapshot.image = reader::sleepDesignImage(std::move(snapshot.image), snapshot, battery.percent,
                                              status, &underlay);
    snapshot.dimPage = false;
}

void HomeSleepScreen::present()
{
    if (!snapshot.image.isNull())
        presentation->presentSleepScreen(snapshot.image, snapshot.orientation);
}

void HomeSleepScreen::clear()
{
    // Sleep, wake, USB and power-off all clear first. None of them may meet a
    // half-written file or unmount storage under the writer.
    finishSave();
    presentation->clearSleepScreen();
    snapshot = {};
    underlay.clear();
    shutdownBackdrop = {};
    poweredOff = false;
}

void HomeSleepScreen::updateBattery()
{
    if (snapshot.image.isNull() || poweredOff)
        return;
    const auto battery = nativeReaderBattery();
    const auto status = battery.fullyCharged ? reader::SleepStatus::Charged
            : battery.charging               ? reader::SleepStatus::Charging
                                             : reader::SleepStatus::Sleeping;
    const auto area =
            reader::repaintSleepStatus(snapshot.image, underlay, snapshot, battery.percent, status);
    if (!area.isEmpty())
        presentation->updateSleepScreen(snapshot.image, area);
}
} // namespace kobalt::nickel
