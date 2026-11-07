#include "ui/style/fonts.h"
#include "kosync_page.h"
#include "kosync_schedule_sheet.h"
#include "ui/controls/text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include <QHideEvent>
#include <QDynamicPropertyChangeEvent>
#include <QVariant>
#include <QLineEdit>
#include <QLocale>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QApplication>

namespace kobalt::reader {
namespace metrics = ui::metrics;

KosyncPage::KosyncPage(QWidget *parent, QWidget *overlayParent, KeyboardFactory keyboard)
    : QWidget(parent), overlayParent_(overlayParent), makeKeyboard_(std::move(keyboard))
{
    setObjectName("rrKosyncPage");
    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(0);
    description_ = new ui::InterfaceLabel(strings::get("kosync.description"), this);
    description_->setObjectName("rrKosyncDescription");
    description_->setTextFormat(Qt::PlainText);
    description_->setWordWrap(true);
    ui::readerStyle(
            description_,
            QString("QLabel{background:palette(midlight);border-bottom:1px solid palette(light);padding:%1px %2px;")
                            .arg(int(metrics::spacing::row))
                            .arg(int(metrics::chrome::inset))
                    + ui::styles::font(metrics::chrome::descriptionText) + "}");
    rows->addWidget(description_);
    sideloadGuidance_ = new ui::InterfaceLabel(strings::get("kosync.sideload_copy"), this);
    sideloadGuidance_->setObjectName("rrKosyncSideloadGuidance");
    sideloadGuidance_->setTextFormat(Qt::PlainText);
    sideloadGuidance_->setWordWrap(true);
    ui::readerStyle(sideloadGuidance_,
                    "QLabel{border-bottom:1px solid palette(light);"
                            + ui::styles::font(metrics::chrome::descriptionText) + "}");
    sideloadGuidance_->setMargin(metrics::chrome::inset);
    rows->addWidget(sideloadGuidance_);
    learnMore_ = ui::makeButton(this, rows, "rrKosyncLearnMore", strings::get("kosync.learn_more"),
                                [this] { showSyncHelp(); });
    serverButton_ = ui::makeCaptionButton(this, rows, "rrKosyncServer",
                                          strings::get("kosync.server"), [this] { edit(0); });
    usernameButton_ = ui::makeCaptionButton(this, rows, "rrKosyncUsername",
                                            strings::get("kosync.username"), [this] { edit(1); });
    passwordButton_ = ui::makeCaptionButton(this, rows, "rrKosyncPassword",
                                            strings::get("kosync.password"), [this] { edit(2); });
    deviceNameButton_ =
            ui::makeCaptionButton(this, rows, "rrKosyncDeviceName",
                                  strings::get("kosync.device_name"), [this] { edit(3); });
    matchingButton_ =
            ui::makeCaptionButton(this, rows, "rrKosyncMatching", strings::get("kosync.matching"),
                                  [this] { showMatchingChoices(); });
    login_ = ui::makeButton(this, rows, "rrKosyncLogin", strings::get("kosync.login"), [this] {
        if (loginRequested)
            loginRequested();
    });
    automaticButton_ =
            ui::makeCaptionButton(this, rows, "rrKosyncAutomatic", strings::get("kosync.automatic"),
                                  [this] { showSchedule(); });
    sync_ = ui::makeButton(this, rows, "rrKosyncNow", strings::get("kosync.sync_now"), [this] {
        if (syncRequested)
            syncRequested();
    });
    logout_ = ui::makeButton(this, rows, "rrKosyncLogout", strings::get("kosync.logout"), [this] {
        if (logoutRequested)
            logoutRequested();
    });
    const auto rowStyle =
            QString("QPushButton{font-size:%1px;border:0;border-radius:0;text-align:left;padding:0 %2px;"
                    "background:palette(window);color:palette(window-text);} QPushButton:pressed{background:palette(midlight);}")
                    .arg(int(metrics::chrome::labelText))
                    .arg(int(metrics::chrome::inset));
    for (auto *field : { serverButton_, usernameButton_, passwordButton_, deviceNameButton_,
                         matchingButton_, automaticButton_ }) {
        ui::readerStyle(field,
                        rowStyle
                                + QString("QPushButton{min-height:%1px;max-height:%1px;}")
                                          .arg(int(metrics::chrome::actionHeight)));
        field->setRule(Qt::BottomEdge);
        field->setIconVisible(false);
        field->setChevron(true);
        field->setValueCaption(true);
        field->setFixedHeight(metrics::chrome::actionHeight);
    }
    for (auto *action : { login_, sync_, logout_, learnMore_ }) {
        static_cast<ui::Button *>(action)->setContentAlignment(Qt::AlignLeft);
        ui::readerStyle(
                action,
                rowStyle
                        + QString("QPushButton{min-height:%1px;max-height:%1px;border-bottom:1px solid palette(light);}")
                                  .arg(int(metrics::chrome::actionHeight) - 1));
        action->setFixedHeight(metrics::chrome::actionHeight);
    }
    syncInformation_ = new QWidget(this);
    auto *informationRows = new QVBoxLayout(syncInformation_);
    informationRows->setContentsMargins(metrics::chrome::inset, metrics::spacing::row,
                                        metrics::chrome::inset, metrics::spacing::row);
    informationRows->setSpacing(ui::px(12));
    status_ = new ui::InterfaceLabel(syncInformation_);
    status_->setObjectName("rrKosyncStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    ui::readerStyle(status_,
                    "color:palette(placeholder-text);"
                            + ui::styles::font(metrics::chrome::descriptionText));
    informationRows->addWidget(status_);
    lastSync_ = new ui::InterfaceLabel(syncInformation_);
    lastSync_->setObjectName("rrKosyncLastSync");
    lastSync_->setTextFormat(Qt::PlainText);
    lastSync_->setWordWrap(true);
    ui::readerStyle(lastSync_,
                    "color:palette(placeholder-text);"
                            + ui::styles::font(metrics::chrome::descriptionText));
    informationRows->addWidget(lastSync_);
    rows->addWidget(syncInformation_);
    setLastSync({});
    setFilenameMatching(false);
    rows->addStretch(1);
    overlayParent_->window()->installEventFilter(this);
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive)
                    maskPassword();
            });
    setAccount({}, {}, false, true);
}

