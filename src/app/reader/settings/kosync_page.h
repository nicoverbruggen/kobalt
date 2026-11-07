#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/options.h"
#include "ui/dialogs/modals.h"
#include "platform/api/keyboard_controller.h"
#include "sync/kosync_schedule.h"
#include <QPointer>
#include <QDateTime>

namespace kobalt::reader {
class KosyncScheduleSheet;

class KosyncPage final : public QWidget
{
public:
    KosyncPage(QWidget *parent, QWidget *overlayParent, KeyboardFactory keyboard);
    ~KosyncPage() override;
    void setAccount(const QString &server, const QString &username, bool signedIn, bool automatic);
    void setSchedule(const kosync::Schedule &schedule);
    // Which platform moments the Automatic sync sheet offers.
    void setScheduleFeatures(bool sleepWake, bool wifi);
    void setNativeCloudSync(bool enabled, bool koboCloud = false);
    void setLastSync(const QDateTime &time);
    void setDeviceName(const QString &name, const QString &defaultName);
    void setFilenameMatching(bool enabled);
    void setStatus(const QString &status, bool busy);
    void fitWidth(int width);

    QString server() const { return server_; }

    QString username() const { return username_; }

    QString takePassword();
    void dismissEditor();
    QPointer<ui::ModalOverlay> editorDialog;
    std::function<void()> loginRequested, logoutRequested, syncRequested;
    std::function<void(bool)> automaticChanged;
    std::function<void(const kosync::Schedule &)> scheduleChanged;
    std::function<void(const QString &)> deviceNameChanged;
    std::function<void(bool)> filenameMatchingChanged;

protected:
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void edit(int field);
    void placeEditor();
    void maskPassword();
    void updateCaptions();
    void showSyncHelp();
    void dismissSyncHelp();
    void showMatchingChoices();
    void dismissMatchingChoices();
    void showSchedule();
    void dismissSchedule();
    void updateAutomaticCaption();
    QPointer<ui::NoticeModal> syncHelp_;
    QPointer<ui::NoticeModal> matchingDialog_;
    QPointer<KosyncScheduleSheet> scheduleSheet_;
    QWidget *overlayParent_;
    KeyboardFactory makeKeyboard_;
    std::unique_ptr<KeyboardController> keyboard_;
    QString server_, username_, password_, deviceName_, defaultDeviceName_;
    bool signedIn_ = false;
    bool submitting_ = false;
    bool accountEdited_ = false;
    bool nativeCloudSync_ = false;
    bool filenameMatching_ = false;
    bool automatic_ = true;
    kosync::Schedule schedule_;
    bool sleepWake_ = false, wifi_ = false;
    ui::CaptionButton *serverButton_, *usernameButton_, *passwordButton_, *deviceNameButton_,
            *matchingButton_;
    QPushButton *login_, *logout_, *sync_, *learnMore_;
    ui::CaptionButton *automaticButton_;
    QLabel *description_, *sideloadGuidance_, *status_, *lastSync_;
    QWidget *syncInformation_;
};
} // namespace kobalt::reader
