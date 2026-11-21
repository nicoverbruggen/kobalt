#include "support/qt_compat.h"
#include "reading_progress.h"

namespace kobalt {
static QString romanPage(int page)
{
    QString result;

    static constexpr struct
    {
        int value;
        const char *digits;
    } numerals[] = { { 1000, "m" }, { 900, "cm" }, { 500, "d" }, { 400, "cd" }, { 100, "c" },
                     { 90, "xc" },  { 50, "l" },   { 40, "xl" }, { 10, "x" },   { 9, "ix" },
                     { 5, "v" },    { 4, "iv" },   { 1, "i" } };

    for (const auto &numeral : numerals)
        while (page >= numeral.value) {
            result += QLatin1String(numeral.digits);
            page -= numeral.value;
        }
    return result;
}

const QStringList &secondaryNumberStyles()
{
    static const QStringList styles{ "upper_roman", "roman", "arabic", "letters", "prefixed" };
    return styles;
}

QString bookPageLabel(int page, int firstMain, int lastMain, const QString &style)
{
    if (page < 0)
        return {};
    if (page >= firstMain && page <= lastMain)
        return QString::number(page - firstMain + 1);
    const bool front = page < firstMain;
    int number = front ? page + 1 : page - lastMain;
    if (style == "arabic")
        return QString::number(number);
    if (style == "prefixed")
        return (front ? QStringLiteral("F") : QStringLiteral("B")) + QString::number(number);
    if (style == "letters") {
        QString label;
        while (number > 0) {
            --number;
            label.prepend(QChar('a' + number % 26));
            number /= 26;
        }
        return label;
    }
    const auto roman = romanPage(number);
    return style == "roman" ? roman : roman.toUpper();
}

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

int pageProgressTenths(int page, int pages)
{
    if (pages <= 0)
        return 0;
    return int(1000 * (qint64(qBound(0, page, pages - 1)) + 1) / pages);
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
    // Callers read the current committed database row, not queued event data.
    // An older timestamp still needs a choice when its position has changed.
    if (!incoming.valid || incoming.book != latest.book || incoming == latest)
        return;
    // A host may recognize a normalized observation of the acknowledged
    // position. This does not weaken exact comparisons before a write.
    const bool duplicate = incoming.samePosition(acknowledged)
            || (normalization && normalization(acknowledged, incoming));
    latest = incoming;
    if (duplicate)
        acknowledged = incoming;
}

void ProgressReconciler::acknowledge(const ReadingProgress &shown)
{
    if (!shown.valid || shown.book != latest.book)
        return;
    if (latest.samePosition(shown))
        acknowledged = latest;
    else if (shown.modified <= latest.modified)
        acknowledged = shown;
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
    // Marking a book as unread resets where the reader is, not how the book
    // is set up. Remove only reading progress; text settings, the text
    // preset, the dictionary and every other choice stay.
    for (const auto *key :
         { "position", "imported_native_location", "percent", "stats_main_percent",
           "stats_volume_percent", "finished_position", "suppress_finish_prompt",
           "nickel_progress", "sync_pending", "native_writeback", "native_writeback_error",
           "native_writeback_failure", "page", "pages", "next_page", "chapter_page",
           "chapter_pages", "chapter_title", "book_fraction", "slice_start", "rough_page",
           "rough_pages", "rough_item_page", "rough_item_pages", "rough_pages_estimated",
           "rough_item_pages_estimated", "cached_pages", "continues_before", "continues_after",
           "whole_book_ready" })
        checkpoint.remove(QLatin1String(key));
    checkpoint["book"] = current.book;
    return true;
}
} // namespace kobalt
