#pragma once
#include "reader_session.h"
#include "publication/publication.h"
#include <QElapsedTimer>
#include <QFile>

// Internal to ReaderSession::open. The session fills an OpeningJob on the GUI
// thread and runs it on the engine worker. Nothing here touches the session.
namespace kobalt {
// Everything one opening needs, copied on the GUI thread before the job starts.
struct OpeningJob
{
    std::shared_ptr<EngineDocument> engine;
    QString path;
    QSize size;
    // The saved book state. mergeSavedState edits it in place.
    QJsonObject saved;
    QString nativeLocation;
    std::shared_ptr<SourceLease> lease;
    ResourceFactory factory;
    PositionCodec positions;
    QString systemFont, layoutCachePath, documentCachePath, userStyleSheetPath, coverPath;
    QString customPinsPath;
    QJsonObject pinnedAppearance, appearanceDefaults;
    bool restartAtStart = false;
    ReaderMarkupRequest markup;
    quint64 markupGeneration = 0;

    // What the session kept from the previous opening on the same engine.
    // The hash is empty when that opening cannot be reused.
    QByteArray retainedHash;
    QJsonObject retainedSettings;
    bool retainedWarning = false;
    QJsonObject retainedDetails;

    // Each callback queues its work on the GUI thread and drops it there
    // when the session has moved on. `coverReady` is empty when nobody wants the cover.
    OpeningProgress progress;
    std::function<void()> notifyReuse;
    std::function<void(QImage)> coverReady;
    // False once a newer request replaced this one or the source is gone.
    std::function<bool()> current;

    bool cancelled() const { return !lease || lease->revoked(); }

    OpeningResult interrupted() const;

    void report(OpeningStage stage, int percent) const
    {
        if (progress)
            progress(stage, percent);
    }
};

// What the session publishes on the GUI thread when the job ends.
struct OpeningOutput
{
    storage::CustomPins customPins;
    ReaderFrame frame;
    OpeningResult outcome;
    QByteArray digest, syncDigest;
    bool savedMatches = false;
    bool formattingWarning = false;
    QJsonObject formattingDetails;
};

// The values that the stages of one opening share. Lives on the engine worker.
struct OpeningState
{
    QElapsedTimer timer;
    OpeningResult outcome;
    ReaderFrame frame;
    QFile file;
    Publication publication;
    QByteArray digest, syncDigest;
    bool savedMatches = false;
    // Set by mergeSavedState. `initial` is the host position to open at, if any.
    bool restart = false;
    QString initial;
    std::shared_ptr<ResourceCache> resources;
    std::shared_ptr<std::atomic_bool> contentRejected = std::make_shared<std::atomic_bool>(false);
    // The first readable text resource. Reading it checks that the host still serves the book.
    QString probe;
    bool coverPublished = false;
    bool retainedHit = false, cacheHit = false, cacheStored = false;
    bool formattingWarning = false;
    QJsonObject formattingDetails;
    qint64 directoryMs = 0, hashMs = 0, inspectionMs = 0, modelMs = 0;
};

// The stages, in the order runOpening calls them. Each one leaves a failure in
// `state.outcome`; the caller checks it before the next stage.

// Opens the archive, reads its directory and fingerprints, and decides
// whether the saved state belongs to this version of the book.
void inspectSource(OpeningJob &job, OpeningState &state);
// Reduces `job.saved` to what this opening may use: the whole state when it
// matches the book, otherwise only the settings that do not depend on the
// book. Then applies the line spacing migration, the book defaults and the
// pinned appearance, in that order.
void mergeSavedState(OpeningJob &job, OpeningState &state);
// Reuses the layout that the engine kept from the previous opening.
void openRetained(OpeningJob &job, OpeningState &state);
// Checks the layout, then loads the book from the document cache or parses it.
void openCold(OpeningJob &job, OpeningState &state);
void logOpeningPhases(const OpeningJob &job, const OpeningState &state);

// Runs on the engine worker, inside an EngineJob.
OpeningOutput runOpening(OpeningJob &job);

// Defined in reader_session.cpp, which uses them for every later frame too.
void captureChapters(ReaderFrame &result, EngineDocument &engine);
ReaderFrame capture(EngineDocument &engine, const PositionCodec &positions, bool neighbours = true,
                    const ReaderMarkupRequest &markup = {}, quint64 markupGeneration = 0);
} // namespace kobalt