KosyncPage::~KosyncPage()
{
    dismissEditor();
}

void KosyncPage::setNativeCloudSync(bool enabled, bool koboCloud)
{
    dismissEditor();
    nativeCloudSync_ = enabled;
    const auto description = koboCloud ? "kosync.kobo_cloud" : "kosync.native_sync";
    description_->setText(strings::get(enabled ? description : "kosync.description"));
    setStatus({}, false);
}

void KosyncPage::setAccount(const QString &server, const QString &username, bool signedIn,
                            bool automatic)
{
    server_ = server;
    username_ = username;
    signedIn_ = signedIn;
    accountEdited_ = false;
    automatic_ = automatic;
    automaticButton_->setEnabled(signedIn);
    if (!signedIn)
        dismissSchedule();
    else if (scheduleSheet_)
        scheduleSheet_->setChoices(automatic_, schedule_);
    updateAutomaticCaption();
    sync_->setEnabled(signedIn);
    logout_->setEnabled(signedIn);
    setStatus({}, false);
    updateCaptions();
}

void KosyncPage::setSchedule(const kosync::Schedule &schedule)
{
    schedule_ = schedule;
    if (scheduleSheet_)
        scheduleSheet_->setChoices(automatic_, schedule_);
    updateAutomaticCaption();
}

void KosyncPage::setScheduleFeatures(bool sleepWake, bool wifi)
{
    if (sleepWake_ == sleepWake && wifi_ == wifi)
        return;
    sleepWake_ = sleepWake;
    wifi_ = wifi;
    dismissSchedule();
    updateAutomaticCaption();
}

void KosyncPage::updateAutomaticCaption()
{
    QString caption;
    if (!automatic_)
        caption = strings::get("kosync.automatic_off_caption");
    else if (!kosync::syncsWhileReading(schedule_))
        caption = strings::get("kosync.automatic_caption_never");
    else if (!wifi_)
        caption = KosyncScheduleSheet::intervalLabel(schedule_.minutes);
    else
        caption = strings::get(schedule_.wakeWifi ? "kosync.automatic_caption_wake"
                                                  : "kosync.automatic_caption_wait")
                          .arg(KosyncScheduleSheet::intervalLabel(schedule_.minutes));
    automaticButton_->setCaption(caption);
}

void KosyncPage::showSchedule()
{
    if (scheduleSheet_ || !signedIn_)
        return;
    auto *sheet =
            new KosyncScheduleSheet(overlayParent_, automatic_, schedule_, { sleepWake_, wifi_ });
    scheduleSheet_ = sheet;
    sheet->closed = [this] { dismissSchedule(); };
    sheet->automaticChanged = [this](bool enabled) {
        if (automaticChanged)
            automaticChanged(enabled);
    };
    sheet->scheduleChanged = [this](const kosync::Schedule &schedule) {
        if (scheduleChanged)
            scheduleChanged(schedule);
    };
    sheet->show();
    sheet->raise();
    sheet->fit();
}

