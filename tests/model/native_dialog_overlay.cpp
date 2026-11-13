#ifndef Q_MOC_RUN
#  include "platforms/kobo/common/window_presentation.h"
#  include "platforms/kobo/common/native_dialog_overlay.h"
#  include "check.h"
#endif
#include <QApplication>
#include <QPushButton>
#include <QTest>
#include <QWindow>

class MainWindowView final : public QWidget
{
    Q_OBJECT
};

class ConfirmationDialog final : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;
};

static void modalReaderInput(Qt::WindowModality modality)
{
    MainWindowView nickel;
    nickel.resize(600, 800);
    nickel.show();
    QWidget reader(nullptr, Qt::Window | Qt::FramelessWindowHint);
    reader.setObjectName("rrReader");
    reader.resize(600, 800);
    reader.setWindowModality(Qt::ApplicationModal);
    reader.show();
    auto presentation = kobalt::nickel::makeWindowPresentation(&reader, false);
    int resumed = 0;
    presentation->inputResumed = [&] {
        require(!kobalt::nickel::NativeDialogOverlay::activeFor(&reader),
                "input resumed before the last native dialog released the reader");
        ++resumed;
    };
    QApplication::processEvents();
    require(QApplication::activeModalWidget() == &reader,
            "the reader must own application-modal input");

    ConfirmationDialog prompt(&nickel);
    prompt.setWindowModality(modality);
    prompt.setGeometry(50, 100, 400, 200);
    QPushButton skip("Skip", &prompt);
    skip.setGeometry(20, 120, 100, 50);
    QPushButton close("X", &prompt);
    close.setGeometry(340, 10, 40, 40);
    int dismissed = 0;
    const auto dismiss = [&] {
        ++dismissed;
        prompt.hide();
    };
    QObject::connect(&skip, &QPushButton::clicked, &prompt, dismiss);
    QObject::connect(&close, &QPushButton::clicked, &prompt, dismiss);
    const auto flags = prompt.windowFlags();
    for (auto *button : { &skip, &close }) {
        prompt.show();
        QApplication::processEvents();
        require(prompt.isWindow()
                        && prompt.windowHandle()->transientParent() == reader.windowHandle(),
                "the native update prompt must be presented above the reader");
        const int before = dismissed;
        // Firmware delivers input to the active modal window. Sending it
        // directly to the native button or its window bypasses the bug.
        auto *input = QApplication::activeModalWidget()->windowHandle();
        QTest::mouseClick(input, Qt::LeftButton, Qt::NoModifier,
                          input->mapFromGlobal(button->mapToGlobal(button->rect().center())));
        QApplication::processEvents();
        require(dismissed == before + 1 && !prompt.isVisible(),
                "Skip and X must dismiss a native prompt above an application-modal reader");
        require(prompt.parentWidget() == &nickel && prompt.windowFlags() == flags
                        && prompt.windowModality() == modality,
                "dismissal must restore native ownership, flags and original modality");
        require(!kobalt::nickel::NativeDialogOverlay::activeFor(&reader)
                        && QApplication::activeModalWidget() == &reader,
                "dismissal must return modal input to the reader");
        require(QTest::qWaitFor([&] { return resumed == dismissed; }),
                "closing a native dialog did not resume deferred reader work");
    }

    prompt.show();
    QApplication::processEvents();
    reader.hide();
    QApplication::processEvents();
    require(!prompt.isVisible() && prompt.windowModality() == modality
                    && prompt.parentWidget() == &nickel && prompt.windowFlags() == flags,
            "sleep handoff must restore the prompt's original modality and ownership");
    require(resumed == dismissed, "hiding the reader resumed deferred work");
}

static void overlappingDialogs()
{
    MainWindowView nickel;
    nickel.show();
    QWidget reader;
    reader.resize(600, 800);
    reader.show();
    auto presentation = kobalt::nickel::makeWindowPresentation(&reader, false);
    int resumed = 0;
    presentation->inputResumed = [&] { ++resumed; };
    ConfirmationDialog first(&nickel);
    first.show();
    require(QTest::qWaitFor(
                    [&] { return first.property("kobalt_native_dialog_overlay").toBool(); }),
            "the first native dialog was not presented");
    auto *second = new ConfirmationDialog(&nickel);
    second->show();
    require(QTest::qWaitFor(
                    [&] { return second->property("kobalt_native_dialog_overlay").toBool(); }),
            "the second native dialog was not presented");
    delete second;
    QApplication::processEvents();
    require(resumed == 0 && kobalt::nickel::NativeDialogOverlay::activeFor(&reader),
            "destroying one of two dialogs resumed the reader");
    first.hide();
    require(QTest::qWaitFor([&] { return resumed == 1; }),
            "closing the last native dialog did not resume the reader");

    auto *destroyed = new ConfirmationDialog(&nickel);
    destroyed->show();
    require(QTest::qWaitFor(
                    [&] { return destroyed->property("kobalt_native_dialog_overlay").toBool(); }),
            "the native dialog was not presented before destruction");
    delete destroyed;
    require(QTest::qWaitFor([&] { return resumed == 2; }),
            "destroying the last dialog did not resume the reader");

    first.show();
    require(QTest::qWaitFor(
                    [&] { return first.property("kobalt_native_dialog_overlay").toBool(); }),
            "the native dialog was not presented before teardown");
    presentation.reset();
    QApplication::processEvents();
    require(resumed == 2, "destroying the presentation resumed deferred work");
}

// Checks native-shaped dialogs above an application-modal reader: input through the active modal, dismissal and restore.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    for (const auto modality : { Qt::NonModal, Qt::WindowModal, Qt::ApplicationModal })
        modalReaderInput(modality);
    overlappingDialogs();
}

#include "native_dialog_overlay.moc"
