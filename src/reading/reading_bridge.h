#pragma once
#include "reading_progress.h"
#include "chapter.h"

namespace kobalt {
// A completed reader frame. Platforms can use its source counts and layout
// without accessing the renderer or supplying a shared prediction algorithm.
struct ReadingActivity {
    QString position;
    double percent = 0;
    int page = -1, pages = 0, turnDirection = 0;
    qint64 totalWords = -1;
    QVector<ChapterLink> chapters;
    ChapterSelection chapter;
};
struct ReadingEstimates {
    qint64 chapterMilliseconds = -1, bookMilliseconds = -1;
    bool learningPace = false;
};
// GUI-thread services for a platform's existing reading database and activity
// recorder. A standalone reader has no bridge; local state still belongs to
// ReadingBook. The bridge never owns the engine session or publication access.
class ReadingBridge {
public:
    virtual ~ReadingBridge() = default;
    virtual bool deviceTransition() const = 0;
    virtual bool writable() const = 0;
    virtual ReadingProgress read(QString *error = nullptr) const = 0;
    virtual bool takeChange() = 0;
    virtual bool write(const ReadingProgress &expected, const QString &location, double percent, bool finished,
                       const ReadingActivity &activity, ReadingProgress *verified, QString *error) = 0;
    virtual void observeReading(const ReadingActivity &activity) = 0;
    // No platform estimator means the reader uses its local session pace.
    virtual bool estimates(const ReadingActivity &, ReadingEstimates *) {
        return false;
    }
    virtual void stopReading() = 0;
};
}
