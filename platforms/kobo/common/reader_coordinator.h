#pragma once
#include "app/reader_interface.h"
#include "native_dialog_overlay.h"
#include "engine/crengine/engine_document.h"
#include "reading/reading_progress.h"
#include "storage/reading_history.h"
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
class HomeSleepScreen;
// Every accepted native opening invalidates a closed reader snapshot.
void forgetHomeSleepScreen();

struct CoordinatorBindings
{
    bool (*enabled)();
    void (*installInput)(QObject *) = nullptr;
    void (*goHome)() = nullptr;
    const void *(*publicationKeys)(const NativeOpening &) = nullptr;
    void (*publicationMismatch)(const NativePublication &, const QString &,
                                const QString &) = nullptr;
    bool (*automaticBookOpening)() = nullptr;
    bool (*setAutomaticBookOpening)(bool) = nullptr;
};

// Called by Nickel's Home key filter before it clears native navigation.
bool interceptHomeKey(QKeyEvent *event);

// The retain callback runs synchronously after the coordinator's opening
// checks. Neither the callback nor borrowed firmware arguments are retained.
using RetainOpening = std::function<std::unique_ptr<NativeOpening>()>;

class ReaderCoordinator : public QObject
{
public:
    ReaderCoordinator(ReaderEnvironment &environment, CoordinatorBindings bindings,
                      QObject *parent);
    ~ReaderCoordinator() override;
    bool available() const;
    bool idleForBenchmark() const;
    bool begin(const RetainOpening &retainOpening);
    bool handleHomeKey(QKeyEvent *event);
    void forgetSleepScreen();

private:
#ifdef KOBALT_SCENARIO_PLAYER
    // Evaluation packages run the scenarios in the profile's scenarios folder
    // once per process, after the first reader has opened.
    void startScenarios();
    void runScenarios(const QString &path);
    bool scenariosStarted = false;
    // A copy of the first opening, so scenarios can open the same book again.
    std::unique_ptr<NativeOpening> scenarioOpening;
#endif
    ReaderEnvironment &environment_;
    CoordinatorBindings bindings_;
    kobalt::NativeLifecycle *lifecycle;
    kobalt::NativeAuthorization *authorization;
    QVector<std::function<void()>> cleanupCallbacks;
    QTimer timer;

    // These owners describe the current or last opening. After ordinary close,
    // session is the sole retained facade and identity/timings remain for reuse
    // and diagnostics. ReadingBook owns the session while its window is open.
    // Keep cleanup explicit: destroying this value never starts a drain.
    struct ReadingRun
    {
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
        std::unique_ptr<kobalt::ReaderSession> session;
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
    bool profilePrepared = false;
    // Shown by the next reader: a saved-data reset, or a benchmark result that
    // no reader was open to show.
    kobalt::ReaderNotice pendingNotice;
    std::shared_ptr<kobalt::ReadingHistory::Profile> historyProfile;
    bool homeAfterClose = false;
    void finishHomeNavigation();

    void status(const QString &value, const QString &reason = {});
    void hideLoading();
    void drainReadingJobs(std::function<void()> finished);
    void disposeForTransition(std::function<void()> finished, bool nativeAllowed = true);
    void finishDisposal(const QString &finalStatus, bool nativeAllowed);
    void release(bool endAuthorization = true);
    void cancelOpening();
    void fail(const QString &reason,
              kobalt::OpeningFallback state = kobalt::OpeningFallback::Failure,
              bool publicationRejected = false);
    bool sameBook() const;
    void poll();
    void checkProgressSoon();
    void watch();
    void prepareOpening(kobalt::ReaderBookRoute candidate);
    void missingAssets(bool assetsReady);
    bool missingAssetsBeforeOpening(bool assetsReady);
    void load();

    void showReader();
    std::unique_ptr<HomeSleepScreen> homeSleep;
};

} // namespace nickel
} // namespace kobalt
