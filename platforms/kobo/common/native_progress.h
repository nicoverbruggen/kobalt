#pragma once
#include "reading/reading_progress.h"
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <functional>

namespace kobalt {
struct ReadingEstimates;
struct ReadingActivity;

class NativeProgress : public QObject
{
    Q_OBJECT
public:
    explicit NativeProgress(QString book);
    ~NativeProgress() override;
    static bool available();
    ReadingProgress read(QString *error = nullptr) const;
    bool write(const ReadingProgress &expected, const QString &location, double percent,
               bool finished, ReadingProgress *verified, QString *error,
               const ReadingEstimates *estimates = nullptr,
               const ReadingActivity *activity = nullptr);
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

    // Called on the GUI thread, inside Nickel's sync or USB signal, once
    // takeChange() has a change to report.
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
        if (changeAvailable)
            changeAvailable();
    }

    static QPointer<NativeProgress> active;
#ifndef KOBALT_READER_HARDWARE
    static bool failNextWrite;
#endif
    QString book;
    quint64 connectionId;
    bool transitioning = false, processing = false, changed = true, signalled = false;
    QVariantMap persisted(QString *error) const;
};
} // namespace kobalt
