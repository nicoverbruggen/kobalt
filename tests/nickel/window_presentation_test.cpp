#include "platform/nickel/common/window_presentation.h"
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QTest>
#include <QWidget>

class MainWindowView : public QWidget {
    Q_OBJECT
};
class AbstractPowerView : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
};

class PaintCounter : public QObject {
public:
    int paints = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint)
            ++paints;
        return false;
    }
};

class WindowPresentationTest : public QObject {
    Q_OBJECT
private slots:
    void batteryUpdatesReuseOnlyAnExistingVisibleSleepFrame() {
        QWidget reader;
        MainWindowView background;
        background.resize(320, 480);
        AbstractPowerView power(&background);
        power.setGeometry(background.rect());
        background.show();
        QApplication::processEvents();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        QImage image(power.size(), QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(!presentation->updateSleepScreen(image, image.rect()));
        QVERIFY(presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        QPointer<QWidget> frame = power.findChild<QWidget *>("rrSleepFrame");
        QVERIFY(frame);
        image.fill(Qt::white);
        QVERIFY(presentation->updateSleepScreen(image, image.rect()));
        QCOMPARE(power.findChild<QWidget *>("rrSleepFrame"), frame.data());
        QCOMPARE(frame->grab().toImage().pixelColor(150, 250), QColor(Qt::white));
        power.hide();
        QVERIFY(!presentation->updateSleepScreen(image, image.rect()));
        presentation->clearSleepScreen();
        QVERIFY(!frame);
        power.show();
        QVERIFY(!presentation->updateSleepScreen(image, image.rect()));
        QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
    }

    void unsupportedHostsKeepTheNativeSleepScreen() {
        QWidget reader;
        MainWindowView background;
        background.resize(320, 480);
        AbstractPowerView power(&background);
        power.setGeometry(background.rect());
        background.show();
        QApplication::processEvents();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, false);
        QVERIFY(!presentation->supportsSleepScreen());
        QImage image(power.size(), QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(!presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
    }

    void sleepHandoffPaintsQueuedChildUpdatesOnce() {
        QWidget reader;
        MainWindowView background;
        background.resize(320, 480);
        AbstractPowerView power(&background);
        power.setGeometry(background.rect());
        background.show();
        reader.show();
        QApplication::processEvents();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        presentation->pauseBackground();
        QImage image(power.size(), QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        auto *frame = power.findChild<QWidget *>("rrSleepFrame");
        QVERIFY(frame);
        PaintCounter counter;
        frame->installEventFilter(&counter);
        frame->update();
        presentation->resumeAndRepaintBackground();
        QCOMPARE(counter.paints, 1);
        reader.hide();
        QApplication::processEvents();
        QCOMPARE(frame->grab().toImage().pixelColor(150, 250), QColor(Qt::gray));
    }

    void customSleepFrameStaysInsideNativePowerView() {
        QWidget reader;
        MainWindowView background;
        background.resize(320, 480);
        AbstractPowerView power(&background);
        power.setGeometry(background.rect());
        background.show();
        power.show();
        reader.show();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        QVERIFY(presentation->supportsSleepScreen());
        presentation->pauseBackground();
        QImage image(power.size(), QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        auto *frame = power.findChild<QWidget *>("rrSleepFrame");
        QVERIFY(frame);
        QCOMPARE(frame->parentWidget(), &power);
        QCOMPARE(frame->geometry(), power.rect());
        QVERIFY(frame->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(frame->focusPolicy(), Qt::NoFocus);
        presentation->resumeAndRepaintBackground();
        QCOMPARE(frame->grab().toImage().pixelColor(150, 250), QColor(Qt::gray));
        power.resize(480, 320);
        QCOMPARE(frame->geometry(), power.rect());
        // Native dialogs remain independent of the sleep image.
        QDialog pin(&background);
        pin.setModal(true);
        pin.show();
        QCOMPARE(QApplication::activeModalWidget(), &pin);
        QVERIFY(pin.isEnabled());
        QVERIFY(pin.updatesEnabled());
        QVERIFY(!frame->isAncestorOf(&pin));
        QVERIFY(presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        QCOMPARE(power.findChildren<QWidget *>("rrSleepFrame").size(), 1);
        presentation->clearSleepScreen();
        QVERIFY(!power.findChild<QWidget *>("rrSleepFrame"));
        QCOMPARE(QApplication::activeModalWidget(), &pin);
    }

    void customSleepFrameRejectsMissingOrAmbiguousPowerViews() {
        QWidget reader, unrelated;
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        QImage image(320, 480, QImage::Format_RGB32);
        image.fill(Qt::white);
        unrelated.show();
        QVERIFY(!presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        MainWindowView background;
        AbstractPowerView first(&background), second(&background);
        background.show();
        first.show();
        second.show();
        QVERIFY(!presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        QVERIFY(!background.findChild<QWidget *>("rrSleepFrame"));
        second.hide();
        QVERIFY(presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        QVERIFY(!presentation->presentSleepScreen({}, Qt::PrimaryOrientation));
        QVERIFY(!background.findChild<QWidget *>("rrSleepFrame"));
    }

    void nativeViewDestructionReleasesCustomFrame() {
        QWidget reader;
        MainWindowView background;
        auto *power = new AbstractPowerView(&background);
        background.show();
        power->show();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        QImage image(320, 480, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(presentation->presentSleepScreen(image, Qt::PrimaryOrientation));
        QPointer<QWidget> frame = power->findChild<QWidget *>("rrSleepFrame");
        QVERIFY(frame);
        delete power;
        QVERIFY(frame.isNull());
        presentation->clearSleepScreen();
        presentation.reset();
    }

    void pausePreservesReaderDescendantsAndPreviouslyDisabledWindows() {
        QWidget reader, background, hidden, disabled;
        QWidget child(&reader);
        QWidget childWindow(&reader, Qt::Window);
        reader.show();
        childWindow.show();
        background.show();
        disabled.show();
        disabled.setUpdatesEnabled(false);
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        presentation->pauseBackground();
        QVERIFY(reader.updatesEnabled());
        QVERIFY(child.updatesEnabled());
        // A top-level QWidget has its own backing store, even with a parent.
        QVERIFY(!childWindow.updatesEnabled());
        QVERIFY(!background.updatesEnabled());
        QVERIFY(hidden.updatesEnabled());
        QVERIFY(!disabled.updatesEnabled());
        presentation->pauseBackground();
        presentation->resumeBackground();
        QVERIFY(background.updatesEnabled());
        QVERIFY(childWindow.updatesEnabled());
        QVERIFY(!disabled.updatesEnabled());
        presentation->resumeBackground();
        QVERIFY(!disabled.updatesEnabled());
    }

    void wakePausesLibraryButKeepsNativeDialogsResponsive() {
        QWidget reader, pin, usb;
        MainWindowView library;
        reader.show();
        library.show();
        pin.show();
        usb.show();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        presentation->pauseBackground(true);
        QVERIFY(!library.updatesEnabled());
        QVERIFY(pin.updatesEnabled());
        QVERIFY(usb.updatesEnabled());
        QVERIFY(reader.updatesEnabled());
        presentation->resumeAndRepaintBackground();
        QVERIFY(library.updatesEnabled());
    }

    void destructionResumesSurvivingBackground() {
        QWidget reader, background;
        reader.show();
        background.show();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        presentation->pauseBackground();
        QVERIFY(!background.updatesEnabled());
        presentation.reset();
        QVERIFY(background.updatesEnabled());
    }

    void destroyedWindowsAreNotRetainedDuringResume() {
        auto *reader = new QWidget;
        auto *background = new QWidget;
        reader->show();
        background->show();
        auto presentation = kobalt::nickel::makeWindowPresentation(reader, true);
        presentation->pauseBackground();
        delete background;
        delete reader;
        presentation->pauseBackground();
        presentation->resumeAndRepaintBackground();
        presentation.reset();
    }

    void handoffCoverSurvivesDialogAcceptanceAndIsThenRemoved() {
        QWidget reader, background;
        background.resize(320, 480);
        QDialog dialog(&background);
        dialog.resize(240, 300);
        background.show();
        dialog.show();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        QPointer<QLabel> observed;
        int calls = 0;
        presentation->acceptHandoff(dialog, [&] {
            ++calls;
            observed = background.findChild<QLabel *>("rrHandoffCover");
            QVERIFY(observed);
            QVERIFY(observed->isVisible());
            QCOMPARE(observed->geometry(), background.rect());
            dialog.accept();
            QVERIFY(observed);
            QVERIFY(observed->isVisible());
        });
        QCOMPARE(calls, 1);
        QVERIFY(observed.isNull());
        QVERIFY(!background.findChild<QLabel *>("rrHandoffCover"));
    }

    void unparentedHandoffCallsContinuationOnce() {
        QWidget reader;
        QDialog dialog;
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        int calls = 0;
        presentation->acceptHandoff(dialog, [&] {
            ++calls;
            dialog.accept();
        });
        QCOMPARE(calls, 1);
        QVERIFY(!dialog.findChild<QLabel *>("rrHandoffCover"));
    }

    void continuationCanDestroyTheBackgroundAndItsCover() {
        QWidget reader;
        auto *background = new QWidget;
        auto *dialog = new QDialog(background);
        background->show();
        dialog->show();
        auto presentation = kobalt::nickel::makeWindowPresentation(&reader, true);
        QPointer<QLabel> cover;
        QPointer<QDialog> guardedDialog(dialog);
        presentation->acceptHandoff(*dialog, [&] {
            cover = background->findChild<QLabel *>("rrHandoffCover");
            QVERIFY(cover);
            delete background;
        });
        QVERIFY(cover.isNull());
        QVERIFY(guardedDialog.isNull());
    }
};

QTEST_MAIN(WindowPresentationTest)
#include "window_presentation_test.moc"
