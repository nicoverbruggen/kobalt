#include "input_checks.h"
#include "window.h"
#include "platforms/android/canvas.h"
#include "platforms/android/start_screen.h"
#include "interfaces/library/library_view.h"
#include "interfaces/reader/views/reading_page.h"
#include <QApplication>
#include <QDebug>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QThread>
#include <QWindow>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSet>
#include <QElapsedTimer>

namespace kobalt::kindle {
namespace {
class Pointer {
public:
    Pointer(Window &window, android::WidgetCanvas &canvas, QWidget &target, QPoint point)
        : window(window), canvas(canvas) {
        const auto contentPoint = target.mapTo(canvas.content, point);
        local = canvas.mapFromScene(canvas.content->graphicsProxyWidget()->mapToScene(contentPoint));
    }
    void send(QEvent::Type type, ulong timestamp, QPoint offset = {}) {
        const QPoint point = local + offset;
        const bool release = type == QEvent::MouseButtonRelease;
        QMouseEvent event(type, point, canvas.viewport()->mapToGlobal(point),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          release ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        event.setTimestamp(timestamp);
        QApplication::sendEvent(canvas.viewport(), &event);
    }
    void cancel() {
        QEvent event(QEvent::UngrabMouse);
        QApplication::sendEvent(window.windowHandle(), &event);
        send(QEvent::MouseButtonRelease, 0);
    }
private:
    Window &window;
    android::WidgetCanvas &canvas;
    QPoint local;
};
bool checkKeyboard(QLineEdit &query) {
    auto *panel = query.parentWidget()->findChild<QWidget *>("widgetKeyboard");
    if (!panel || !panel->isVisible()) {
        qCritical() << "Library search did not show its keyboard";
        return false;
    }
    QSignalBlocker blocked(query);
    query.clear();
    QString expected;
    QSet<QChar> characters;
    auto typePage = [&] {
        for (auto *key : panel->findChildren<QPushButton *>()) {
            const auto character = key->property("keyboard.character").toString();
            if (!key->isVisible() || character.isEmpty())
                continue;
            key->click();
            expected += character;
            characters.insert(character.front());
        }
    };
    auto click = [&](const char *name) {
        auto *key = panel->findChild<QPushButton *>(QString::fromLatin1(name));
        if (!key)
            return false;
        key->click();
        return true;
    };
    typePage();
    if (!click("widgetKeyboardShift")) return false;
    typePage();
    if (!click("widgetKeyboardSymbols")) return false;
    typePage();
    if (!click("widgetKeyboardSpace")) return false;
    expected += ' ';
    characters.insert(' ');
    for (ushort character = 32; character < 127; ++character)
        if (!characters.contains(QChar(character))) {
            qCritical() << "Kindle keyboard cannot enter ASCII character" << character;
            return false;
        }
    if (query.text() != expected || !click("widgetKeyboardDelete")) {
        qCritical() << "Keyboard inserted text differs from its keys" << query.text() << expected;
        return false;
    }
    expected.chop(1);
    if (query.text() != expected || !click("widgetKeyboardDone") || panel->isVisible()) {
        qCritical() << "Keyboard delete or Done did not complete";
        return false;
    }
    query.clear();
    return true;
}
}

bool checkLibraryInput(Window &window) {
    auto *start = window.findChild<android::StartScreen *>();
    if (!start || !start->libraryShown())
        return false;
    auto *view = start->library();
    auto *pages = view->findChild<QWidget *>("libraryPages");
    if (!pages || pages->size().isEmpty())
        return false;
    int opened = 0;
    QScopedValueRollback<std::function<void(const QString &)>> restore(view->openBook);
    view->openBook = [&](const QString &) { ++opened; };
    Pointer pointer(window, *start->libraryCanvas(), *pages, {100, 150});
    auto menuShown = [&] {
        auto *menu = view->findChild<QWidget *>("libraryBookMenu");
        return menu && menu->isVisible();
    };
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
        pointer.send(type, 0xfffffc18u);
        pointer.cancel();
        if (opened || menuShown()) {
            qCritical() << "Cancelled library contact opened a book or menu";
            return false;
        }
    }
    auto *search = view->findChild<QWidget *>("librarySearch");
    auto *closeSearch = view->findChild<QWidget *>("libraryCloseSearch");
    auto *query = view->findChild<QLineEdit *>("librarySearchQuery");
    if (!search || !closeSearch || !query)
        return false;
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
        pointer.send(type, 4000);
        window.simulatePowerEvent("goingToScreenSaver");
        pointer.cancel();
        pointer.send(QEvent::MouseButtonDblClick, 5000);
        pointer.send(QEvent::MouseButtonRelease, 5050);
        window.simulatePowerEvent("outOfScreenSaver");
        Pointer searchPointer(window, *start->libraryCanvas(), *search, search->rect().center());
        searchPointer.send(QEvent::MouseButtonPress, 6000);
        searchPointer.send(QEvent::MouseButtonRelease, 6050);
        if (opened || menuShown() || !query->isVisible()) {
            qCritical() << "Sleep cancellation retained the old canvas grab or accepted sleeping input";
            return false;
        }
        if (type == QEvent::MouseButtonPress && !checkKeyboard(*query)) {
            qCritical() << "Kindle keyboard entry or dismissal failed";
            return false;
        }
        Pointer closePointer(window, *start->libraryCanvas(), *closeSearch, closeSearch->rect().center());
        closePointer.send(QEvent::MouseButtonPress, 7000);
        closePointer.send(QEvent::MouseButtonRelease, 7050);
    }
    pointer.send(QEvent::MouseButtonPress, 0);
    QThread::msleep(650);
    pointer.send(QEvent::MouseButtonRelease, 85);
    if (opened != 1 || menuShown()) {
        qCritical() << "Queued library short tap did not open exactly once";
        return false;
    }
    pointer.send(QEvent::MouseButtonPress, 2000);
    pointer.send(QEvent::MouseButtonRelease, 3000);
    if (opened != 1 || !menuShown()) {
        qCritical() << "Queued library hold did not show its menu";
        return false;
    }
    view->dismiss();
    return true;
}