void KosyncPage::dismissSchedule()
{
    if (!scheduleSheet_)
        return;
    auto *sheet = scheduleSheet_.data();
    scheduleSheet_ = nullptr;
    sheet->dismissIntervals();
    sheet->hide();
    sheet->deleteLater();
}

void KosyncPage::updateCaptions()
{
    serverButton_->setCaption(server_.isEmpty() ? strings::get("kosync.not_set") : server_);
    usernameButton_->setCaption(username_.isEmpty() ? strings::get("kosync.not_set") : username_);
    passwordButton_->setCaption(password_.isEmpty() ? strings::get("kosync.not_set")
                                                    : QStringLiteral("••••••••"));
    deviceNameButton_->setCaption(deviceName_.isEmpty() ? defaultDeviceName_ : deviceName_);
}

void KosyncPage::setDeviceName(const QString &name, const QString &defaultName)
{
    deviceName_ = name;
    defaultDeviceName_ = defaultName;
    updateCaptions();
}

void KosyncPage::setFilenameMatching(bool enabled)
{
    filenameMatching_ = enabled;
    matchingButton_->setCaption(
            strings::get(enabled ? "kosync.match_filename" : "kosync.match_binary"));
}

QString KosyncPage::takePassword()
{
    const auto value = password_;
    password_.clear();
    updateCaptions();
    return value;
}

void KosyncPage::setLastSync(const QDateTime &time)
{
    lastSync_->setText(strings::get("kosync.last_synced")
                               .arg(time.isValid() ? QLocale().toString(time.toLocalTime(),
                                                                        QLocale::ShortFormat)
                                                   : strings::get("kosync.never")));
}

void KosyncPage::setStatus(const QString &status, bool busy)
{
    sideloadGuidance_->setVisible(nativeCloudSync_);
    learnMore_->setVisible(nativeCloudSync_);
    status_->setText(status);
    status_->setVisible(!status.isEmpty());
    serverButton_->setVisible(!nativeCloudSync_);
    usernameButton_->setVisible(!nativeCloudSync_);
    deviceNameButton_->setVisible(!nativeCloudSync_);
    matchingButton_->setVisible(!nativeCloudSync_);
    syncInformation_->setVisible(!nativeCloudSync_);
    const bool authenticating = !nativeCloudSync_ && (!signedIn_ || accountEdited_);
    passwordButton_->setVisible(authenticating);
    login_->setVisible(authenticating);
    automaticButton_->setVisible(!nativeCloudSync_ && signedIn_ && !accountEdited_);
    sync_->setVisible(!nativeCloudSync_ && signedIn_ && !accountEdited_);
    logout_->setVisible(!nativeCloudSync_ && signedIn_);
    lastSync_->setVisible(signedIn_ && !accountEdited_);
    for (auto *button :
         { static_cast<QPushButton *>(serverButton_), static_cast<QPushButton *>(usernameButton_),
           static_cast<QPushButton *>(passwordButton_),
           static_cast<QPushButton *>(deviceNameButton_),
           static_cast<QPushButton *>(matchingButton_), login_ })
        button->setEnabled(!busy);
    automaticButton_->setEnabled(!busy && signedIn_ && !accountEdited_);
    sync_->setEnabled(!busy && signedIn_ && !accountEdited_);
    logout_->setEnabled(!busy && signedIn_);
}

void KosyncPage::fitWidth(int width)
{
    // Measure the wrapped message before the pager uses the row's size hint.
    auto *rows = syncInformation_->layout();
    rows->invalidate();
    syncInformation_->setFixedHeight(qMax(rows->minimumSize().height(),
                                          rows->hasHeightForWidth()
                                                  ? rows->totalHeightForWidth(width)
                                                  : rows->sizeHint().height()));
    layout()->invalidate();
}

