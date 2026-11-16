#pragma once
#include <QString>
#include <functional>
#include <memory>
#include "reading_estimates.h"
#include "reading_pace_cache.h"
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

    // Pauses the session. Nickel's session stays open until a boundary ends it.
    void stop();
    // Ends a paused session inside the transaction that saves Nickel's
    // progress, as Nickel's reader does when it leaves a book or the device
    // sleeps, and adds the session's reading time to `volume`, the record
    // that transaction saves. Call saved() with the outcome.
    void endIn(void *volume);
    // Ends a paused session in Nickel without saving its time, which stays
    // in pendingSeconds for the next save.
    void endSession();
    void saved(bool committed);
    // Ends the session and saves its reading time in a short transaction of
    // its own, for a boundary where no progress write carried it. On the GUI
    // thread, at a boundary where no input can arrive.
    bool endNow();

    void finish(std::function<void(bool)> done) { done(endNow()); }

    // An explicit retry saves now, at a boundary where no input can arrive.
    // A background retry, such as on wake, writes nothing: the user can read
    // then, and the next progress write carries the time.
    void retryFailedSave(std::function<void(bool)> done, bool explicitRetry)
    {
        if (explicitRetry)
            done(endNow());
        else
            done(true);
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
    void startPace();
    // `ending`: paused, with Nickel's session still open. `pendingSeconds`:
    // the time of ended sessions that no transaction has saved yet, or -1.
    bool running = false, ending = false;
    int pendingSeconds = -1;
    nickel::PageReadingActivity activity_;
    nickel::ReadingPaceCache pace_{ nickel::ReadingPaceCache::refreshInterval,
                                    nickel::ReadingPaceCache::Schedule::Ticked };
};
} // namespace kobalt
