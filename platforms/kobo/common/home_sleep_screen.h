#pragma once
#include "storage/reader_storage.h"
#include "reading/sleep_screen.h"
#include "platform/api/window_presentation.h"
#include <QObject>
#include <future>

namespace kobalt::nickel {
class HomeSleepScreen final : public QObject
{
public:
    explicit HomeSleepScreen(storage::Paths paths, QObject *parent);
    void remember(const sleep::Snapshot &, const QString &identity);
    void forget();
    void prepare(bool enabled, bool powerOff = false);
    void preparePowerOff(sleep::Snapshot);
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
    void save(const sleep::Snapshot &, const QString &identity);
    bool poweredOff = false, invalidated = false;
    void compose();
    std::unique_ptr<WindowPresentation> presentation;

    struct Saved
    {
        bool ok = false;
        QString token;
    };

    std::future<Saved> pendingSave;
    sleep::Snapshot remembered;
    QString rememberedToken;
#ifdef KOBALT_SCENARIO_PLAYER
    std::shared_ptr<QJsonObject> saveMeasurement;
#endif
};
} // namespace kobalt::nickel
