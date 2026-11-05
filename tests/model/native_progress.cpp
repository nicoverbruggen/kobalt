#include "platforms/kobo/common/native_progress.h"
#include "platforms/kobo/common/tolino_progress.h"
#include "check.h"
#include <QCoreApplication>

// Emit Nickel's signals without calling the PLT hooks. Calls inside Nickel
// can bypass those hooks, while Qt still delivers the connected signals.
class PlugWorkflowManager : public QObject
{
    Q_OBJECT
public:
    static PlugWorkflowManager *sharedInstance();
signals:
    void aboutToConnect();
    void aboutToUnplug();
    void startedProcessing();
    void doneProcessing();
public slots:

    void unplugged() { emit aboutToUnplug(); }
};

PlugWorkflowManager *PlugWorkflowManager::sharedInstance()
{
    static PlugWorkflowManager manager;
    return &manager;
}

class PlugManager : public QObject
{
    Q_OBJECT
public:
    static PlugManager *sharedInstance();
signals:
    void unplugged();
};

PlugManager *PlugManager::sharedInstance()
{
    static PlugManager manager;
    return &manager;
}

class N3SyncManager : public QObject
{
    Q_OBJECT
public:
    static N3SyncManager *sharedInstance();
signals:
    void finished();
    void userProfileUpdated();
};

N3SyncManager *N3SyncManager::sharedInstance()
{
    static N3SyncManager manager;
    return &manager;
}

// Checks Tolino bookmark verification, and that Nickel sync, account and USB signals notify the reader once a USB transition ends.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QVariantMap bookmark{ { "position", "chapter.xhtml#p:7" },
                          { "currentPosition", "2" },
                          { "lastPosition", "35" },
                          { "progress", float(2.0 / 35) },
                          { "modified", qint64(123456) },
                          { "deletedLocally", false } };
    const auto matches = [&] {
        return kobalt::tolinoBookmarkMatches(bookmark, "chapter.xhtml#p:7", 2, 35);
    };
    require(matches(), "native string page counts and float fractions round trip");
    for (const auto &field : { "position", "currentPosition", "lastPosition", "progress",
                               "modified", "deletedLocally" }) {
        const auto original = bookmark.take(field);
        require(!matches(), "partial native saves cannot pass verification");
        bookmark.insert(field, original);
    }
    bookmark["currentPosition"] = "2invalid";
    require(!matches(), "malformed page count is not a zero page");
    bookmark["currentPosition"] = 2;
    bookmark["progress"] = 0.8;
    require(!matches(), "a stale Tolino fraction cannot verify content-only progress");
    bookmark["progress"] = float(2.0 / 35);
    bookmark["deletedLocally"] = true;
    require(!matches(), "deleted native bookmark cannot verify a save");
    bookmark["deletedLocally"] = false;
    require(!kobalt::tolinoBookmarkMatches(bookmark, "chapter.xhtml#p:7", 0, 0),
            "empty pagination cannot divide by zero");
    bookmark["currentPosition"] = 35;
    bookmark["progress"] = 1.;
    require(kobalt::tolinoBookmarkMatches(bookmark, "chapter.xhtml#p:7", 35, 35),
            "finished progress uses the complete book");
    auto *usb = PlugWorkflowManager::sharedInstance();
    auto *plug = PlugManager::sharedInstance();
    QObject::connect(plug, &PlugManager::unplugged, usb, &PlugWorkflowManager::unplugged);
    kobalt::NativeProgress progress(QStringLiteral("cancel-unplug-fixture"));
    require(!progress.deviceTransition(), "reading starts outside a USB transition");
    require(progress.signalsChanges(),
            "connected sync and USB signals replace the minute progress read");
    progress.takeChange();
    int notifications = 0;
    progress.changeAvailable = [&] {
        require(!progress.deviceTransition(),
                "progress notifications wait for the transition to finish");
        require(progress.takeChange(), "a notification exposes the pending progress change");
        ++notifications;
    };
    emit N3SyncManager::sharedInstance() -> finished();
    emit N3SyncManager::sharedInstance() -> userProfileUpdated();
    require(notifications == 2, "sync and account signals notify the reader without polling");
    progress.changeAvailable = [&] {
        require(!progress.deviceTransition(),
                "USB notifications wait for the transition to finish");
        ++notifications;
    };

    for (int attempt = 0; attempt != 2; ++attempt) {
        // Cancel keeps the book open. Removing the charge-only cable then
        // runs Nickel's unplug slot, without a processing cycle or PLT hook.
        kobalt::NativeProgress::disconnected();
        require(!progress.deviceTransition(), "Cancel keeps page navigation available");
        progress.takeChange();
        emit plug->unplugged();
        require(progress.deviceTransition(), "unplug starts a progress transition");
        QCoreApplication::processEvents();
        require(!progress.deviceTransition(),
                "Cancel then unplug restores page navigation without a PLT hook");
        require(progress.takeChange(), "unplug makes native progress eligible for rechecking");
        require(notifications == 4 + attempt * 2,
                "charge-only disconnect and unplug notify the reader");
    }

    emit usb->aboutToConnect();
    require(progress.deviceTransition(), "confirmed connection blocks progress access");
    emit usb->startedProcessing();
    emit plug->unplugged();
    QCoreApplication::processEvents();
    kobalt::NativeProgress::disconnected();
    require(progress.deviceTransition(),
            "unplug cannot release progress during storage processing");
    require(notifications == 6, "storage processing does not notify before it finishes");
    emit usb->doneProcessing();
    require(!progress.deviceTransition(), "processing completion releases progress");
    require(notifications == 7 && progress.takeChange(),
            "processing completion notifies with a pending change");
}

#include "native_progress.moc"
