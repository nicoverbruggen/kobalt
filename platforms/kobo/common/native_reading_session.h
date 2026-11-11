#pragma once
#include <QString>
#include <memory>
#include "reading_estimates.h"
#include "reading_pace_cache.h"
#include "native_reading_save.h"

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
    void finish(std::function<void(bool)> done);

    void retryFailedSave() { saves.retryFailed(); }

    bool savePending() const
    {
        // `ending` can retain a failed synchronous stop. It is not a worker
        // using storage and must not keep a terminal handoff pending forever.
        return startFlushes > 0 || (saves.pending() && !saves.failed());
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
    // Session-start flushes still running on Nickel's event thread.
    int startFlushes = 0;
    std::shared_ptr<NativeReadingSession *> alive = std::make_shared<NativeReadingSession *>(this);
    nickel::PageReadingActivity activity_;
    nickel::ReadingPaceCache pace_{ nickel::ReadingPaceCache::refreshInterval,
                                    nickel::ReadingPaceCache::Schedule::Ticked };
    nickel::NativeReadingSave saves;
};
} // namespace kobalt
