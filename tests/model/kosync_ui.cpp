#include "interfaces/reader/settings/settings_panel.h"
#include "interfaces/reader/views/kosync_notice.h"
#include "check.h"
#include "support/strings.h"
#include "reader_inputs.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/styles.h"
#include "interfaces/reader/ui/style/theme.h"
#include "interfaces/reader/ui/style/private_text.h"
#include <QLabel>
#include "interfaces/reader/ui/controls/keyboard.h"
#include <QApplication>
#include <QLineEdit>
#include <QLocale>
#include <QFontInfo>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QVariant>
#include <QStyleFactory>
#include <QDir>
#include <QImage>
#include <QTest>

using namespace kobalt;
namespace {
class Keyboard final : public KeyboardController {
public:
    explicit Keyboard(int &destroyed, QWidget *host) : destroyed_(destroyed), host_(host) {}
    ~Keyboard() override { ++destroyed_; }
    bool show() override { place(); return true; }
    void hide() override { host_->window()->setProperty("reader_keyboard_bounds", QRect()); }
    void place() override { host_->window()->setProperty("reader_keyboard_bounds", QRect(host_->mapTo(host_->window(), QPoint(0, host_->height() / 2)), QSize(host_->width(), host_->height() - host_->height() / 2))); }
    void setSubmitText(const QString &text) override { submitText = text; }
    QString submitText;
private:
    int &destroyed_;
    QWidget *host_;
};
void checkKeyboardInputStaysInEditor() {
    const int savedDensity = ui::density();
    for (int ppi : {212, 300})
        for (bool dark : {false, true}) {
            ui::setDensity(ppi);
            QWidget host;
            host.setProperty("reader_dark", dark);
            host.resize(ui::px(800), ui::px(1136));
            host.setFont(ui::interfaceFont());
            ui::applyReaderPalette(&host, ui::readerPalette(host.palette(), dark));
            ui::readerStyle(&host, ui::styles::reader());
            reader::KosyncPage page(&host, &host, ui::makeWidgetKeyboard);
            page.resize(host.size());
            host.show();
            page.show();
            QApplication::processEvents();
            for (const char *name : {"rrKosyncLogin", "rrKosyncNow", "rrKosyncLogout", "rrKosyncLearnMore"}) {
                page.setNativeCloudSync(QString(name) == "rrKosyncLearnMore");
                page.setAccount("https://sync.example", "reader", QString(name) != "rrKosyncLogin", true);
                auto *action = page.findChild<QPushButton *>(name);
                require(action && action->isVisible() && action->isEnabled(), "Sync action is available for contrast check");
                const auto normal = action->grab().toImage();
                QTest::mousePress(action, Qt::LeftButton);
                require(action->isDown(), "Sync contrast check delivered a press");
                const auto held = action->grab().toImage();
                const auto colors = ui::themeColors(dark);
                require(held.pixelColor(ui::px(4), ui::px(4)) == colors.pressedPaper, "Sync action uses the list pressed fill");
                int ink = 0;
                for (int y = ui::px(4); y < held.height() - ui::px(4); ++y)
                    for (int x = ui::px(24); x < held.width() - ui::px(24); ++x)
                        ink += held.pixelColor(x, y) == colors.ink;
                require(ink > 10, "Sync action retains contrasting label ink while pressed");
                QTest::mouseRelease(action, Qt::LeftButton, Qt::NoModifier, QPoint(-1, -1));
                require(action->grab().toImage() == normal, "Cancelled Sync action restores its appearance");
            }
            page.setNativeCloudSync(false);
            page.setAccount({}, {}, false, true);
            for (const auto *field : {"rrKosyncServer", "rrKosyncUsername", "rrKosyncPassword"}) {
                page.findChild<QPushButton *>(field)->click();
                QApplication::processEvents();
                auto *dialog = page.editorDialog.data();
                auto *input = dialog->findChild<QLineEdit *>("rrKosyncInput");
                auto *panel = dialog->findChild<QWidget *>("widgetKeyboard");
                auto *first = panel->findChild<QPushButton *>("widgetKey1_0");
                auto *second = panel->findChild<QPushButton *>("widgetKey1_1");
                // The server field starts with its required scheme.
                const QString prefix = QString(field) == "rrKosyncServer" ? "https://" : "";
                for (int i = 0; i < 3; ++i) QTest::mouseClick(first, Qt::LeftButton);
                require(input->text() == prefix + "qqq", "real keyboard enters the account draft");

                const auto left = first->mapTo(panel, QPoint(first->width(), first->height() / 2));
                const auto right = second->mapTo(panel, QPoint(0, second->height() / 2));
                require(right.x() > left.x(), "keyboard regression targets an actual gap between keys");
                const auto gap = (left + right) / 2;
                auto *gapTarget = panel->childAt(gap);
                if (!gapTarget) gapTarget = panel;
                QTest::mouseClick(gapTarget, Qt::LeftButton, Qt::NoModifier, gapTarget->mapFrom(panel, gap));
                require(page.editorDialog && input->text() == prefix + "qqq", "a key gap does not dismiss the account draft");
                QTest::mouseClick(panel, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
                require(page.editorDialog, "keyboard padding does not dismiss the editor");

                QTest::mousePress(first, Qt::LeftButton);
                QTest::mouseRelease(first, Qt::LeftButton, Qt::NoModifier, QPoint(-1, -1));
                require(page.editorDialog && input->text() == prefix + "qqq", "releasing outside a key keeps the draft open");
                QTest::mousePress(first, Qt::LeftButton);
                QEvent cancel(QEvent::TouchCancel);
                QApplication::sendEvent(first, &cancel);
                QTest::mouseRelease(first, Qt::LeftButton, Qt::NoModifier, QPoint(-1, -1));
                require(page.editorDialog && input->text() == prefix + "qqq", "a cancelled key release keeps the draft open");

                QTest::mousePress(first, Qt::LeftButton);
                QTest::mousePress(second, Qt::LeftButton);
                QTest::mouseRelease(first, Qt::LeftButton);
                require(page.editorDialog, "overlapping key presses do not dismiss the editor");
                QTest::mouseRelease(second, Qt::LeftButton);
                require(input->text() == prefix + "qqqw", "the remaining pressed key still enters its character");
                for (int i = 0; i < 100; ++i) QTest::mouseClick(first, Qt::LeftButton);
                require(page.editorDialog && input->text() == prefix + "qqqw" + QString(100, QChar('q')),
                        "rapid key clicks continue editing after gaps and cancelled presses");
                if (QString(field) == "rrKosyncPassword") {
                    input->clear();
                    auto *shift = panel->findChild<QPushButton *>("widgetKeyboardShift");
                    auto *reveal = dialog->findChild<QPushButton *>("rrKosyncPasswordReveal");
                    QTest::mouseClick(shift, Qt::LeftButton);
                    QTest::mouseClick(first, Qt::LeftButton);
                    QTest::mouseClick(second, Qt::LeftButton);
                    require(input->text() == "Qw" && input->echoMode() == QLineEdit::Password && !shift->isChecked(),
                            "masked passwords consume Shift after one letter");
                    reveal->click();
                    require(input->displayText() == "Qw", "Show reveals the exact mixed-case draft");
                    shift->click(); shift->click();
                    const auto lockedIcon = shift->icon().cacheKey();
                    require(shift->accessibleName() == strings::get("keyboard.caps_lock"), "locked Shift has an explicit accessible name");
                    QTest::mouseClick(first, Qt::LeftButton);
                    QTest::mouseClick(second, Qt::LeftButton);
                    reveal->click();
                    require(input->text() == "QwQW" && input->echoMode() == QLineEdit::Password && shift->isChecked(),
                            "Caps Lock remains active through password reveal and hiding");
                    require(shift->icon().cacheKey() == lockedIcon, "typing while locked keeps the lock icon");
                    dialog->findChild<QPushButton *>("rrKosyncEditSave")->click();
                    QApplication::processEvents();
                    require(page.takePassword() == "QwQW", "saving passes the exact mixed-case password to the account owner");
                    page.findChild<QPushButton *>(field)->click();
                    dialog = page.editorDialog.data();
                    shift = dialog->findChild<QPushButton *>("widgetKeyboardShift");
                    require(!shift->isChecked(), "a new password editor starts in lowercase");
                }
                dialog->findChild<QPushButton *>("rrKosyncEditCancel")->click();
                require(!page.editorDialog, "Cancel still closes the account editor");
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
        }
    ui::setDensity(savedDensity);
}
}
namespace {
void checkPhoneSettings() {
    const int savedDensity = ui::density();
    for (int ppi : {212, 300}) {
        ui::setDensity(ppi);
        for (bool dark : {false, true}) {
            QWidget host;
            host.setProperty("reader_dark", dark);
            host.setPalette(ui::readerPalette(host.palette(), dark));
            ui::readerStyle(&host, ui::styles::reader());
            host.resize(ui::px(720), ui::px(1600));
            reader::SettingsPanel panel(&host, {});
            panel.refitRequested = [&] { panel.fitContent(host.size()); };
            panel.resize(host.size());
            panel.fitContent(host.size());
            host.show();
            panel.show();
            QApplication::processEvents();
            auto *title = panel.findChild<QLabel *>("rrSettingsTitle");
            require(title, "Settings has a title");
            const auto inkBounds = [](const QImage &image, QColor paper) {
                QRect bounds;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x)
                        if (image.pixelColor(x, y).alpha() && image.pixelColor(x, y) != paper) bounds |= QRect(x, y, 1, 1);
                return bounds;
            };
            const auto baseline = [&] {
                const auto shape = ui::renderPrivateText(title->text(), title->font());
                QRect ink;
                for (int y = 0; y < shape.image.height(); ++y)
                    for (int x = 0; x < shape.image.width(); ++x)
                        if (qAlpha(shape.image.pixel(x, y))) ink |= QRect(x, y, 1, 1);
                const auto shown = inkBounds(title->grab().toImage(), ui::themeColors(dark).paper);
                return shown.top() - ink.top() - shape.origin.y();
            };
            const int reference = baseline();
            const auto position = title->mapTo(&panel, title->contentsRect().topLeft());
            for (const char *name : {"rrSettingsDisplay", "rrSettingsControls", "rrSettingsDefaults", "rrSettingsStyling",
                                     "rrSettingsDictionaries", "rrSettingsSystem", "rrSettingsAbout", "rrSettingsKosync"}) {
                auto *row = panel.findChild<QPushButton *>(name);
                require(row && row->isVisible(), "Phone Settings section row is visible");
                const auto idle = row->grab().toImage();
                row->setDown(true);
                const auto held = row->grab().toImage();
                require(held.pixelColor(5, 5) == ui::themeColors(dark).pressedPaper,
                        "Phone Settings section rows use the shared pressed gray");
                const auto countInk = [&](QRect area) {
                    int count = 0;
                    for (int y = area.top(); y <= area.bottom(); ++y)
                        for (int x = area.left(); x <= area.right(); ++x)
                            count += qAbs(qGray(held.pixel(x, y)) - qGray(ui::themeColors(dark).ink.rgb())) < 64;
                    return count;
                };
                require(countInk(QRect(0, 4, held.width() / 2, held.height() - 8)) > 30,
                        "Pressed settings rows keep visible label and icon ink");
                auto *chevron = row->findChild<QWidget *>("rrSettingsSectionChevron");
                require(chevron && countInk(chevron->geometry()) > 5,
                        "Pressed settings rows keep a visible chevron");
                require(held.pixelColor(chevron->x(), chevron->y()) == ui::themeColors(dark).pressedPaper,
                        "Settings chevrons do not cover the pressed gray with a paper rectangle");
                const auto directory = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
                const auto out = directory.isEmpty() ? QString() : directory + "/" + QString::number(ppi) + "-" + name;
                if (!out.isEmpty()) require(panel.grab().save(out + (dark ? "-held-dark.png" : "-held-light.png")), "save held Settings row");
                row->setDown(false);
                require(row->grab().toImage() == idle, "Releasing the settings row restores its original appearance");
                row->click();
                panel.fitContent(host.size());
                QApplication::processEvents();
                require(title->mapTo(&panel, title->contentsRect().topLeft()) == position,
                        "Settings submenu titles keep the same text origin");
                require(baseline() == reference, "Settings submenu titles keep the Settings baseline");
                if (!out.isEmpty()) require(panel.grab().save(out + (dark ? "-dark.png" : "-light.png")), "save Settings submenu");
                panel.findChild<QPushButton *>("rrSettingsBack")->click();
                panel.fitContent(host.size());
                QApplication::processEvents();
            }
        }
    }
    ui::setDensity(savedDensity);
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setPalette(ui::readerPalette(app.palette(), false));
    ui::setInterfaceWeight(ui::heavierTextSteps);
    require(ui::initializeInterfaceFonts(test::readerAssets()), "load the production interface fonts");
    checkPhoneSettings();
    checkKeyboardInputStaysInEditor();
    {
        QWidget host;
        host.resize(1072, 1448);
        host.show();
        reader::KosyncNotice notice(&host);
        notice.show();
        notice.fitCard();
        require(notice.primaryButton()->isVisible(), "in-flight sync offers cancellation");
        int completions = 0;
        notice.completed = [&] { ++completions; notice.hide(); };
        QElapsedTimer elapsed;
        elapsed.start();
        notice.complete();
        require(notice.completing(), "success has a pending automatic dismissal");
        require(notice.findChild<QLabel *>("rrModalTitle")->text() == strings::get("kosync.completed") &&
                notice.descriptionLabel()->text() == strings::get("kosync.up_to_date") &&
                !notice.primaryButton()->isVisible(), "successful sync shows confirmation without a button");
        QTest::qWait(1000);
        require(notice.isVisible() && completions == 0, "success does not disappear immediately");
        require(QTest::qWaitFor([&] { return completions == 1; }, 2000) && elapsed.elapsed() >= 1500,
                "success automatically dismisses after at least 1.5 seconds");
        notice.show();
        notice.complete();
        notice.hide();
        require(!notice.completing(), "hidden notices release their pending confirmation");
        QTest::qWait(1600);
        require(completions == 1, "hiding the success notice cancels its dismissal timer");
    }
    {
        const int savedDensity = ui::density();
        for (int ppi : {212, 300})
            for (bool dark : {false, true}) {
                ui::setDensity(ppi);
                QWidget host;
                host.setProperty("reader_dark", dark);
                host.setFont(ui::interfaceFont());
                ui::applyReaderPalette(&host, ui::readerPalette(host.palette(), dark));
                ui::readerStyle(&host, ui::styles::reader());
                int destroyed = 0;
                reader::KosyncPage page(&host, &host, [&](QWidget *parent, QLineEdit *) {
                    return std::make_unique<Keyboard>(destroyed, parent);
                });
                host.resize(ui::px(800), ui::px(2000));
                page.resize(host.size());
                host.show();
                page.show();
                for (const auto *name : {"rrKosyncLogin", "rrKosyncNow", "rrKosyncLogout", "rrKosyncLearnMore"}) {
                    auto *action = page.findChild<QPushButton *>(name);
                    require(action, "Sync action is missing");
                    action->setEnabled(true);
                    action->show();
                    QApplication::processEvents();
                    action->ensurePolished();
                    action->resize(ui::px(700), ui::metrics::chrome::actionHeight);
                    QImage image(action->size(), QImage::Format_RGB32);
                    image.fill(dark ? Qt::black : Qt::white);
                    action->render(&image);
                    QRect ink;
                    const auto area = image.rect().adjusted(ui::px(8), ui::px(8), -ui::px(8), -ui::px(8));
                    for (int y = area.top(); y <= area.bottom(); ++y)
                        for (int x = area.left(); x <= area.right(); ++x)
                            if (dark ? qGray(image.pixel(x, y)) > 192 : qGray(image.pixel(x, y)) < 64)
                                ink |= QRect(x, y, 1, 1);
                    require(!ink.isEmpty() && qAbs(ink.left() - int(ui::metrics::chrome::inset)) <= ui::px(4),
                            "Sync action label is not left-aligned with the Settings rows");
                    require(qAbs(ink.center().y() - image.rect().center().y()) <= 2,
                            "Sync action label is not vertically centered");
                    const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
                    if (!output.isEmpty()) {
                        require(QDir().mkpath(output), "Cannot create Sync capture directory");
                        require(image.save(output + QString("/%1-%2-%3.png").arg(name).arg(ppi).arg(dark ? "dark" : "light")),
                                "Cannot save Sync action capture");
                    }
                }
                page.findChild<QPushButton *>("rrKosyncPassword")->click();
                auto *reveal = page.editorDialog->findChild<QPushButton *>("rrKosyncPasswordReveal");
                auto *input = page.editorDialog->findChild<QLineEdit *>("rrKosyncInput");
                input->setText("sample-password");
                for (const auto size : {QSize(800, 1136), QSize(1136, 800)}) {
                    host.resize(ui::px(size.width()), ui::px(size.height()));
                    page.resize(host.size());
                    QApplication::processEvents();
                    for (bool shown : {false, true}) {
                        reveal->setChecked(shown);
                        QApplication::processEvents();
                        const auto *card = page.editorDialog->card();
                        require(card->geometry().bottom() < host.height() / 2,
                                "password editor fits above the keyboard in both themes, densities and orientations");
                        require(input->width() > ui::px(100) && input->geometry().right() < reveal->geometry().left(),
                                "reveal control leaves room for password input");
                        const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
                        if (!output.isEmpty())
                            require(host.grab().save(output + QString("/password-%1-%2-%3-%4.png")
                                .arg(ppi).arg(dark ? "dark" : "light").arg(size.width()).arg(shown ? "shown" : "masked")),
                                    "save password editor capture");
                    }
                }
            }
        ui::setDensity(savedDensity);
    }
    QWidget window;
    window.setFont(ui::interfaceFont());
    window.resize(800, 1136);
    // Nickel applies its own button constraints before Kobalt opens.
    ui::readerStyle(&window, ui::styles::reader() + "QPushButton{min-height:60px;max-height:60px;}");
    int destroyed = 0;
    Keyboard *keyboard = nullptr;
    reader::SettingsPanelInputs inputs;
    inputs.keyboard = [&](QWidget *host, QLineEdit *) {
        auto controller = std::make_unique<Keyboard>(destroyed, host);
        keyboard = controller.get();
        return controller;
    };
    reader::SettingsPanel panel(&window, inputs);
    panel.resize(window.size());
    auto *tab = panel.findChild<QPushButton *>("rrSettingsKosync");
    require(tab && panel.settingsPages->widget(reader::KosyncSettings) == panel.kosync,
            "Sync has its own Settings tab on desktop");
    panel.show();
    window.show();
    tab->click();
    panel.fitContent(window.size());
    QApplication::processEvents();
    // A narrow panel lists its sections without a selection; the rail checks the open one.
    require(panel.settingsPages->currentIndex() == reader::KosyncSettings && (tab->isChecked() || !tab->isCheckable()),
            "Sync tab selects its page and check state");
    auto *about = panel.findChild<QPushButton *>("rrSettingsAbout");
    require(about && tab->mapTo(&panel, QPoint()).y() < about->mapTo(&panel, QPoint()).y(),
            "Sync appears above About in the Settings rail");
    auto *page = panel.kosync;
    page->setDeviceName({}, "Kobo Clara BW (Kobalt)");
    auto *server = page->findChild<QPushButton *>("rrKosyncServer");
    auto *password = page->findChild<QPushButton *>("rrKosyncPassword");
    auto *login = page->findChild<QPushButton *>("rrKosyncLogin");
    auto *automatic = page->findChild<QCheckBox *>("rrKosyncAutomatic");
    auto *sync = page->findChild<QPushButton *>("rrKosyncNow");
    auto *logout = page->findChild<QPushButton *>("rrKosyncLogout");
    require(server && password && login && automatic && sync && logout, "all sync controls exist");
    require(server->height() == ui::metrics::chrome::actionHeight &&
            password->height() == ui::metrics::chrome::actionHeight &&
            login->height() == ui::metrics::chrome::actionHeight &&
            sync->height() == ui::metrics::chrome::actionHeight,
            "account fields and actions keep Settings row heights instead of collapsing");
    require(!automatic->isEnabled() && !sync->isEnabled() && !logout->isEnabled(), "signed-out actions are disabled");
    require(page->server().isEmpty(), "fresh account has no prefilled server");
    server->click();
    require(page->editorDialog && keyboard, "server editor uses the host keyboard");
    require(keyboard->submitText == "OK", "account editor labels native submission as OK rather than Search");
    auto *editor = page->editorDialog->findChild<QLineEdit *>("rrKosyncInput");
    require(editor, "server editor is accessible");
    require(editor->text() == "https://" && editor->cursorPosition() == editor->text().size(),
            "empty server field starts with the required scheme and the cursor after it");
    // Native keys fall back to their nearest ancestor with click focus.
    page->editorDialog->setFocus(Qt::MouseFocusReason);
    QApplication::processEvents();
    require(editor->hasFocus(), "a native key's ancestor focus fallback keeps the server input focused");
    require(QFontInfo(editor->font()).family() == ui::interfaceFont().family(),
            "input inherits the production UI font rather than the system font");
    ui::ChoiceModal reference(&window, {"A regular alert.", "Alert", {}},
                              {"Cancel", "rrReferenceCancel"}, {"OK", "rrReferenceOK"});
    reference.fitCard();
    reference.card()->rows()->activate();
    auto *card = page->editorDialog->card();
    auto *cancel = card->findChild<QPushButton *>("rrKosyncEditCancel");
    auto *save = card->findChild<QPushButton *>("rrKosyncEditSave");
    require(QFontInfo(save->font()).family() == ui::interfaceFont().family() &&
            QFontInfo(card->findChild<QLabel *>("rrModalTitle")->font()).family() == ui::interfaceFont().family(),
            "dialog title and actions inherit the production UI font");
    require(card->rows()->contentsMargins() == reference.card()->rows()->contentsMargins() &&
            card->rows()->spacing() == reference.card()->rows()->spacing(),
            "input dialog uses the regular alert frame margins and row spacing");
    require(cancel->geometry().left() == reference.secondaryButton()->geometry().left() &&
            save->geometry().right() == reference.primaryButton()->geometry().right() &&
            cancel->geometry().bottom() == card->height() - 3,
            "Cancel and OK dividers meet the alert frame without outer padding");
    require(editor->x() == cancel->x() && editor->width() == cancel->width() + save->width(),
            "input occupies the alert content band without an inset box");
    if (argc > 1) {
        reference.show();
        reference.raise();
        require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-alert.png"), "save regular alert reference");
        reference.hide();
    }
    require(page->editorDialog->card()->geometry().bottom() < window.height() / 2,
            "entire text entry card stays above the Kobo keyboard");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-editor.png"), "save editor capture");
    for (const auto size : {QSize(1072, 1448), QSize(1448, 1072), QSize(800, 1136)}) {
        window.resize(size);
        panel.resize(size);
        panel.fitContent(size);
        QApplication::processEvents();
        require(page->editorDialog->card()->geometry().bottom() < size.height() / 2,
                "text entry remains above keyboard after Clara portrait and landscape resizes");
    }
    editor->setText("https://sync.example/prefix");

    keyboard->submitted();
    require(destroyed == 0, "keyboard survives its own submission callback");
    QApplication::processEvents();
    require(!page->editorDialog && destroyed == 1 && page->server() == "https://sync.example/prefix",
            "server edit commits and releases keyboard after submission");
    password->click();
    editor = page->editorDialog->findChild<QLineEdit *>("rrKosyncInput");
    page->editorDialog->setFocus(Qt::MouseFocusReason);
    QApplication::processEvents();
    require(editor->hasFocus(), "recreated password editor keeps the native keyboard's focus fallback");
    require(editor->echoMode() == QLineEdit::Password && editor->inputMethodHints().testFlag(Qt::ImhSensitiveData),
            "password entry hides characters and disables keyboard learning");
    editor->setText("private-test-password");
    auto *reveal = page->editorDialog->findChild<QPushButton *>("rrKosyncPasswordReveal");
    require(reveal && !reveal->isChecked() && reveal->text() == "Show" && reveal->accessibleName() == "Show password",
            "password reveal is explicit and masked by default");
    const auto *originalKeyboard = keyboard;
    editor->setSelection(3, 5);
    const int cursor = editor->cursorPosition();
    const int selection = editor->selectionStart();
    const auto inputBounds = editor->geometry();
    const auto revealBounds = reveal->geometry();
    QTest::mouseClick(reveal, Qt::LeftButton);
    QApplication::processEvents();
    require(reveal->isChecked() && reveal->text() == "Hide" && reveal->accessibleName() == "Hide password" &&
            editor->displayText() == editor->text(), "Show reveals the existing password and updates its action");
    require(editor->hasFocus() && keyboard == originalKeyboard && destroyed == 1 &&
            editor->cursorPosition() == cursor && editor->selectionStart() == selection && editor->selectedText() == "vate-",
            "tapping Show preserves focus, keyboard, cursor and selection");
    require(editor->geometry() == inputBounds && reveal->geometry() == revealBounds,
            "changing Show to Hide does not move or resize the input");
    require(editor->inputMethodHints().testFlag(Qt::ImhSensitiveData) &&
            editor->inputMethodHints().testFlag(Qt::ImhNoPredictiveText) &&
            editor->inputMethodHints().testFlag(Qt::ImhNoAutoUppercase), "revealed passwords retain sensitive input hints");
    QTest::mouseClick(reveal, Qt::LeftButton);
    require(editor->echoMode() == QLineEdit::Password && editor->text() == "private-test-password",
            "Hide masks the same password without changing it");
    reveal->click();
    QTest::keyClicks(editor, "x");
    require(editor->text() == "prixtest-password", "typing after revealing replaces the retained selection");
    editor->setText("private-test-password");
    QMetaObject::invokeMethod(qApp, "applicationStateChanged", Qt::DirectConnection,
                              Q_ARG(Qt::ApplicationState, Qt::ApplicationInactive));
    require(editor->echoMode() == QLineEdit::Password && !reveal->isChecked(), "backgrounding masks the password");
    reveal->click();
    page->editorDialog->hide();
    require(editor->echoMode() == QLineEdit::Password && !reveal->isChecked(), "hiding the editor masks the password");
    page->editorDialog->show();
    reveal->click();
    keyboard->submitted();
    require(editor->echoMode() == QLineEdit::Password, "submission masks before deferred editor deletion");
    QApplication::processEvents();
    password->click();
    editor = page->editorDialog->findChild<QLineEdit *>("rrKosyncInput");
    reveal = page->editorDialog->findChild<QPushButton *>("rrKosyncPasswordReveal");
    require(editor->echoMode() == QLineEdit::Password && !reveal->isChecked() && editor->text() == "private-test-password",
            "reopening keeps the saved input masked");
    reveal->click();
    editor->setText("cancelled-password");
    page->editorDialog->findChild<QPushButton *>("rrKosyncEditCancel")->click();
    QApplication::processEvents();
    QString submitted;
    page->loginRequested = [&] { submitted = page->takePassword(); };
    login->click();
    require(submitted == "private-test-password" && page->takePassword().isEmpty(), "login consumes and clears password");
    page->setAccount("https://sync.example/prefix", "reader", true, true);
    require(automatic->isEnabled() && automatic->isChecked() && sync->isEnabled() && logout->isEnabled(),
            "signed-in account enables sync controls");
    auto *lastSync = page->findChild<QLabel *>("rrKosyncLastSync");
    require(lastSync && lastSync->isVisible() && lastSync->text() == "Last synced: Never",
            "new account clearly has no recorded sync");
    page->setLastSync(QDateTime::fromString("2026-10-25T09:42:00Z", Qt::ISODate));
    require(lastSync->text().startsWith("Last synced: ") && !lastSync->text().contains("Never"),
            "successful sync has a local date and time");
    auto *deviceName = page->findChild<QPushButton *>("rrKosyncDeviceName");
    require(deviceName && deviceName->isVisible(), "sync device name is editable");
    QString renamed;
    page->deviceNameChanged = [&](const QString &value) { renamed = value; };
    deviceName->click();
    editor = page->editorDialog->findChild<QLineEdit *>("rrKosyncInput");
    require(editor->placeholderText() == "Kobo Clara BW (Kobalt)" && editor->maxLength() == 128 &&
            page->editorDialog->card()->geometry().bottom() < window.height() / 2,
            "device name editor shows its default and stays above the keyboard");
    editor->setText("Living room Kobo");
    keyboard->submitted();
    QApplication::processEvents();
    require(renamed == "Living room Kobo" && automatic->isEnabled() && !login->isVisible(),
            "renaming reaches the owner without requiring account authentication");
    deviceName->click();
    page->editorDialog->findChild<QLineEdit *>("rrKosyncInput")->clear();
    keyboard->submitted();
    QApplication::processEvents();
    require(renamed.isEmpty(), "empty device name requests the automatic default");
    auto *matching = dynamic_cast<ui::CaptionButton *>(page->findChild<QPushButton *>("rrKosyncMatching"));
    require(matching && matching->caption() == "Binary (default)", "matching defaults to Binary");
    int matchingChanges = 0;
    page->filenameMatchingChanged = [&](bool filename) { ++matchingChanges; page->setFilenameMatching(filename); };
    matching->click();
    auto *matchingDialog = dynamic_cast<ui::NoticeModal *>(window.findChildren<QWidget *>("rrKosyncMatchingDialog").last());
    require(matchingDialog && matchingDialog->isVisible() && window.rect().contains(matchingDialog->card()->geometry()),
            "matching selector fits on the Kobo screen");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-matching.png"), "save document matching selector");
    auto *filename = matchingDialog->findChild<QAbstractButton *>("rrKosyncFilename");
    require(filename, "filename option is available");
    filename->click();
    require(matchingChanges == 1 && matching->caption() == "Filename" && !matchingDialog->isVisible(), "filename selection reaches the owner");
    page->setStatus(strings::get("kosync.matching_changed"), false);
    panel.fitContent(window.size());
    QApplication::processEvents();
    require(lastSync->visibleRegion().boundingRect() == lastSync->rect(), "last sync remains visible after a matching change");
    matching->click();
    matchingDialog = dynamic_cast<ui::NoticeModal *>(window.findChildren<QWidget *>("rrKosyncMatchingDialog").last());
    matchingDialog->primaryButton()->click();
    require(matchingChanges == 1 && matching->caption() == "Filename", "Cancel leaves matching unchanged");
    matching->click();
    matchingDialog = dynamic_cast<ui::NoticeModal *>(window.findChildren<QWidget *>("rrKosyncMatchingDialog").last());
    matchingDialog->findChild<QAbstractButton *>("rrKosyncBinary")->click();
    require(matchingChanges == 2 && matching->caption() == "Binary (default)", "binary selection can be restored");
    matching->click();
    QPointer<QWidget> dismissedMatching = window.findChildren<QWidget *>("rrKosyncMatchingDialog").last();
    page->dismissEditor();
    require(!dismissedMatching || !dismissedMatching->isVisible(), "sleep cleanup dismisses matching choices");
    page->setStatus("Syncing progress…", true);
    require(!login->isEnabled() && !server->isEnabled() && !automatic->isEnabled() && !matching->isEnabled(), "busy state blocks overlapping edits");
    page->setStatus("Reading progress is up to date.", false);
    panel.fitContent(window.size());
    QApplication::processEvents();
    QElapsedTimer settled;
    settled.start();
    while (logout->visibleRegion().boundingRect() != logout->rect() && settled.elapsed() < 1000)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    require(!password->isVisible() && !login->isVisible(), "signed-in page hides sign-in controls");
    require(sync->visibleRegion().boundingRect() == sync->rect() &&
            logout->visibleRegion().boundingRect() == logout->rect(), "signed-in controls fit on one page");
    require(lastSync->visibleRegion().boundingRect() == lastSync->rect(), "last sync fits on the signed-in page");
    auto *status = page->findChild<QLabel *>("rrKosyncStatus");
    require(status->palette().color(QPalette::WindowText) == ui::noteColor(status) &&
            lastSync->palette().color(QPalette::WindowText) == ui::noteColor(lastSync),
            "sync status and last sync use the same ink as list descriptions");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1])), "save account settings capture");
    page->setNativeCloudSync(true);
    page->setAccount("https://sync.example/prefix", "reader", true, true);
    page->setStatus("Reading progress is up to date.", false);
    panel.fitContent(window.size());
    QApplication::processEvents();
    auto *disclaimer = page->findChild<QLabel *>("rrKosyncDescription");
    // A narrow panel shows an open section without its list, so the tab is
    // not on screen; it must still exist rather than be hidden.
    require(!tab->isHidden() && disclaimer && disclaimer->isVisible() &&
            disclaimer->text().contains("Kobo’s built-in sync system") &&
            page->findChild<QLabel *>("rrKosyncSideloadGuidance")->text().contains("DRM-free copy"), "Kobo-managed books retain a Sync tab with a sideloading disclaimer");
    for (const auto *control : {server, page->findChild<QPushButton *>("rrKosyncUsername"), password, login,
                                static_cast<QPushButton *>(sync), logout, deviceName, static_cast<QPushButton *>(matching)})
        require(control && !control->isVisible(), "Kobo-managed books hide all account and sync actions");
    require(!automatic->isVisible() && !lastSync->isVisible() &&
            !page->findChild<QLabel *>("rrKosyncStatus")->isVisible(), "Kobo-managed books hide automatic sync and account status");
    require(disclaimer->visibleRegion().boundingRect() == disclaimer->rect(), "Kobo sync disclaimer fits on one page");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-kobo-managed.png"), "save Kobo-managed disclaimer capture");
    auto *guidance = page->findChild<QLabel *>("rrKosyncSideloadGuidance");
    auto *learnMore = page->findChild<QPushButton *>("rrKosyncLearnMore");
    require(guidance->isVisible() && learnMore && learnMore->isVisible() &&
            guidance->geometry().top() >= disclaimer->geometry().bottom(), "sideloading guidance and Learn More sit below the disclaimer");
    learnMore->click();
    auto *help = dynamic_cast<ui::NoticeModal *>(window.findChild<QWidget *>("rrKosyncHelp"));
    require(help && help->isVisible() && help->descriptionLabel()->text().contains("Only reading progress"), "Learn More explains Kobo ownership and KOsync scope");
    require(window.rect().contains(help->card()->geometry()), "sync explanation fits in the screen");
    if (argc > 1) require(window.grab().save(QString::fromLocal8Bit(argv[1]) + "-learn-more.png"), "save sync explanation capture");
    help->primaryButton()->click();
    require(!help->isVisible(), "OK dismisses the sync explanation");
    learnMore->click();
    QPointer<QWidget> reopenedHelp = window.findChildren<QWidget *>("rrKosyncHelp").last();
    require(reopenedHelp && reopenedHelp->isVisible(), "sync explanation can be reopened");
    panel.settingsPages->setCurrentIndex(reader::DisplaySettings);
    require(!reopenedHelp || !reopenedHelp->isVisible(), "leaving Sync dismisses its explanation");
    tab->click();
    page->setNativeCloudSync(false);
    page->setAccount("https://sync.example/prefix", "reader", true, true);
    require(server->isVisible() && sync->isVisible() && automatic->isVisible(), "sideloaded books retain normal KOsync controls");
    bool autoChanged = false;
    page->automaticChanged = [&](bool value) { autoChanged = !value; };
    automatic->click();
    require(autoChanged, "automatic sync toggle reaches the owner");
    page->setAccount("https://sync.example/prefix", "reader", true, false);
    require(automatic->accessibleDescription() == strings::get("kosync.manual_description"),
            "manual mode shows its toolbar guidance");
    page->setStatus({}, true);
    require(!automatic->isEnabled() && !sync->isEnabled(), "busy sync blocks manual controls");
    page->setNativeCloudSync(true);
    require(!automatic->isVisible() && !sync->isVisible(), "Kobo-managed books hide manual sync controls");
    page->setNativeCloudSync(false);
    page->setAccount({}, {}, false, false);
    panel.fitContent(window.size());
    QApplication::processEvents();
    password->click();
    require(page->editorDialog, "signed-out password editor opens");
    panel.settingsPages->setCurrentIndex(reader::DisplaySettings);
    require(!page->editorDialog && page->takePassword().isEmpty(), "leaving Sync dismisses keyboard and clears secrets");
    tab->click();
    server->click();
    editor = page->editorDialog->findChild<QLineEdit *>("rrKosyncInput");
    require(editor->text() == "https://", "a cleared account prefills the server scheme again");
    keyboard->submitted();
    QApplication::processEvents();
    require(page->server().isEmpty() &&
            dynamic_cast<ui::CaptionButton *>(server)->caption() == strings::get("kosync.not_set"),
            "saving only the prefilled scheme leaves the server unset");
    return 0;
}
