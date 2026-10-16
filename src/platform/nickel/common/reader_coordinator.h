#pragma once
#include "interfaces/reader_interface.h"
#include "usb_dialog_overlay.h"
#include "engine/crengine/reader_model.h"
#include "reading/reading_progress.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <functional>
#include <memory>

class QKeyEvent;

namespace kobalt {
class NativeOpening;
class NativePublication;
class NativeLifecycle;
class NativeAuthorization;
class NativeProgress;
class ReaderSession;
class ReaderOrientation;
class SourceLease;
enum class ReaderBookRoute;
namespace nickel {
struct ReaderEnvironment;
struct CoordinatorBindings {
    bool (*enabled)();
    void (*installInput)(QObject *) = nullptr;
    void (*goHome)() = nullptr;
    const void *(*publicationKeys)(const NativeOpening &) = nullptr;
    void (*publicationMismatch)(const NativePublication &, const QString &, const QString &) = nullptr;
    bool (*automaticBookOpening)() = nullptr;
    bool (*setAutomaticBookOpening)(bool) = nullptr;
};
// Called by Nickel's Home key filter before it clears native navigation.
bool interceptHomeKey(QKeyEvent *event);

// The retain callback runs synchronously after the coordinator's opening
// checks. Neither the callback nor borrowed firmware arguments are retained.
using RetainOpening = std::function<std::unique_ptr<NativeOpening>()>;
class ReaderCoordinator : public QObject {
public:
    ReaderCoordinator(ReaderEnvironment &environment, CoordinatorBindings bindings, QObject *parent);
    ~ReaderCoordinator() override;
    bool available() const;
    bool idleForBenchmark() const;
    bool begin(const RetainOpening &retainOpening);
    bool handleHomeKey(QKeyEvent *event);

private:
    bool eventFilter(QObject *object, QEvent *event) override;
    ReaderEnvironment &environment_;
    CoordinatorBindings bindings_;
    kobalt::NativeLifecycle *lifecycle;
    kobalt::NativeAuthorization *authorization;
    QVector<std::function<void()>> cleanupCallbacks;
    QTimer timer;
    // These owners describe the current or last opening. After ordinary close,
    // model is the sole retained facade and identity/timings remain for reuse
    // and diagnostics. ReadingBook owns the session while its window is open.
    // Keep cleanup explicit: destroying this value never starts a drain.
    struct ReadingRun {
        ReadingRun();
        ~ReadingRun();
        QPointer<kobalt::ReaderWindow> reader;
        QPointer<QDialog> fallbackAlert;
        std::unique_ptr<kobalt::ReaderOpeningView> loading;
        const kobalt::ReaderInterface *interface = nullptr;
        std::unique_ptr<kobalt::NativeOpening> opening;
        std::unique_ptr<kobalt::NativePublication> nativePublication;
        std::unique_ptr<kobalt::NativeProgress> nativeProgress;
        kobalt::ReadingProgress baseline;
        kobalt::ReadingProgress diskBaseline;
        std::unique_ptr<kobalt::ReaderSession> model;
        std::unique_ptr<kobalt::ReaderOrientation> orientation;
        std::shared_ptr<kobalt::SourceLease> sourceLease;
        QByteArray expectedHash;
        QString identity, filePath, statePath;
        QElapsedTimer openingTimer;
        QJsonObject openingTimings;
        qint64 layoutStarted = 0;
        kobalt::OpeningStage openingStage = kobalt::OpeningStage::Content;
        int openingPercent = -1;
        qint64 progressPainted = -1000;
    } run;
    void updateOpening(kobalt::OpeningStage stage, int percent);
    unsigned generation = 0;
    bool busy = false;
    bool homeAfterClose = false;
    void finishHomeNavigation();

    void status(const QString &value, const QString &reason = {});
    void hideLoading();
    void drainReadingJobs(std::function<void()> finished);
    void disposeForTransition(std::function<void()> finished);
    void release(bool endAuthorization = true);
    void cancelOpening();
    void fail(const QString &reason, kobalt::OpeningFallback state = kobalt::OpeningFallback::Failure,
              bool publicationRejected = false);
    bool sameBook() const;
    void poll();
    void watch();
    void prepareOpening(kobalt::ReaderBookRoute candidate);
    void missingAssets(bool assetsReady);
    bool missingAssetsBeforeOpening(bool assetsReady);
    void load();

    void showReader();
    UsbDialogOverlay usbDialog_;
};

}
}