bool checkReaderInput(Window &window, ReadingPage &page) {
    auto *canvas = static_cast<android::WidgetCanvas *>(window.findChild<QGraphicsView *>("kindleReaderCanvas"));
    if (!canvas || canvas->content != page.window())
        return false;
    {
        QScopedValueRollback<headerfooter::Layout> restoreChrome(page.chrome);
        page.chrome.slotFields[2] = headerfooter::Field::BatteryPercent;
        const auto body = page.image;
        window.simulateBattery(43, false);
        if (!page.slotContents[2].text.contains("43")) {
            qCritical() << "Battery notification did not update the awake header";
            return false;
        }
        page.tapped(page.rect().center());
        auto *battery = page.window()->findChild<QWidget *>("rrMenuBattery");
        if (!battery || !battery->isVisible())
            return false;
        const auto before = battery->grab().toImage();
        window.simulateBattery(72, true);
        if (battery->grab().toImage() == before || page.image != body) {
            qCritical() << "Battery notification did not update the menu without changing the page body";
            return false;
        }
        page.tapped(page.rect().center());
        window.simulateBattery(44, false);
        if (!page.slotContents[2].text.contains("44"))
            return false;
    }
    auto *start = window.findChild<android::StartScreen *>();
    auto *libraryKeyboard = start ? start->library()->findChild<QWidget *>("widgetKeyboard") : nullptr;
    if (!libraryKeyboard || !libraryKeyboard->isHidden())
        return false;
    {
        QLineEdit foreign(&window);
        foreign.setGeometry(20, 20, 200, 40);
        foreign.show();
        foreign.setFocus();
        if (!libraryKeyboard->isHidden()) {
            qCritical() << "A reader editor opened the library keyboard";
            return false;
        }
    }
    int tapped = 0, held = 0, swiped = 0;
    QScopedValueRollback<std::function<void(QPoint)>> restoreTap(page.tapped), restoreHold(page.held);
    QScopedValueRollback<std::function<void(int)>> restoreSwipe(page.swiped);
    QScopedValueRollback<std::function<bool(QPoint, int)>> restoreSelection(page.annotationDragStarted);
    page.tapped = [&](QPoint) { ++tapped; };
    page.held = [&](QPoint) { ++held; };
    page.annotationDragStarted = [&](QPoint, int) { ++held; return false; };
    page.swiped = [&](int) { ++swiped; };
    Pointer pointer(window, *canvas, page, page.rect().center());
    pointer.send(QEvent::MouseButtonPress, 1000);
    window.simulatePowerEvent("goingToScreenSaver");
    pointer.cancel();
    auto *sleepView = window.findChild<QWidget *>("kindleSleepView");
    if (!sleepView || !sleepView->isVisible() || window.readerWindow()->isVisible()) {
        qCritical() << "Reader did not hand off to its sleep view";
        return false;
    }
    pointer.send(QEvent::MouseButtonDblClick, 2000);
    pointer.send(QEvent::MouseButtonRelease, 2050);
    window.simulatePowerEvent("outOfScreenSaver");
    QElapsedTimer wake;
    wake.start();
    while (!window.readerWindow()->readyForPresentation() && wake.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    if (!window.readerWindow()->isVisible() || !window.readerWindow()->readyForPresentation() ||
        window.readerWindow()->size() != QSize(1236, 1648) || sleepView->isVisible() || tapped || held || swiped) {
        qCritical() << "Reader wake did not restore its viewport or canceled input";
        return false;
    }
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
        pointer.send(type, 0xfffffc18u);
        pointer.send(QEvent::MouseMove, 0xfffffc30u, {450, 0});
        pointer.cancel();
        if (tapped || held || swiped) {
            qCritical() << "Cancelled reader contact completed a gesture";
            return false;
        }
    }
    pointer.send(QEvent::MouseButtonPress, 0);
    QThread::msleep(650);
    pointer.send(QEvent::MouseButtonRelease, 85);
    pointer.send(QEvent::MouseButtonPress, 2000);
    pointer.send(QEvent::MouseButtonRelease, 3000);
    if (tapped != 1 || held != 1 || swiped) {
        qCritical() << "Reader source-duration checks failed" << tapped << held << swiped;
        return false;
    }
    return true;
}
}
