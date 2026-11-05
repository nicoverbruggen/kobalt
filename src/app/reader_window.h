#pragma once
#include <QWidget>
#include "reading/sleep_screen.h"
#include "reader_timing.h"
#include <functional>
#include <memory>

namespace kobalt {
class ReaderSession;
class ReaderOrientation;
class ReadingBook;
struct ReaderHostServices;
struct ReaderNotice;

// The firmware controller owns opening, authorization and publication lifetime.
// The window returns a retained engine session only after a normal close.
class ReaderWindow : public QWidget
{
public:
    using QWidget::QWidget;
    std::function<void(std::unique_ptr<ReaderSession>)> finished;
    virtual bool readyForPresentation() const = 0;

    // Prepare optional page-bound content before the host replaces its opening view.
    // Empty preparation completes synchronously. Cancellation destroys the window.
    virtual void preparePresentation(std::function<void(bool)> finished) { finished(true); }

    virtual void checkpoint() = 0;

    // Asked before the device enters standby. False means reading-position
    // saves are pending; the window starts them and the host should ask again.
    virtual bool flushBeforeStandby() { return true; }

    virtual bool pendingStorageSave() const { return false; }

    // Shows a notice over the page. False when the window cannot show one now,
    // for example while it is hidden or another notice is open.
    virtual bool showNotice(const ReaderNotice &) { return false; }

    // Cancel revoked native work before hiding can stop the reading interval.
    virtual void revokeNativeReading() { }

    // Revocation drains local data without writing through a revoked native account.
    virtual void finishStorageSave(std::function<void(bool)> finished, bool nativeAllowed = true)
    {
        (void)nativeAllowed;
        finished(true);
    }

    // Limit the native-position wait to two seconds before file release.
    virtual void prepareUsb(std::function<void()> finished) = 0;
    virtual void prepareShutdown(std::function<void()> finished, bool allowFailedSave = false) = 0;

    virtual sleep::Snapshot powerScreenSnapshot() { return {}; }

    virtual void sleepReader() = 0;

    // A bounded final upload can briefly keep the CPU awake after the sleep view appears.
    virtual bool pendingNetworkSync() const { return false; }

    virtual void prepareWake() = 0;
    virtual void cancelWake() = 0;
    virtual void sleepViewReady() = 0;
    virtual void wakeReader() = 0;
    virtual void progressTick() = 0;
    // Called on the reader's minute tick, while it is visible and awake, so a
    // host's periodic checks share the clock's wakeup instead of their own timer.
    std::function<void()> minuteTicked;
    virtual void refreshHostPresentation() = 0;
    // Logical forward/backward input, independent of touch swipe direction.
    virtual void pageButton(int direction) = 0;

    // For the scenario player: a page key that skips the filter for rapid
    // duplicate input, so a sample measures the turn and not the filter.
    virtual void scriptedPageButton(int direction) { pageButton(direction); }

    // Whether the reading engine has work in flight, such as a layout, a render
    // or drawing the pages beside the current one.
    virtual bool busy() const { return false; }

    // When the opening began and the host's stages before and around the
    // window's construction. The window adds its own build, theme and show
    // stages and writes "reader-shown" once its page has painted.
    virtual void setOpeningTiming(const QElapsedTimer &, const QJsonObject &) { }

    // When the close began and the stages the window has timed, so the host
    // can add its own and write "book-close".
    virtual ActionTiming closingTiming() const { return {}; }

    // For the scenario player: finds `text` as a whole word, turns to the page
    // of its first occurrence and reports its centre in the page widget.
    virtual void locateText(const QString &, std::function<void(QPoint)> found)
    {
        found({ -1, -1 });
    }

    // For the scenario player: the centre of the link with this label on the
    // current page, in the page widget.
    virtual void locateLink(const QString &, std::function<void(QPoint)> found)
    {
        found({ -1, -1 });
    }
};

ReaderWindow *createReaderWindow(std::unique_ptr<ReadingBook> book, ReaderOrientation *orientation,
                                 ReaderHostServices services);
} // namespace kobalt
