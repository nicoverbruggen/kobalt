#pragma once
#include "reading/native_save_result.h"
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <functional>

namespace kobalt {
struct ReadingEstimates;
struct ReadingActivity;

// Work that rides the progress transaction. Nickel's reader ends its reading
// session and saves the reading time in the transaction that saves the
// progress; Kobalt does the same through this.
struct NativeProgressExtra
{
    // Runs inside the transaction on the Nickel Volume about to be saved,
    // passed as void * because each firmware defines its own Volume.
    std::function<void(void *volume)> apply;
    // Set when the transaction that ran `apply` committed.
    bool committed = false;
};

class NativeProgress : public QObject
{
    Q_OBJECT
public:
    explicit NativeProgress(QString book);
    ~NativeProgress() override;
    static bool available();
    static std::function<bool()> syncQuery();
    ReadingProgress read(QString *error = nullptr) const;
    bool poll(bool requested, ReadingProgress &result);
    NativeSaveResult write(const ReadingProgress &expected, const QString &location, double percent,
                           bool finished, const ReadingEstimates *estimates = nullptr,
                           const ReadingActivity *activity = nullptr,
                           NativeProgressExtra *extra = nullptr);
    bool markReading();
    // Records the book as recently read for Nickel's Home screen, as Nickel's
    // reader does when a book closes. Best effort; a busy database skips it.
    void markRecent();
    static void disconnected();

    bool deviceTransition() const { return transitioning; }

    // True when Nickel's sync and USB signals are connected, so takeChange()
    // reports every change.
    bool signalsChanges() const { return signalled; }

    // True once after Nickel may have changed the saved progress: after sync,
    // an account update or USB.
    bool takeChange()
    {
        const bool value = changed;
        changed = false;
        return value;
    }

    // Counts the changes that takeChange() reports. A caller that keeps
    // Nickel data of its own compares it to know when to read that data again.
    quint64 changes() const { return changeCount; }

    // Wakes the reader on the GUI thread after a native change or a completed poll.
    std::function<void()> changeAvailable;
#ifndef KOBALT_READER_HARDWARE
    static void testCommand();
    static QJsonObject testAction(const QJsonObject &command);
#endif
private slots:

    void beginTransition() { transitioning = true; }

    void beginProcessing() { processing = transitioning = true; }

    void endTransition()
    {
        transitioning = processing = false;
        noteChange();
    }

    void finishUnplug()
    {
        if (!processing)
            endTransition();
    }

    void markChanged() { noteChange(); }

private:
    void noteChange()
    {
        changed = true;
        ++changeCount;
        invalidatePoll();
        if (changeAvailable)
            changeAvailable();
    }

    static QPointer<NativeProgress> active;
#ifndef KOBALT_READER_HARDWARE
    static bool failNextWrite;
#endif
    QString book;
    quint64 connectionId;
    quint64 changeCount = 0;
    bool transitioning = false, processing = false, changed = true, signalled = false;

    void invalidatePoll() const
    {
        ++pollGeneration;
        pollReady = false;
    }

    mutable quint64 pollGeneration = 0;
    mutable bool pollReady = false;
    bool pollPending = false;
    quint64 pollStorage = 0;
    ReadingProgress polled;
    std::function<QVariantMap()> rowQuery(bool polling, QString *error) const;
    ReadingProgress decode(const QVariantMap &row, QString *error) const;
    QVariantMap persisted(QString *error) const;
};
} // namespace kobalt
