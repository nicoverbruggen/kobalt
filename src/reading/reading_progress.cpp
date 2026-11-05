#include "support/qt_compat.h"
#include "reading_progress.h"

namespace kobalt {
// Nickel's reader counts the page on screen as read and rounds down: page 1
// of 83 is 1%, page 82 is 98% and the last page is 100%. Kobalt uses the same
// count, so Kobo's library shows the number that Kobalt shows.
double pageProgressFraction(int page, int pages)
{
    if (pages <= 0)
        return 0;
    return double(qBound(0, page, pages - 1) + 1) / pages;
}

int pageProgressPercent(int page, int pages)
{
    if (pages <= 0)
        return 0;
    return int(100 * (qint64(qBound(0, page, pages - 1)) + 1) / pages);
}

bool ReadingProgress::samePosition(const ReadingProgress &other) const
{
    return valid && other.valid && book == other.book && location == other.location
            && status == other.status;
}

bool ReadingProgress::operator==(const ReadingProgress &other) const
{
    return samePosition(other) && modified == other.modified;
}

QJsonObject ReadingProgress::json() const
{
    return kobalt::qt::object({ { "book", book },
                                { "location", location },
                                { "modified", QString::number(modified) },
                                { "status", status },
                                { "valid", valid } });
}

ReadingProgress ReadingProgress::fromJson(const QJsonObject &value)
{
    return { value.value("book").toString(), value.value("location").toString(),
             value.value("modified").toString().toLongLong(), value.value("status").toInt(),
             value.value("valid").toBool() };
}

void ProgressReconciler::observe(const ReadingProgress &incoming)
{
    if (!incoming.valid || incoming.book != latest.book || incoming == latest
        || (latest.valid && incoming.modified <= latest.modified))
        return;
    // A duplicate timestamp update, or a return to the acknowledged position,
    // does not need another decision. A pending different position still does.
    const bool duplicate = incoming.samePosition(acknowledged);
    latest = incoming;
    if (duplicate)
        acknowledged = incoming;
}

void ProgressReconciler::acknowledge(const ReadingProgress &shown)
{
    if (!shown.valid || shown.book != latest.book || shown.modified > latest.modified)
        return;
    acknowledged = shown;
    if (latest.samePosition(shown))
        acknowledged = latest;
}

bool preferNativeProgress(const ReadingProgress &current, const ReadingProgress &saved)
{
    return current.valid && !current.location.isEmpty() && !current.samePosition(saved)
            && (!saved.valid || current.book != saved.book || current.modified > saved.modified);
}

bool resetNativeUnread(QJsonObject &checkpoint, const ReadingProgress &current)
{
    const auto saved = ReadingProgress::fromJson(checkpoint.value("nickel_progress").toObject());
    if (!current.valid || current.book.isEmpty() || current.status != 0
        || (!checkpoint.isEmpty() && checkpoint.value("book").toString() != current.book)
        || (saved.valid
            && (saved.book != current.book
                || (saved.status == 0 && !preferNativeProgress(current, saved)))))
        return false;
    checkpoint = qt::object({ { "book", current.book } });
    return true;
}
} // namespace kobalt
