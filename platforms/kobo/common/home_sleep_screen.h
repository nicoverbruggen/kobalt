#pragma once
#include <QVector>
#include "storage/reader_storage.h"
#include "reading/sleep_screen.h"
#include "platform/api/window_presentation.h"
#include <QObject>
#include <functional>
#include <memory>

namespace kobalt::nickel {
class HomeSleepScreen final : public QObject
{
public:
    explicit HomeSleepScreen(storage::Paths paths, QObject *parent);
    void remember(const sleep::Snapshot &, const QString &identity);
    void forget();
    void prepare(bool enabled, bool powerOff = false);
    void preparePowerOff(sleep::Snapshot);
    // Rebuild from the last book's durable privacy choice, never a cached image.
    bool preparePrivateScreen(bool powerOff = false, const QString &identity = {});
    // Kobalt's own screen outside the reader, when Kobalt opened the last book.
    // Draws it from current data: the book's place, Nickel's progress, title
    // and full-size cover, and the wallpaper folder. Reading page and PNG over
    // text dim what Nickel shows, or use the cover when that cannot be read.
    // Nickel's own reading view keeps Nickel's screen. A private book keeps
    // its neutral screen. Returns false to leave Nickel's screen.
    bool prepareHomeScreen(bool powerOff = false, const QString &identity = {});
    // Registers Kobalt's interface fonts, once it is clear Kobalt draws.
    std::function<void()> prepareFonts;
    void persistPowerOff(const QString &identity);
    void present();
    void clear();
    // Waits for the background write of a remembered screen.
    void finishSave();
#ifdef KOBALT_SCENARIO_PLAYER
    QJsonObject takeSaveMeasurement();
#endif

private:
    bool eventFilter(QObject *, QEvent *) override;
    void updateBattery();
    storage::Paths paths;
    sleep::Snapshot snapshot, shutdownBackdrop;
    // What lies under the status, so a battery change repaints only that.
    QVector<QImage> underlay;
    void save(const sleep::Snapshot &, const QString &identity);
    bool poweredOff = false, invalidated = false;
    void compose();
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
