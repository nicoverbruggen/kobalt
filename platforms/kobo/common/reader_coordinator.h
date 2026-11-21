#pragma once
#include "app/reader_interface.h"
#include "native_dialog_overlay.h"
#include "engine/blaze/adapter/engine_document.h"
#include "reading/reading_progress.h"
#include "storage/reading_history.h"
#include "storage/record_writer.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSize>
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
// Nickel can request standby before a reader exists or after its window closes.
inline std::function<bool()> readerStandbyCheck(ReaderWindow *reader)
{
    return [window = QPointer<ReaderWindow>(reader)] {
        return !window || window->flushBeforeStandby();
    };
}

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
    // -1 clears the in-memory benchmark choice; 0 and 1 select a reader.
    bool (*setBenchmarkReader)(int) = nullptr;
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
    void finishPreviewMigration();
    bool idleForBenchmark() const;
#if defined(KOBALT_BENCHMARK_API) || defined(KOBALT_SCENARIO_PLAYER)
    QJsonObject benchmarkState(const QString &identity) const;
    bool selectBenchmarkReader(bool enabled);
    bool clearBenchmarkReader();
    bool benchmarkCommand(const QString &identity, int command, int argument);
#endif
    bool begin(const RetainOpening &retainOpening);
    bool handleHomeKey(QKeyEvent *event);
    void forgetSleepScreen();
    // Opens the book that Kobalt showed last in the background, when it is
    // still a plain EPUB on the device, and keeps it as a closed book is kept.
    // Tapping it then takes the fast reopening path. Any opening, USB or
    // other transition cancels a preload that is still running.
    void preloadLastBook();

private:
#ifdef KOBALT_SCENARIO_PLAYER
    // Evaluation packages run the scenarios in the profile's scenarios folder
    // once per process, after the first reader has opened.
    void startScenarios();
    void runScenarios(const QString &path);
    bool scenariosStarted = false;
    storage::RecordWriter scenarioWriter;
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

    // A cancelled opening can retain its reader and failed checkpoint until unlock.
    bool disposingForUsb = false;
    QPointer<kobalt::ReaderWindow> sleepingOpening;

    void updateOpening(kobalt::OpeningStage stage, int percent);
    unsigned generation = 0;
    bool busy = false;
    bool profilePrepared = false;
    // The interface fonts are registered; the Home sleep screen reuses them.
    bool interfaceFontsLoaded = false;
    // Shown by the next reader: a saved-data reset, or a benchmark result that
    // no reader was open to show.
    kobalt::ReaderNotice pendingNotice;
    std::shared_ptr<kobalt::ReadingHistory::Profile> historyProfile;
    bool homeAfterClose = false;
    void finishHomeNavigation();

    void status(const QString &value, const QString &reason = {});
    void hideLoading();
    void drainReadingJobs(std::function<void()> finished);
    void disposeForTransition(std::function<void()> finished, bool nativeAllowed = true, bool usb = false);
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
    void prepareOpening(kobalt::ReaderBookRoute candidate, QJsonObject preferences);
    void prepareOpeningControls();
    bool openingDark = false;
    void missingAssets(bool assetsReady);
    void load(const QJsonObject &globalPreferences);

    void showReader();
    kobalt::ReaderWindow *createReader(bool koboCloud);
    std::unique_ptr<HomeSleepScreen> homeSleep;

    // The book being preloaded; see preloadLastBook(). When it is ready it
    // moves into run.session.
    struct Preload
    {
        std::unique_ptr<kobalt::ReaderSession> session;
        std::shared_ptr<kobalt::SourceLease> lease;
        QElapsedTimer started;
    } preload;

    // Writes the last-book record and reads it back for a preload, off the
    // GUI thread.
    std::unique_ptr<kobalt::storage::RecordWriter> lastBook;
    QString lastBookPath() const;
    void rememberLastBook();
    void startPreload(const QString &identity, const QString &path, QJsonObject saved,
                      const QJsonObject &preferences);
    void cancelPreload();
};

} // namespace nickel
} // namespace kobalt
