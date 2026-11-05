#pragma once
#include "reading_progress.h"
#include "chapter.h"
#include <functional>

namespace kobalt {
// A completed reader frame. Platforms can use its source counts and layout
// without accessing the renderer or supplying a shared prediction algorithm.
struct ReadingActivity
{
    QString position;
    double percent = 0;
    int page = -1, pages = 0, turnDirection = 0;
    qint64 totalWords = -1;
    QVector<ChapterLink> chapters;
    ChapterSelection chapter;
    // Native mapping captured with the displayed frame; empty when unsupported.
    QString nativePosition = {};
};

struct ReadingEstimates
{
    qint64 chapterMilliseconds = -1, bookMilliseconds = -1;
    bool learningPace = false;
};

// GUI-thread services for a platform's existing reading database and activity
// recorder. A standalone reader has no bridge; local state still belongs to
// ReadingBook. The bridge never owns the engine session or publication access.
class ReadingBridge
{
public:
    virtual ~ReadingBridge() = default;
    virtual bool deviceTransition() const = 0;
    virtual bool writable() const = 0;
    virtual ReadingProgress read(QString *error = nullptr) const = 0;
    virtual bool takeChange() = 0;

    // True when takeChange() reports every change, from the platform's own
    // signals. Without them, the reader also reads the saved progress once a
    // minute.
    virtual bool signalsChanges() const { return false; }

    virtual bool write(const ReadingProgress &expected, const QString &location, double percent,
                       bool finished, const ReadingActivity &activity, ReadingProgress *verified,
                       QString *error) = 0;

    // Called once under the loading view, without position or estimate writes.
    virtual bool markReading() { return true; }

    virtual void observeReading(const ReadingActivity &activity) = 0;

    // No platform estimator means the reader uses its local session pace.
    virtual bool estimates(const ReadingActivity &, ReadingEstimates *) { return false; }

    // Called once a minute while the reader is visible and awake, with the
    // clock update, so periodic platform work shares one wakeup.
    virtual void minuteTick() { }

    // Called once when the book closes, after the final progress write. Kobo's
    // reader records the book as recently read for the Home screen at this point.
    virtual void markRecent() { }

    virtual void stopReading() = 0;

    // A lifecycle boundary can precede the last frame-observation callback.
    virtual void stopReading(const ReadingActivity &) { stopReading(); }

    // Keep the bridge alive until completion before returning native storage.
    virtual void finishReading(std::function<void(bool)> done)
    {
        stopReading();
        done(true);
    }

    virtual bool readingSavePending() const { return false; }

    virtual void retryFailedReadingSave() { }

    // Called on the GUI thread when a background pace refresh changes estimates.
    std::function<void()> estimatesChanged;
};
} // namespace kobalt