void KosyncPage::edit(int field)
{
    if (editorDialog || !makeKeyboard_)
        return;
    submitting_ = false;
    const auto titleText = strings::get(field == 0           ? "kosync.server"
                                                : field == 1 ? "kosync.username"
                                                : field == 2 ? "kosync.password"
                                                             : "kosync.device_name");
    auto *dialog = new ui::ChoiceModal(overlayParent_, { {}, titleText, {} },
                                       { strings::get("common.cancel"), "rrKosyncEditCancel" },
                                       { strings::get("common.ok"), "rrKosyncEditSave" });
    editorDialog = dialog;
    editorDialog->setObjectName("rrKosyncEditor");
    editorDialog->setDimmed(true);
    editorDialog->setGeometry(overlayParent_->rect());
    auto *card = editorDialog->card();
    auto *editor = new QLineEdit(card);
    kobalt::ui::prepareNativeEditor(editor);
    editor->setObjectName("rrKosyncInput");
    // Native keys have no click focus. Keep their focus fallback on the input
    // instead of repainting the modal backdrop on every press and release.
    editorDialog->setFocusProxy(editor);
    editor->setMaxLength(field == 0 ? 2048 : field == 3 ? 128 : 256);
    editor->setText(field == 0           ? server_
                            : field == 1 ? username_
                            : field == 2 ? password_
                                         : deviceName_);
    // The server must use HTTPS, so an empty server field starts with its scheme.
    if (field == 0 && server_.isEmpty())
        editor->setText(QStringLiteral("https://"));
    editor->setAccessibleName(titleText);
    editor->setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase
                                | (field == 2 ? Qt::ImhSensitiveData : Qt::ImhNone));
    if (field == 2)
        editor->setEchoMode(QLineEdit::Password);
    ui::readerStyle(
            editor,
            "QLineEdit{border:none;border-radius:0;padding:0;background:palette(window);color:palette(window-text);"
                    + ui::styles::font(metrics::chrome::labelText) + "}");
    editor->setTextMargins(metrics::notice::cardMargins.left(), 0,
                           metrics::notice::cardMargins.right(), 0);
    if (field == 3)
        editor->setPlaceholderText(defaultDeviceName_);
    editor->setMinimumHeight(metrics::chrome::actionHeight);
    if (field == 2) {
        auto *row = new QWidget(card);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, metrics::notice::cardMargins.right(), 0);
        layout->setSpacing(0);
        layout->addWidget(editor, 1);
        auto *reveal = ui::makeButton(row, layout, "rrKosyncPasswordReveal",
                                      strings::get("kosync.show_password"), [] { });
        reveal->setCheckable(true);
        ui::readerStyle(reveal,
                        ui::styles::outlined() + "QPushButton{"
                                + ui::styles::font(metrics::chrome::labelText) + "}");
        reveal->ensurePolished();
        const int showWidth = reveal->sizeHint().width();
        reveal->setText(strings::get("kosync.hide_password"));
        reveal->setFixedWidth(qMax(showWidth, reveal->sizeHint().width()));
        reveal->setText(strings::get("kosync.show_password"));
        reveal->setAccessibleName(strings::get("kosync.show_password_accessible"));
        connect(reveal, &QPushButton::toggled, editor, [editor, reveal](bool visible) {
            editor->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
            editor->setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase
                                        | Qt::ImhSensitiveData);
            reveal->setText(
                    strings::get(visible ? "kosync.hide_password" : "kosync.show_password"));
            reveal->setAccessibleName(strings::get(visible ? "kosync.hide_password_accessible"
                                                           : "kosync.show_password_accessible"));
        });
        card->rows()->insertWidget(1, row);
    } else {
        card->rows()->insertWidget(1, editor);
    }
    auto *cancel = dialog->secondaryButton();
    auto *save = dialog->primaryButton();
    const auto submit = [this, field, editor] {
        if (submitting_)
            return;
        submitting_ = true;
        auto text = editor->text();
        // The prefilled scheme alone is not a server.
        if (field == 0 && text.trimmed() == QLatin1String("https://"))
            text.clear();
        (field == 0           ? server_
                 : field == 1 ? username_
                 : field == 2 ? password_
                              : deviceName_) = text;
        updateCaptions();
        if (field == 3) {
            if (deviceNameChanged)
                deviceNameChanged(deviceName_);
        } else if (field != 2) {
            accountEdited_ = true;
            setStatus(strings::get("kosync.account_changed"), false);
        }
        // Native submission runs inside the controller. Destroy it only after
        // its callback and any duplicate Return signal have returned.
        editorDialog->hide();
        QTimer::singleShot(0, this, [this] { dismissEditor(); });
    };
    connect(cancel, &QPushButton::clicked, this, [this] { dismissEditor(); });
    connect(save, &QPushButton::clicked, this, submit);
    connect(editor, &QLineEdit::returnPressed, this, submit);
    editorDialog->dismissed = [this] { dismissEditor(); };
    keyboard_ = makeKeyboard_(editorDialog, editor);
    keyboard_->setSubmitText(strings::get("common.ok"));
    keyboard_->submitted = submit;
    editorDialog->installEventFilter(this);
    editorDialog->show();
    editorDialog->raise();
    editorDialog->fitCard();
    editor->setFocus();
    keyboard_->show();
    placeEditor();
}

