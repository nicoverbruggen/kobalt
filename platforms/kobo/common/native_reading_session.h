#pragma once
#include <QString>
#include <memory>
#include "reading_estimates.h"
#include "reading_pace_cache.h"
#include "native_reading_save.h"
#include "reading/native_activity_result.h"

namespace kobalt {
// Construct only after firmwareSupported() accepts Nickel, on the GUI thread.
class NativeReadingSession
{
public:
    explicit NativeReadingSession(QString book);
    ~NativeReadingSession();
    static bool available();
    void start(const ReadingActivity &activity);
    void page(const ReadingActivity &activity);
    bool estimates(const ReadingActivity &activity, ReadingEstimates *result);

    bool estimates(ReadingEstimates *result) { return estimates(activity_.current(), result); }

    void stop();
    // Waits for every queued reading-time write, for a lifecycle handoff.
    void finish(std::function<void(bool)> done);
    // Ends the session for a close without waiting for its reading time.
    // `done` runs once Nickel's database is free for the close's progress
    // write; the reading time follows after the reader has closed.
    void finishForClose(std::function<void(bool)> done);

    // Starts no new reading-time write until finishForClose() has run and the
    // reader is gone, or reading resumes.
    void holdSaves() { saves.hold(); }

    void retryFailedSave(std::function<void(bool)> done = {},
                         ReadingSaveRetry mode = ReadingSaveRetry::Explicit)
    {
        saves.retryFailed();
        if (!done)
            return;
        // A deferred stop still owns the old interval. Never stop a new, running interval.
        if (ending && mode == ReadingSaveRetry::Explicit)
            stop();
        if (ending)
            done(false);
        else
            saves.finish(std::move(done));
    }

    bool savePending() const
    {
        // `ending` can retain a failed synchronous stop. It is not a worker
        // using storage and must not keep a terminal handoff pending forever.
        // A held write is not pending either: the close that holds it waits
        // for this to be false before the reader goes, and the reader going
        // releases it. A handoff that needs it saved calls finish(), which
        // releases the hold first.
        return saves.pending() && !saves.failed() && !saves.isHeld();
    }

    std::function<void()> estimatesChanged;

    // The reader's minute tick refreshes Nickel's reading pace.
    void minuteTick() { pace_.tick(); }

    // Page events use one lookup of the book's Nickel record for as long as
    // `generation` stays the same. The owner passes a new generation when
    // Nickel reports a sync, an account update or the end of a USB session.
    // With `cache` false every page event looks the record up again: the
    // owner cannot tell when Nickel changes it.
    void useRecord(bool cache, quint64 generation)
    {
        if (!cache || generation != recordGeneration)
            forgetRecord();
        cacheRecord = cache;
        recordGeneration = generation;
    }
#ifndef KOBALT_READER_HARDWARE
    static void testCommand();
#endif
private:
    // Fires the page event for the words on the page just left.
    void report(int words);
    void forgetRecord();
    // Nickel's record of the book, defined with each firmware's Volume.
    struct Record;
    std::unique_ptr<Record> record_;
    bool cacheRecord = false;
    quint64 recordGeneration = 0;
    QString book;
    double percent = 0;
    bool running = false, ending = false;
    nickel::PageReadingActivity activity_;
    nickel::ReadingPaceCache pace_{ nickel::ReadingPaceCache::refreshInterval,
                                    nickel::ReadingPaceCache::Schedule::Ticked };
    // One queue for the process, so a book's reading time can still be
    // written after its reader has closed.
    nickel::NativeReadingSave &saves = nickel::NativeReadingSave::shared();
};
} // namespace kobalt
