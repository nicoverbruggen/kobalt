#include "home_sleep_screen.h"
#include "last_read_book.h"
#include "native_ui.h"
#include "window_presentation.h"
#include "app/reader/views/sleep_screen.h"
#include "storage/sleep_snapshot.h"
#include "support/diagnostics.h"
#include "support/strings.h"
#include <QFile>
#include <QApplication>
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
    snapshot =
            sleep::privateSnapshot(screen ? screen->size() : QSize(800, 1200), preferences, source);
    if (source.privateBook && source.design == sleep::Design::Overlay
        && sleep::design(preferences) == sleep::Design::Overlay && !source.image.isNull())
        snapshot.image = source.image;
    poweredOff = powerOff;
    compose();
    return true;
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
    const auto mode = sleep::mode(preferences.object);
    const auto token = readToken(databasePath());
    if (!token.isEmpty() && token == rememberedToken && remembered.mode == mode
        && (remembered.coverPath.isEmpty() || storage::currentSleepCover(remembered)))
        snapshot = remembered;
    else
        snapshot = storage::readSleepSnapshot(paths.sleepScreen(), token, mode);
    snapshot.tile = sleep::tile(preferences.object);
    snapshot.design = sleep::design(preferences.object);
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
