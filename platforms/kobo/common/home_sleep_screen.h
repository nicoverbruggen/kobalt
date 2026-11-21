#pragma once
#include "native_ui.h"
#include <QVector>
#include "storage/reader_storage.h"
#include "storage/record_writer.h"
#include "reading/sleep_screen.h"
#include "platform/api/window_presentation.h"
#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

namespace kobalt::nickel {
struct ReaderEnvironment;

class HomeSleepScreen final : public QObject
{
public:
    explicit HomeSleepScreen(storage::Paths paths, QObject *parent,
                             std::function<quint64()> storageGeneration = {});
    void remember(const sleep::Snapshot &, const QString &identity);
    void forget();
    void prepare(bool enabled, bool powerOff = false);
    void preparePowerOff(sleep::Snapshot, bool preparedOnly = false);
    bool keepPresentedForPowerOff(sleep::Snapshot shown = {});
    void prefetchPowerOff(const QString &identity = {});
    void invalidatePowerOff();
    void prepareCachedPowerOff(const QString &identity = {});
    // Rebuild from the last book's durable privacy choice, never a cached image.
    bool preparePrivateScreen(bool powerOff = false, const QString &identity = {});
    // Kobalt's own screen outside the reader, when Kobalt opened the last book.
    // Draws it from current data: the book's place, Nickel's progress, title
    // and full-size cover, and the wallpaper folder. Reading page and PNG over
    // text dim what Nickel shows, or use the cover when that cannot be read.
    // Nickel's own reading view keeps Nickel's screen. A private book keeps
    // its neutral screen. Returns false to leave Nickel's screen.
    bool prepareHomeScreen(bool powerOff = false, const QString &identity = {});
    void setFontSource(ReaderEnvironment &environment, std::function<bool()> install);

    bool waitingForFonts() const { return !fontBackdrop.image.isNull(); }

    void persistPowerOff(const QString &identity);
    void present();
    // Drop the overlay while a sleep or tentative wake is in progress.
    void clearFrame();
    // A completed wake or a different owner ends the current random status.
    void clear();
    // Waits for the background write of a remembered screen.
    void finishSave();
#ifdef KOBALT_SCENARIO_PLAYER
    QJsonObject takeSaveMeasurement();
#endif

private:
    bool eventFilter(QObject *, QEvent *) override;
    struct PowerOffInputs;
    std::function<quint64()> storageGeneration;
    std::shared_ptr<const PowerOffInputs> powerOffInputs;
    QJsonObject lastPowerOffPreferences;
    std::shared_ptr<std::atomic_bool> powerOffRequest;
    QImage powerOffPage;
    QTimer powerOffPageTimer;
    bool grabbingPowerOffPage = false;
    quint64 powerOffGeneration = 0;
    QString powerOffIdentity;
    QFileSystemWatcher powerOffFiles;
    storage::RecordWriter powerOffReader;
    bool watchingPowerOff = false;
    void updateBattery();
    storage::Paths paths;
    sleep::Snapshot snapshot, shutdownBackdrop, fontBackdrop;
    // What lies under the status, so a battery change repaints only that.
    QVector<QImage> underlay;
    NativeBatteryStatus paintedBattery;
    NativeBatteryStatus powerOffBattery;
    void save(const sleep::Snapshot &, const QString &identity);
    bool poweredOff = false, presented = false, invalidated = false;
    std::function<bool()> prepareFonts;
    void finishFonts(bool ready);
    sleep::Design statusPick(const QJsonObject &preferences, bool powerOff);
    sleep::Design lastStatus = sleep::Design::Stock;
    bool statusChosen = false;
    void composeHome();
    void compose(bool preparedOnly = false);
    std::unique_ptr<WindowPresentation> presentation;

    struct Saved
    {
        bool ok = false;
        QString token;
#ifdef KOBALT_SCENARIO_PLAYER
        double prepareMs = 0;
        QJsonObject measurement;
#endif
    };

    // Only this generation may supply the remembered image and token.
    std::shared_ptr<Saved> pendingSave;
    sleep::Snapshot remembered;
    QString rememberedToken;
#ifdef KOBALT_SCENARIO_PLAYER
    std::shared_ptr<Saved> measuredSave;
#endif
};
} // namespace kobalt::nickel