void KosyncPage::dismissEditor()
{
    dismissSyncHelp();
    dismissMatchingChoices();
    dismissSchedule();
    if (!editorDialog)
        return;
    keyboard_->hide();
    keyboard_->submitted = {};
    editorDialog->hide();
    keyboard_.reset();
    editorDialog->deleteLater();
    editorDialog = nullptr;
}

void KosyncPage::maskPassword()
{
    if (editorDialog)
        if (auto *reveal = editorDialog->findChild<QPushButton *>("rrKosyncPasswordReveal"))
            reveal->setChecked(false);
}

void KosyncPage::showMatchingChoices()
{
    if (matchingDialog_)
        return;
    auto *dialog = new ui::NoticeModal(overlayParent_, { {}, strings::get("kosync.matching"), {} },
                                       { strings::get("common.cancel"), "rrKosyncMatchingCancel" });
    matchingDialog_ = dialog;
    dialog->setObjectName("rrKosyncMatchingDialog");
    dialog->setDimmed(true);
    dialog->setGeometry(overlayParent_->rect());
    auto *choices = new ui::RadioList(dialog->card());
    choices->setUniformRows(false);
    choices->setMinimumRowHeight(metrics::chrome::listActionHeight);
    choices->add("rrKosyncBinary", strings::get("kosync.match_binary"),
                 strings::get("kosync.match_binary_description"));
    choices->add("rrKosyncFilename", strings::get("kosync.match_filename"),
                 strings::get("kosync.match_filename_description"));
    choices->setCurrent(filenameMatching_ ? 1 : 0);
    choices->chosen = [this](int index) {
        dismissMatchingChoices();
        if (filenameMatchingChanged)
            filenameMatchingChanged(index == 1);
    };
    dialog->card()->rows()->insertWidget(1, choices);
    const int width = qMin<int>(metrics::notice::cardWidth,
                                overlayParent_->width() - 2 * metrics::notice::sideClearance);
    choices->setFixedHeight(choices->heightForWidth(width - 2 * metrics::control::border));
    connect(dialog->primaryButton(), &QPushButton::clicked, this,
            [this] { dismissMatchingChoices(); });
    dialog->dismissed = [this] { dismissMatchingChoices(); };
    dialog->show();
    dialog->raise();
    dialog->fitCard();
}

void KosyncPage::dismissMatchingChoices()
{
    if (!matchingDialog_)
        return;
    matchingDialog_->hide();
    matchingDialog_->deleteLater();
    matchingDialog_ = nullptr;
}

void KosyncPage::showSyncHelp()
{
    if (syncHelp_)
        return;
    auto *dialog = new ui::NoticeModal(overlayParent_,
                                       { strings::get("kosync.learn_more_description"),
                                         strings::get("kosync.learn_more_title"),
                                         {} },
                                       { strings::get("common.ok"), "rrKosyncHelpOK" });
    syncHelp_ = dialog;
    dialog->setObjectName("rrKosyncHelp");
    dialog->setDimmed(true);
    dialog->setGeometry(overlayParent_->rect());
    connect(dialog->primaryButton(), &QPushButton::clicked, this, [this] { dismissSyncHelp(); });
    dialog->dismissed = [this] { dismissSyncHelp(); };
    dialog->show();
    dialog->raise();
    dialog->fitCard();
}

void KosyncPage::dismissSyncHelp()
{
    if (!syncHelp_)
        return;
    syncHelp_->hide();
    syncHelp_->deleteLater();
    syncHelp_ = nullptr;
}

void KosyncPage::hideEvent(QHideEvent *event)
{
    dismissEditor();
    password_.clear();
    updateCaptions();
    QWidget::hideEvent(event);
}

void KosyncPage::placeEditor()
{
    if (!editorDialog)
        return;
    const auto bounds = overlayParent_->window()->property("reader_keyboard_bounds").toRect();
    int bottom = editorDialog->height() / 2;
    if (!bounds.isEmpty())
        bottom = editorDialog->mapFrom(overlayParent_->window(), bounds.topLeft()).y();
    editorDialog->setCardArea(QRect(0, 0, editorDialog->width(), qMax(1, bottom)));
}

bool KosyncPage::eventFilter(QObject *object, QEvent *event)
{
    if (object == editorDialog && event->type() == QEvent::Hide)
        maskPassword();
    if (object == editorDialog && event->type() == QEvent::Resize && keyboard_) {
        keyboard_->place();
        editorDialog->fitCard();
        placeEditor();
    }
    if (event->type() == QEvent::DynamicPropertyChange
        && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()
                == "reader_keyboard_bounds")
        placeEditor();
    return QWidget::eventFilter(object, event);
}
} // namespace kobalt::reader
