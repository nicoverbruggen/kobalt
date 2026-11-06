#pragma once
#include <QJsonObject>
#include <QString>

namespace kobalt {
// Display progress uses zero-based pages: first is 0%, last is 100%.
// A one-page book is already on its last page; an unknown length is 0%.
double pageProgressFraction(int page, int pages);
int pageProgressPercent(int page, int pages);

struct ReadingProgress
{
    QString book, location;
    qint64 modified = 0;
    int status = 0;
    bool valid = false;
    bool samePosition(const ReadingProgress &other) const;
    bool operator==(const ReadingProgress &other) const;
    QJsonObject json() const;
    static ReadingProgress fromJson(const QJsonObject &value);
};

// One reconciler belongs to one book opening. A different position needs a choice,
// except when Nickel drops an acknowledged KEPUB span's offset at the same time.
// A choice about an older alert cannot accept a later one.
class ProgressReconciler
{
public:
    explicit ProgressReconciler(ReadingProgress initial)
        : latest(initial), acknowledged(initial) { }

    void observe(const ReadingProgress &incoming);
    void acknowledge(const ReadingProgress &shown);

    bool pending() const { return latest.valid && !(latest == acknowledged); }

    bool mayWrite(const ReadingProgress &current) const
    {
        return !pending() && current == acknowledged;
    }

    const ReadingProgress &candidate() const { return latest; }

    const ReadingProgress &baseline() const { return acknowledged; }

private:
    ReadingProgress latest, acknowledged;
};

// A local checkpoint is authoritative until Nickel has a different, newer
// position than the native baseline stored alongside that checkpoint.
bool preferNativeProgress(const ReadingProgress &current, const ReadingProgress &saved);
// Nickel can clear DateLastRead and the location when a book is marked unread.
// Reset settings and progress only for a changed unread status, not a failed read.
bool resetNativeUnread(QJsonObject &checkpoint, const ReadingProgress &current);
} // namespace kobalt
