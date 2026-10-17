#include <cmath>
#include "variable_fonts.h"
#include "qt_test_shards.h"
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/settings/settings_panel.h"
#include "interfaces/reader/ui/style/theme.h"
#include "platform/desktop/window.h"
#include "platform/android/canvas.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "reading/reading_density.h"
#include "interfaces/reader/views/reading_page.h"
#include "interfaces/reader/settings/settings_panel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QApplication>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFile>
#include <QGraphicsView>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QScreen>
#include <QWindow>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace kobalt;
namespace {
AssetBundle *assets;
FontCopies *copies;
struct Fixture {
    QTemporaryDir directory;
    storage::Paths paths;
    desktop::Window window;
    Fixture()
        : paths{directory.path()},
          window(reportingPaths(), test::variableFontInputs(engineFontInputs(*assets, {}, {}, {})),
                 placeholderFontFamily(*assets, *copies)) {
        window.show();
        window.activateWindow();
    }
    storage::Paths reportingPaths() {
        paths.stateReport = directory.path() + "/state.json";
        return paths;
    }
    ~Fixture() {
        window.close();
        QElapsedTimer elapsed;
        elapsed.start();
        while (!window.idle() && elapsed.elapsed() < 30000)
            QTest::qWait(10);
    }
    QJsonObject state() {
        if (window.readerWindow())
            window.readerWindow()->checkpoint();
        return storage::readJson(paths.stateReport);
    }
    QJsonObject reportedState() const {
        return storage::readJson(paths.stateReport);
    }
    bool canvasClick(QWidget *target, QPoint local) {
        auto *reader = window.readerWindow();
        auto *canvas = window.findChild<QGraphicsView *>();
        if (!reader || !canvas || !target || !target->isVisible() || !target->isEnabled() ||
            !target->rect().contains(local))
            return false;
        const auto point = target->mapTo(reader, local);
        auto *hit = reader->childAt(point);
        if (hit != target && (!hit || !target->isAncestorOf(hit)))
            return false;
        const auto viewportPoint = canvas->mapFromScene(point);
        if (!canvas->viewport()->rect().contains(viewportPoint))
            return false;
        // Keep input on the canvas event path under headless Qt platforms.
        const auto global = canvas->viewport()->mapToGlobal(viewportPoint);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(viewportPoint), QPointF(global), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(viewportPoint), QPointF(global), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &release);
        return true;
    }
    QString book() const {
        return QStringLiteral(KOBALT_FIXTURES) + "/text.epub";
    }
};
}
class DesktopTest : public QObject {
    Q_OBJECT
private slots:
    void dictionariesStayInFillWindowWithoutHover() {
        Fixture f;
        f.window.resize(1000, 1000);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        auto *reader = f.window.readerWindow();
        reader->findChild<ReadingPage *>("rrPage")->tapped(reader->rect().center());
        reader->findChild<QPushButton *>("rrSettings")->click();
        auto *tab = reader->findChild<QPushButton *>("rrSettingsDictionaries");
        QVERIFY(tab->isVisible());
        tab->click();
        QWidget *line = nullptr;
        QTRY_VERIFY((line = reader->findChild<QWidget *>("rrDictionaryRow0")) && line->isVisible());
        ui::ListRow *row = nullptr;
        for (auto *button : line->findChildren<QPushButton *>())
            if (auto *candidate = dynamic_cast<ui::ListRow *>(button))
                row = candidate;
        QVERIFY(row);
        // Isolate the row's painting from the dictionary page's deferred rebuilds.
        QWidget paintHost;
        paintHost.resize(500, 100);
        row->setParent(&paintHost);
        row->setGeometry(paintHost.rect());
        paintHost.show();
        row->show();
        row->setAttribute(Qt::WA_UnderMouse, false);
        const QColor normal = row->grab().toImage().pixelColor(2, 2);
        row->setAttribute(Qt::WA_UnderMouse, true);
        QCOMPARE(row->grab().toImage().pixelColor(2, 2), normal);
        row->setDown(true);
        QVERIFY(row->grab().toImage().pixelColor(2, 2) != normal);
        row->setDown(false);
        auto *pages = reader->findChild<QStackedWidget *>("rrSettingsPages");
        auto *picker = f.window.findChild<QComboBox *>("desktopScreenPicker");
        for (int preset : {1, 2, 0}) {
            picker->setCurrentIndex(preset);
            QCOMPARE(tab->isHidden(), preset != 0);
            if (preset)
                QCOMPARE(pages->currentIndex(), int(reader::DisplaySettings));
        }
        QVERIFY(tab->isVisible());
        tab->click();
        QCOMPARE(pages->currentIndex(), int(reader::DictionariesSettings));
    }
    void pageNavigationFollowsPreviewButtons() {
        Fixture f;
        auto *picker = f.window.findChild<QComboBox *>("desktopScreenPicker");
        picker->setCurrentIndex(2);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        auto *reader = f.window.readerWindow();
        reader->findChild<QPushButton *>("rrSettings")->click();
        reader->findChild<QPushButton *>("rrSettingsControls")->click();
        auto *navigation = reader->findChild<QPushButton *>("rrPageNavigation");
        navigation->click();
        auto *buttons = reader->findChild<QCheckBox *>("rrNavigation_buttons_only");
        QVERIFY(buttons && buttons->isVisible());
        buttons->click();
        QTRY_VERIFY(!reader->findChild<QWidget *>("rrPageNavigationPopup"));
        QCOMPARE(storage::readJson(f.paths.preferences()).value("page_navigation").toString(), QString("buttons_only"));
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        page->tapped(page->rect().center());
        const int before = f.state().value("page").toInt();
        page->swiped(1);
        QTest::qWait(100);
        QCOMPARE(f.state().value("page").toInt(), before);
        for (int preset : {1, 0}) {
            picker->setCurrentIndex(preset);
            QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
            reader->findChild<QPushButton *>("rrSettings")->click();
            reader->findChild<QPushButton *>("rrSettingsControls")->click();
            navigation->click();
            QVERIFY(!reader->findChild<QCheckBox *>("rrNavigation_buttons_only"));
            auto *both = reader->findChild<QCheckBox *>("rrNavigation_taps_and_swipes");
            QVERIFY(both && both->isChecked());
            auto *popup = reader->findChild<QWidget *>("rrPageNavigationPopup");
            QTest::keyClick(popup, Qt::Key_Escape);
            QTRY_VERIFY(!reader->findChild<QWidget *>("rrPageNavigationPopup"));
            page->tapped(page->rect().center());
            const int previous = f.state().value("page").toInt();
            page->swiped(1);
            QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool() && f.state().value("page").toInt() > previous, 10000);
        }
        QCOMPARE(storage::readJson(f.paths.preferences()).value("page_navigation").toString(), QString("buttons_only"));
    }
    void bookmarkTargetCoversRightThird() {
        Fixture f;
        f.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        auto *reader = f.window.readerWindow();
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        auto *toolbar = reader->findChild<QWidget *>("rrControlsTop");
        if (toolbar->isVisible())
            page->tapped(page->rect().center());
        page->swiped(1);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        const auto target = page->bookmarkCorner();
        QCOMPARE(target.width(), qCeil(page->width() * .33));
        QCOMPARE(target.right(), page->width() - 1);
        for (int x : {target.left(), target.center().x(), target.right()}) {
            const bool before = page->bookmarked;
            page->tapped(QPoint(x, target.center().y()));
            QTRY_VERIFY(page->bookmarked != before);
            QVERIFY(!toolbar->isVisible());
        }
    }
    void devicePreviewMenuTapsStayNearCenter() {
        Fixture f;
        auto *picker = f.window.findChild<QComboBox *>("desktopScreenPicker");
        picker->setCurrentIndex(1);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        auto *reader = f.window.readerWindow();
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        auto *toolbar = reader->findChild<QWidget *>("rrControlsTop");
        for (int preset : {1, 2}) {
            picker->setCurrentIndex(preset);
            QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
            if (toolbar->isVisible())
                QVERIFY(f.canvasClick(page, page->rect().center()));
            QVERIFY(!toolbar->isVisible());
            const QPoint above(page->width() / 2, page->height() / 5);
            const QPoint below(page->width() / 2, page->height() * 4 / 5);
            for (QPoint point : {QPoint(page->width() / 2, 1), above, below}) {
                QVERIFY(f.canvasClick(page, point));
                QVERIFY(!toolbar->isVisible());
            }
            QVERIFY(f.canvasClick(page, page->rect().center()));
            QVERIFY(toolbar->isVisible());
            for (QPoint point : {above, below, QPoint(1, page->height() / 2)}) {
                QVERIFY(f.canvasClick(page, point));
                QVERIFY(toolbar->isVisible());
            }
            QVERIFY(f.canvasClick(page, page->rect().center()));
            QVERIFY(!toolbar->isVisible());
        }
    }
    void heldButtonsShowPressedState() {
        // A held reader button must look pressed in the window, through the canvas.
        Fixture f;
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        auto *reader = f.window.readerWindow();
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        page->tapped(page->rect().center());
        reader->findChild<QPushButton *>("rrNavText")->click();
        auto *font = reader->findChild<QPushButton *>("rrFont");
        QTRY_VERIFY(font && font->isVisible());
        auto *canvas = f.window.findChild<QGraphicsView *>();
        QVERIFY(canvas);
        const QPoint local(font->width() - 12, 6);
        const auto shown = [&] {
            QCoreApplication::processEvents();
            const auto viewportPoint = canvas->mapFromScene(font->mapTo(reader, local));
            const auto image = canvas->viewport()->grab().toImage();
            const qreal dpr = image.devicePixelRatio();
            return image.pixelColor(qRound(viewportPoint.x() * dpr), qRound(viewportPoint.y() * dpr));
        };
        const auto idle = shown();
        const auto viewportPoint = canvas->mapFromScene(font->mapTo(reader, font->rect().center()));
        const auto global = canvas->viewport()->mapToGlobal(viewportPoint);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(viewportPoint), QPointF(global), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &press);
        QVERIFY(font->isDown());
        const auto held = shown();
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(QPoint(-50, -50)), QPointF(global), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &release);
        // The theme's pressed gray, not a platform palette color: setting a
        // style sheet derives a new palette from the platform.
        QVERIFY2(idle == QColor(Qt::white), qPrintable(idle.name()));
        QVERIFY2(held == QColor("#dddddd"), qPrintable(held.name()));
    }
    void devicePreviewTapAbovePanelClosesIt() {
        // In a device preview only the middle of the page opens the menu. A
        // tall panel can cover that middle, so a tap on the visible page
        // above it must still close the panel.
        Fixture f;
        f.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(1);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        auto *reader = f.window.readerWindow();
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        auto *menu = reader->findChild<QWidget *>("rrMenu");
        auto *toolbar = reader->findChild<QWidget *>("rrControlsTop");
        page->tapped(page->rect().center());
        QVERIFY(toolbar->isVisible());
        reader->findChild<QPushButton *>("rrNavLayout")->click();
        QTRY_VERIFY(reader->findChild<QWidget *>("rrMargins")->isVisible());
        // The page, toolbar and panel are siblings; compare them in the reader's coordinates.
        const int pageTop = page->mapTo(reader, QPoint()).y();
        const int toolbarBottom = toolbar->mapTo(reader, QPoint()).y() + toolbar->height();
        const int menuTop = menu->mapTo(reader, QPoint()).y();
        QVERIFY(menuTop > toolbarBottom);
        const QPoint above(page->width() / 2, (toolbarBottom + menuTop) / 2 - pageTop);
        page->tapped(above);
        QTRY_VERIFY_WITH_TIMEOUT(!menu->isVisible() && !f.state()["busy"].toBool(), 30000);
    }
    void previewSettingsKeepSavedStatusPreference() {
        Fixture f;
        f.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(1);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        auto *reader = f.window.readerWindow();
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        QVERIFY(!page->slotContents[0].empty());
        page->tapped(page->rect().center());
        reader->findChild<QPushButton *>("rrSettings")->click();
        reader->findChild<QPushButton *>("rrSettingsUI")->click();
        QVERIFY(reader->findChild<QCheckBox *>("rrHideStatusBar")->isChecked());
    }
    void bookmarksFollowStatusAndPreviewChanges() {
        for (bool hidden : {true, false}) {
            Fixture f;
            QVERIFY(storage::writeJson(f.paths.preferences(), {{"native_hide_status_bar", hidden}}));
            QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
            f.window.openBook(f.book());
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
            auto *reader = f.window.readerWindow();
            auto *page = reader->findChild<ReadingPage *>("rrPage");
            page->tapped(page->rect().center());
            reader->findChild<QPushButton *>("rrBookmarks")->click();
            auto *list = reader->findChild<QWidget *>("rrBookmarkList");
            auto *title = reader->findChild<QLabel *>("rrBookmarkListTitle");
            auto *rule = reader->findChild<QWidget *>("rrNavigationUpperEdge");
            auto *picker = f.window.findChild<QComboBox *>("desktopScreenPicker");
            for (int preset : {0, 1, 2, 0}) {
                picker->setCurrentIndex(preset);
                const int top = preset || !hidden ? int(ui::metrics::chrome::statusHeight) : 0;
                QTRY_COMPARE(list->y(), top);
                QVERIFY(list->isVisible());
                QVERIFY(rule->isHidden());
                // The time shows whether or not the status bar is hidden.
                QVERIFY(!page->slotContents[0].empty());
                QVERIFY(!title->visibleRegion().isEmpty());
                if (preset)
                    QCOMPARE(reader->findChild<QLabel *>("rrMenuClock")->text(), QString("09:00"));
                // The panel's close control must receive input above the navigation rules.
                auto *close = reader->findChild<QPushButton *>("rrBookmarkListClose");
                const QPoint center = close->mapTo(reader, close->rect().center());
                auto *hit = reader->childAt(center);
                QVERIFY(hit == close || close->isAncestorOf(hit));
            }
            QCOMPARE(storage::readJson(f.paths.preferences())["native_hide_status_bar"].toBool(), hidden);
        }
    }
    void trackpadTouchDoesNotEmulateMouse() {
        class Surface final : public QWidget {
        public:
            int presses = 0, releases = 0, moves = 0, cancels = 0;
            bool event(QEvent *event) override {
                if (event->type() == QEvent::MouseButtonPress)
                    ++presses;
                if (event->type() == QEvent::MouseButtonRelease)
                    ++releases;
                if (event->type() == QEvent::MouseMove)
                    ++moves;
                if (event->type() == QEvent::TouchCancel)
                    ++cancels;
                return QWidget::event(event);
            }
        };
        android::WidgetCanvas canvas(nullptr);
        canvas.resize(400, 300);
        auto *surface = new Surface;
        surface->resize(1000, 1000);
        canvas.setContent(surface, true);
        canvas.setSceneRect(0, 0, 1000, 1000);
        canvas.setTransform(QTransform::fromScale(0.3, 0.3));
        canvas.show();
        auto *trackpad = QTest::createTouchDevice(QInputDevice::DeviceType::TouchPad);
        QTouchEvent begin(QEvent::TouchBegin, trackpad, Qt::NoModifier,
                          {QEventPoint(0, QEventPoint::Pressed, QPointF(), QPointF())});
        QApplication::sendEvent(canvas.viewport(), &begin);
        QCOMPARE(surface->presses, 0);
        QTouchEvent update(QEvent::TouchUpdate, trackpad, Qt::NoModifier,
                           {QEventPoint(0, QEventPoint::Updated, QPointF(50, 50), QPointF(50, 50))});
        QApplication::sendEvent(canvas.viewport(), &update);
        QTouchEvent cancel(QEvent::TouchCancel, trackpad);
        QApplication::sendEvent(canvas.viewport(), &cancel);
        QTouchEvent end(QEvent::TouchEnd, trackpad, Qt::NoModifier,
                        {QEventPoint(0, QEventPoint::Released, QPointF(50, 50), QPointF(50, 50))});
        QApplication::sendEvent(canvas.viewport(), &end);
        QCOMPARE(surface->moves, 0);
        QCOMPARE(surface->cancels, 0);
        QCOMPARE(surface->releases, 0);
        // A touchscreen still needs the canvas adapter on Android.
        auto *screen = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
        QTouchEvent screenBegin(QEvent::TouchBegin, screen, Qt::NoModifier,
                                {QEventPoint(0, QEventPoint::Pressed, QPointF(), QPointF())});
        QApplication::sendEvent(canvas.viewport(), &screenBegin);
        QCOMPARE(surface->presses, 1);
        QTouchEvent screenEnd(QEvent::TouchEnd, screen, Qt::NoModifier,
                              {QEventPoint(0, QEventPoint::Released, QPointF(), QPointF())});
        QApplication::sendEvent(canvas.viewport(), &screenEnd);
        QCOMPARE(surface->releases, 1);
    }
    void pageTapsAfterClosingText_data() {
        QTest::addColumn<int>("preset");
        QTest::newRow("six-inch") << 1;
        QTest::newRow("seven-inch") << 2;
    }
    void pageTapsAfterClosingText() {
        QFETCH(int, preset);
        Fixture f;
        f.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        auto *reader = f.window.readerWindow();
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        auto click = [&](QWidget *target, QPoint position) {
            auto *canvas = f.window.findChild<QGraphicsView *>();
            const auto viewportPoint = canvas->mapFromScene(target->mapTo(reader, position));
            QTest::mouseClick(f.window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                              canvas->viewport()->mapTo(&f.window, viewportPoint));
        };
        click(page, page->rect().center());
        auto *text = reader->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QTRY_VERIFY(text->isVisible());
        click(text, text->rect().center());
        QTest::qWait(200);
        click(page, QPoint(page->width() / 2, page->height() / 3));
        QTRY_VERIFY(!text->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        const auto before = f.state().value("page").toInt();
        click(page, QPoint(page->width() * 5 / 6, page->height() / 2));
        QTRY_VERIFY_WITH_TIMEOUT(f.state().value("page").toInt() > before, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        click(page, QPoint(page->width() / 6, page->height() / 2));
        QTRY_COMPARE_WITH_TIMEOUT(f.state().value("page").toInt(), before, 10000);
    }
    void toolbarLayout() {
        Fixture f;
        auto *picker = f.window.findChild<QComboBox *>("desktopScreenPicker");
        QVERIFY(picker);
        auto *logo = f.window.findChild<QWidget *>("desktopLogo");
        QVERIFY(logo);
        auto *version = f.window.findChild<QLabel *>("desktopVersion");
        QVERIFY(version);
        QCOMPARE(version->text(), QStringLiteral(KOBALT_VERSION));
        QVERIFY(QFile::exists(":/desktop/kobalt-android.svg"));
        for (const int width : {420, 640, 920}) {
            f.window.resize(width, 800);
            QTest::qWait(30);
            const auto center = picker->mapTo(&f.window, picker->rect().center()).x();
            QVERIFY(qAbs(center - f.window.rect().center().x()) <= 2);
            QVERIFY(logo->mapTo(&f.window, QPoint()).x() < picker->mapTo(&f.window, QPoint()).x());
            QVERIFY(version->mapTo(&f.window, QPoint()).x() > center);
        }
    }
    void resizedDeviceControls_data() {
        QTest::addColumn<int>("preset");
        QTest::newRow("six-inch") << 1;
        QTest::newRow("seven-inch") << 2;
    }
    void resizedDeviceControls() {
        QFETCH(int, preset);
        Fixture f;
        f.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        for (const auto size : {QSize(920, 600), QSize(420, 500), QSize(640, 800)}) {
            f.window.resize(size);
            QTest::qWait(200);
            auto *reader = f.window.readerWindow();
            auto *page = reader->findChild<ReadingPage *>("rrPage");
            QVERIFY(page);
            QVERIFY(f.canvasClick(page, page->rect().center()));
            auto *settings = reader->findChild<QPushButton *>("rrSettings");
            QVERIFY(settings);
            QTRY_VERIFY(settings->isVisible());
            QVERIFY(f.canvasClick(settings, settings->rect().center()));
            auto *tab = reader->findChild<QPushButton *>("rrSettingsUI");
            QVERIFY(tab);
            QTRY_VERIFY(tab->isVisible());
            QVERIFY(f.canvasClick(tab, tab->rect().center()));
            auto *toggle = reader->findChild<QCheckBox *>("rrHideStatusBar");
            QVERIFY(toggle);
            QTRY_VERIFY(toggle->isVisible());
            const bool before = toggle->isChecked();
            QVERIFY(f.canvasClick(toggle, toggle->rect().center()));
            QTRY_COMPARE(toggle->isChecked(), !before);
            QVERIFY(f.canvasClick(settings, settings->rect().center()));
            QTRY_VERIFY(!reader->findChild<QWidget *>("rrGeneralSettings")->isVisible());
            if (settings->isVisible())
                QVERIFY(f.canvasClick(page, page->rect().center()));
        }
    }
    void previewRendersInNativePixels() {
        class Surface final : public QWidget {
        public:
            qreal paintedScale = 0;
            QColor ink = Qt::black;
            void paintEvent(QPaintEvent *) override {
                QPainter painter(this);
                paintedScale = painter.deviceTransform().m11();
                painter.fillRect(rect(), Qt::white);
                painter.fillRect(QRect(100, 100, 100, 100), ink);
            }
        };
        android::WidgetCanvas canvas(nullptr);
        auto *surface = new Surface;
        surface->resize(1000, 1000);
        canvas.setContent(surface, true);
        canvas.setSceneRect(0, 0, 1000, 1000);
        canvas.resize(500, 500);
        canvas.setTransform(QTransform::fromScale(0.5, 0.5));
        canvas.show();
        QCoreApplication::processEvents();
        auto sample = [&] {
            const auto point = canvas.mapFromScene(QPointF(150, 150)) * canvas.devicePixelRatioF();
            return canvas.viewport()->grab().toImage().pixelColor(point);
        };
        QCOMPARE(sample(), QColor(Qt::black));
        QCOMPARE(surface->paintedScale, 1.0);
        surface->ink = Qt::red;
        surface->update(QRect(100, 100, 100, 100));
        QTRY_COMPARE(sample(), QColor(Qt::red));
        canvas.resize(250, 250);
        canvas.setTransform(QTransform::fromScale(0.25, 0.25));
        QCoreApplication::processEvents();
        QCOMPARE(sample(), QColor(Qt::red));
        QCOMPARE(surface->paintedScale, 1.0);
    }
    void deviceScreensKeepResolutionAndPosition() {
        Fixture f;
        auto *picker = f.window.findChild<QComboBox *>("desktopScreenPicker");
        QVERIFY(picker);
        picker->setCurrentIndex(1);
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        QCOMPARE(f.window.readerWindow()->size(), QSize(1072, 1448));
        QCOMPARE(readingPpi.load(), 300);
        QCOMPARE(ui::density(), 300);
        const auto firstPage = f.state().value("page").toInt();
        picker->setFocus();
        QTest::keyClick(&f.window, Qt::Key_Right);
        QCOMPARE(f.state().value("page").toInt(), firstPage);
        auto *page = f.window.readerWindow()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        QVERIFY(f.canvasClick(page, QPoint(page->width() * 5 / 6, page->height() / 2)));
        QTRY_VERIFY_WITH_TIMEOUT(f.state().value("page").toInt() > firstPage, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        const auto position = f.state().value("position").toString();
        const auto pages = f.state().value("pages").toInt();
        f.window.resize(920, 600);
        QTest::qWait(200);
        QCOMPARE(f.window.readerWindow()->size(), QSize(1072, 1448));
        QCOMPARE(f.state().value("pages").toInt(), pages);
        auto *canvas = f.window.findChild<QGraphicsView *>();
        QVERIFY(canvas);
        const auto preview = canvas->mapFromScene(QRectF(0, 0, 1072, 1448)).boundingRect();
        QVERIFY(preview.width() <= canvas->viewport()->width() + 1);
        QVERIFY(preview.height() <= canvas->viewport()->height() + 1);
        picker->setCurrentIndex(2);
        QTRY_COMPARE(f.window.readerWindow()->size(), QSize(1264, 1680));
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        QCOMPARE(f.state().value("position").toString(), position);
        auto *previous = f.window.findChild<QPushButton *>("desktopDevicePrevious");
        QVERIFY(previous);
        auto *next = f.window.findChild<QPushButton *>("desktopDeviceNext");
        QVERIFY(next);
        QVERIFY(previous->isVisible() && next->isVisible());
        QVERIFY(previous->isEnabled() && next->isEnabled());
        const int beforeButtons = f.state().value("page").toInt();
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(f.state().value("page").toInt() > beforeButtons, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        QTest::mouseClick(previous, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(f.state().value("page").toInt(), beforeButtons, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        const auto afterButtons = f.state().value("position").toString();
        picker->setCurrentIndex(0);
        QVERIFY(!previous->isVisible() && !next->isVisible());
        QCOMPARE(opened.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        QCOMPARE(f.window.readerWindow()->size(), canvas->size() * 2);
        QCOMPARE(canvas->transform().m11(), 0.5);
        QCOMPARE(readingPpi.load(), 300);
        QCOMPARE(f.state().value("position").toString(), afterButtons);
    }
    void canvasBookLifecycle_data() {
        QTest::addColumn<QString>("filename");
        int position = 0;
        const auto row = [&](const char *name, const QString &filename) {
            if (test::shardContains(position++, "KOBALT_TEST_DATA_SHARD")) {
                QTest::newRow(name) << filename;
            }
        };
        row("normal", "text.epub");
        row("long-public-domain", "public-domain-ru.epub");
        row("linked", "links.epub");
        row("arabic", "multilingual-ar.epub");
        row("japanese", "public-domain-ja.epub");
    }
    void canvasBookLifecycle() {
        QFETCH(QString, filename);
        Fixture f;
        // Hardware-mode runtime reports ordinary saves only with this opt-in.
        QFile liveState(f.directory.path() + "/live-state");
        QVERIFY(liveState.open(QIODevice::WriteOnly));
        liveState.close();
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        QSignalSpy failed(&f.window, &desktop::Window::bookFailed);
        const auto path = QStringLiteral(KOBALT_FIXTURES) + "/" + filename;
        f.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.reportedState().value("busy").toBool(), 30000);
        QCOMPARE(failed.count(), 0);
        const auto first = f.reportedState();
        QVERIFY(first.value("reading_image_height").toInt() > 0);
        QVERIFY(first.value("pages").toInt() > 1);
        QVERIFY(!first.value("position").toString().isEmpty());
        auto *reader = f.window.readerWindow();
        QVERIFY(reader);
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        const int forwardX = first.value("page_progression_rtl").toBool() ? page->width() / 6 : page->width() * 5 / 6;
        QVERIFY(f.canvasClick(page, QPoint(forwardX, page->height() / 2)));
        QTRY_VERIFY_WITH_TIMEOUT(f.reportedState().value("page").toInt() > first.value("page").toInt(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.reportedState().value("busy").toBool(), 30000);
        const auto turned = f.reportedState();
        const auto position = turned.value("position").toString();
        QVERIFY(!position.isEmpty());
        QVERIFY(position != first.value("position").toString());
        // Observe production saves without forcing a checkpoint from the test.
        QVERIFY(f.canvasClick(page, page->rect().center()));
        auto *back = reader->findChild<QPushButton *>("rrReturn");
        QVERIFY(back);
        QTRY_VERIFY(back->isVisible() && back->isEnabled());
        QVERIFY(f.canvasClick(back, back->rect().center()));
        QTRY_VERIFY_WITH_TIMEOUT(f.window.idle(), 30000);
        auto *title = f.window.findChild<QLabel *>("androidStartTitle");
        QVERIFY(title);
        QTRY_VERIFY(title->isVisible());
        QVERIFY(f.window.isVisible());
        f.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.reportedState().value("busy").toBool(), 30000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(f.reportedState().value("position").toString(), position);
        QCOMPARE(f.reportedState().value("page").toInt(), turned.value("page").toInt());
        QVERIFY(!f.window.grab().isNull());
    }
    void displayChangesRestoreCanvasScale() {
        Fixture f;
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        auto *canvas = f.window.findChild<QGraphicsView *>();
        QVERIFY(canvas);
        auto *handle = f.window.windowHandle();
        QVERIFY(handle);
        auto *screen = handle->screen();
        QVERIFY(screen);
        auto *picker = f.window.findChild<QComboBox *>("desktopScreenPicker");
        QVERIFY(picker);
        for (const int preset : {0, 1, 2}) {
            picker->setCurrentIndex(preset);
            QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
            QTest::qWait(150);
            const auto viewport = f.window.readerWindow()->size();
            const auto transform = canvas->transform();
            if (preset == 0) {
                QCOMPARE(viewport, canvas->size() * 2);
                QCOMPARE(transform.m11(), 0.5);
            }
            for (const char *signal : {"logicalDotsPerInchChanged", "physicalDotsPerInchChanged"}) {
                canvas->setTransform(QTransform::fromScale(.125, .125));
                QVERIFY(QMetaObject::invokeMethod(screen, signal, Qt::DirectConnection,
                                                  Q_ARG(qreal, screen->logicalDotsPerInch())));
                QTRY_COMPARE(canvas->transform(), transform);
                QCOMPARE(f.window.readerWindow()->size(), viewport);
            }
            canvas->setTransform(QTransform::fromScale(.125, .125));
            QVERIFY(QMetaObject::invokeMethod(handle, "screenChanged", Qt::DirectConnection, Q_ARG(QScreen *, screen)));
            QTRY_COMPARE(canvas->transform(), transform);
            QCOMPARE(f.window.readerWindow()->size(), viewport);
        }
    }
    void resizeAndNavigate() {
        Fixture f;
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        const int firstPage = f.state().value("page").toInt();
        QTest::keyClick(&f.window, Qt::Key_Right);
        QTRY_VERIFY_WITH_TIMEOUT(f.state().value("page").toInt() > firstPage, 10000);
        QTest::keyClick(&f.window, Qt::Key_Left);
        QTRY_COMPARE_WITH_TIMEOUT(f.state().value("page").toInt(), firstPage, 10000);
        const int before = f.state().value("viewport_width").toInt();
        f.window.resize(920, 600);
        const int expected = f.window.centralWidget()->width() * 2;
        QVERIFY(expected != before);
        QTRY_COMPARE_WITH_TIMEOUT(f.state().value("viewport_width").toInt(), expected, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        const auto state = f.state();
        QCOMPARE(state.value("reading_image_height").toInt(), state.value("viewport_height").toInt() -
                                                                  state.value("reading_top").toInt() -
                                                                  state.value("reading_bottom").toInt());
        QVERIFY(!f.window.grab().isNull());
        f.window.close();
        QTRY_VERIFY_WITH_TIMEOUT(f.window.idle(), 30000);
    }
    void fontHoverKeepsHeaderBorder() {
        Fixture f;
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        auto *reader = f.window.readerWindow();
        reader->findChild<ReadingPage *>("rrPage")->tapped(reader->rect().center());
        reader->findChild<QPushButton *>("rrNavText")->click();
        auto *font = reader->findChild<QPushButton *>("rrFont");
        QTRY_VERIFY(font->isVisible() && font->isEnabled());
        font->click();
        auto *close = reader->findChild<QPushButton *>("rrFontClose");
        QVERIFY(close && close->isVisible());
        auto *canvas = f.window.findChild<QGraphicsView *>();
        QVERIFY(canvas);
        QCoreApplication::processEvents();
        const auto before = canvas->viewport()->grab().toImage();
        const auto point = canvas->mapFromScene(close->mapTo(reader, close->rect().center()));
        const auto outside = canvas->mapFromScene(close->parentWidget()->mapTo(reader, QPoint(20, 20)));
        // Send through the view so the proxy invalidates its device cache.
        for (int pass = 0; pass < 3; ++pass)
            for (const bool hovering : {true, false}) {
                const auto position = hovering ? point : outside;
                QMouseEvent move(QEvent::MouseMove, QPointF(position),
                                 QPointF(canvas->viewport()->mapToGlobal(position)), Qt::NoButton, Qt::NoButton,
                                 Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &move);
                QCoreApplication::processEvents();
                QCOMPARE(close->underMouse(), hovering);
                QCOMPARE(canvas->viewport()->grab().toImage(), before);
            }
    }
    void variableFontWeightPreviewPersists() {
        const auto face = test::variableFont(test::variableFontInputs(engineFontInputs(*assets, {}, {}, {})));
        if (!face.weightAxis.available())
            QSKIP("Set KOBALT_TEST_VARIABLE_FONTS to exercise a variable font");
        const auto axis = face.weightAxis;
        const int initialWeight = qBound(axis.minimum, defaultVariableFontWeight, axis.maximum);
        Fixture f;
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        auto *reader = f.window.readerWindow();
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        QVERIFY(f.canvasClick(page, page->rect().center()));
        auto *text = reader->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(f.canvasClick(text, text->rect().center()));
        QTRY_VERIFY_WITH_TIMEOUT(f.state()["preview_ready"].toBool() && !f.state()["preview_busy"].toBool(), 30000);
        auto *font = reader->findChild<QPushButton *>("rrFont");
        QVERIFY(font);
        QVERIFY(f.canvasClick(font, font->rect().center()));
        QPushButton *choice = nullptr;
        for (int i = 0; i < 12 && !choice; ++i) {
            for (auto *label : reader->findChildren<QLabel *>())
                if (label->isVisible() && label->objectName().endsWith("Title") &&
                    label->text() == fontDisplayName(face.family))
                    choice = qobject_cast<QPushButton *>(label->parentWidget());
            if (choice)
                break;
            auto *next = reader->findChild<QPushButton *>("rrFontPagerNext");
            QVERIFY(next);
            QVERIFY(f.canvasClick(next, next->rect().center()));
        }
        QVERIFY(choice);
        QVERIFY(f.canvasClick(choice, choice->rect().center()));
        QTRY_COMPARE_WITH_TIMEOUT(f.state()["preview_settings"].toObject()["font_face"].toString(), face.family, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["preview_busy"].toBool(), 30000);
        auto *close = reader->findChild<QPushButton *>("rrFontClose");
        QVERIFY(close);
        QVERIFY(f.canvasClick(close, close->rect().center()));
        auto *weight = reader->findChild<QSlider *>("rrVariableFontWeight");
        QVERIFY(weight);
        QTRY_VERIFY(weight->isVisible() && weight->isEnabled());
        auto *optical = reader->findChild<QSlider *>("rrOpticalSize");
        QVERIFY(optical);
        QCOMPARE(optical->parentWidget()->isVisible(), face.opticalSizeAxis.available());
        auto *thickness = reader->findChild<QSlider *>("rrFontWeight");
        QVERIFY(thickness);
        QVERIFY(thickness->isVisible() && thickness->isEnabled());
        QCOMPARE(thickness->minimum(), 0);
        QCOMPARE(thickness->maximum(), 12);
        QVERIFY(f.canvasClick(thickness, QPoint(thickness->width() / 2, thickness->height() / 2)));
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["preview_busy"].toBool(), 30000);
        const int selectedThickness = f.state()["preview_settings"].toObject()["weight_steps"].toInt();
        QVERIFY(selectedThickness > 0);
        QCOMPARE(f.state()["preview_settings"].toObject()["font_weight"].toInt(), initialWeight);
        QCOMPARE(weight->minimum(), axis.minimum);
        QCOMPARE(weight->maximum(), axis.maximum);
        QCOMPARE(weight->singleStep(), 10);
        QCOMPARE(weight->value(), initialWeight);
        QVERIFY(f.canvasClick(weight,
                              QPoint(weight->width() * (initialWeight < (axis.minimum + axis.maximum) / 2 ? 3 : 1) / 4,
                                     weight->height() / 2)));
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["preview_busy"].toBool(), 30000);
        const auto draft = f.state()["preview_settings"].toObject();
        const int selected = draft["font_weight"].toInt();
        QVERIFY(selected != initialWeight && selected >= axis.minimum && selected <= axis.maximum);
        QVERIFY((selected - axis.minimum) % 10 == 0 || selected == axis.maximum);
        QCOMPARE(draft["weight_steps"].toInt(), selectedThickness);
        QCOMPARE(reader->findChild<QLabel *>("rrVariableFontWeightValue")->text(), QString::number(selected));
        QVERIFY(f.canvasClick(text, text->rect().center()));
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool() && !f.state()["preview_ready"].toBool(), 30000);
        QCOMPARE(f.state()["font_weight"].toInt(), selected);
        QCOMPARE(f.state()["weight_steps"].toInt(), selectedThickness);
        auto *back = reader->findChild<QPushButton *>("rrReturn");
        QVERIFY(back);
        if (!back->isVisible())
            QVERIFY(f.canvasClick(page, page->rect().center()));
        QTRY_VERIFY(back->isVisible() && back->isEnabled());
        QVERIFY(f.canvasClick(back, back->rect().center()));
        QTRY_VERIFY_WITH_TIMEOUT(f.window.idle(), 30000);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state()["busy"].toBool(), 30000);
        QCOMPARE(f.state()["font_face"].toString(), face.family);
        QCOMPARE(f.state()["font_weight"].toInt(), selected);
        QCOMPARE(f.state()["weight_steps"].toInt(), selectedThickness);
    }
    void nativeUiPreferencesPersist() {
        Fixture f;
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        auto *reader = f.window.readerWindow();
        auto *page = reader->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        QVERIFY(!page->slotContents[0].empty());
        QVERIFY(page->showHeader());
        page->tapped(page->rect().center());
        auto *toolbar = reader->findChild<QWidget *>("rrControlsTop");
        QVERIFY(toolbar);
        const int compactHeight = toolbar->height();
        QVERIFY(reader->findChild<QLabel *>("rrMenuClock")->isHidden());
        auto *settings = reader->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        settings->click();
        auto *nativeTab = reader->findChild<QPushButton *>("rrSettingsUI");
        QVERIFY(nativeTab);
        nativeTab->click();
        auto *hideStatus = reader->findChild<QCheckBox *>("rrHideStatusBar");
        auto *hideDevice = reader->findChild<QCheckBox *>("rrHideDeviceOptions");
        QVERIFY(hideStatus && hideDevice);
        QVERIFY(hideStatus->isChecked());
        QVERIFY(hideDevice->isChecked());
        QVERIFY(hideStatus->isVisible());
        auto *animation = reader->findChild<QCheckBox *>("rrPageAnimations");
        auto *refresh = reader->findChild<QPushButton *>("rrScreenRefresh");
        QVERIFY(animation && refresh);
        QVERIFY(animation->isHidden());
        QVERIFY(refresh->isHidden());
        hideDevice->click();
        hideStatus->click();
        reader->findChild<QPushButton *>("rrSettingsDisplay")->click();
        QVERIFY(animation->isVisible());
        QVERIFY(refresh->isVisible());
        QVERIFY(!animation->isEnabled());
        page->tapped(page->rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        QTRY_VERIFY(!reader->findChild<QWidget *>("rrGeneralSettings")->isVisible());
        QVERIFY(!page->slotContents[0].empty());
        page->tapped(page->rect().center());
        QTRY_VERIFY(toolbar->height() > compactHeight);
        QVERIFY(reader->findChild<QLabel *>("rrMenuClock")->isVisible());
        reader->findChild<QPushButton *>("rrNavChapters")->click();
        auto *chapters = reader->findChild<QWidget *>("rrChoices");
        QVERIFY(chapters);
        QTRY_VERIFY(chapters->isVisible());
        QCOMPARE(chapters->y(), int(ui::metrics::chrome::statusHeight));
        const auto prefs = storage::readJson(f.paths.preferences());
        QVERIFY(prefs.contains("native_hide_status_bar"));
        QVERIFY(!prefs.value("native_hide_status_bar").toBool());
        QVERIFY(!prefs.value("native_hide_device_options").toBool());
        reader->findChild<QPushButton *>("rrReturn")->click();
        QTRY_VERIFY_WITH_TIMEOUT(f.window.idle(), 30000);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        auto *reopened = f.window.readerWindow()->findChild<ReadingPage *>("rrPage");
        QVERIFY(reopened);
        QVERIFY(!reopened->slotContents[0].empty());
    }
    void hiddenDeviceOptionsKeepCompactRows() {
        Fixture f;
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        auto *reader = f.window.readerWindow();
        reader->findChild<ReadingPage *>("rrPage")->tapped(reader->rect().center());
        reader->findChild<QPushButton *>("rrSettings")->click();
        for (bool hidden : {true, false, true}) {
            reader->findChild<QPushButton *>("rrSettingsUI")->click();
            auto *toggle = reader->findChild<QCheckBox *>("rrHideDeviceOptions");
            if (toggle->isChecked() != hidden)
                toggle->click();
            for (const auto &tab : {"Advanced", "Display"}) {
                reader->findChild<QPushButton *>(QString("rrSettings") + tab)->click();
                const QStringList names = QString(tab) == "Advanced"
                                              ? QStringList{"rrInheritTextLayout", "rrInheritHeaderFooter"}
                                              : QStringList{"rrDarkMode", "rrBionicReading"};
                for (const auto &name : names) {
                    auto *option = reader->findChild<QCheckBox *>(name);
                    QVERIFY(option);
                    QTRY_COMPARE(option->height(), option->heightForWidth(option->width()));
                }
                if (QString(tab) == "Advanced") {
                    auto *lastDefault = reader->findChild<QCheckBox *>("rrInheritHeaderFooter");
                    auto *content = reader->findChild<QWidget *>("rrAdvancedContentSection");
                    QVERIFY(lastDefault && content);
                    auto *reset = reader->findChild<QPushButton *>("rrRestoreDefaults");
                    QTRY_COMPARE(reset->y(), lastDefault->geometry().bottom() + 1);
                    QTRY_COMPARE(content->y(), reset->geometry().bottom() + 1);
                }
            }
        }
    }
    void settingsRowsDrawBottomBorders() {
        Fixture f;
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.state().value("busy").toBool(), 30000);
        auto *reader = f.window.readerWindow();
        reader->findChild<ReadingPage *>("rrPage")->tapped(reader->rect().center());
        reader->findChild<QPushButton *>("rrSettings")->click();
        for (const auto &tab : {"Display", "Advanced", "UI", "Stats"}) {
            reader->findChild<QPushButton *>(QString("rrSettings") + tab)->click();
            auto *pages = reader->findChild<QStackedWidget *>("rrSettingsPages");
            for (auto *row : pages->currentWidget()->findChildren<QWidget *>()) {
                auto *action = dynamic_cast<ui::CaptionButton *>(row);
                if (!row->isVisible() ||
                    (!qobject_cast<QCheckBox *>(row) &&
                     !(action && action->presentation() == ui::CaptionButton::Presentation::ListAction)))
                    continue;
                const auto original = row->palette();
                for (bool dark : {false, true}) {
                    auto palette = original;
                    palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
                    palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
                    row->setPalette(palette);
                    const auto grab = row->grab();
                    const auto image = grab.toImage();
                    const int pixelRatio = qRound(grab.devicePixelRatioF());
                    const int x = image.width() / 2;
                    const int borderPixels = int(ui::metrics::control::border) * pixelRatio;
                    for (int offset = 1; offset <= borderPixels; ++offset) {
                        QCOMPARE(image.pixelColor(x, image.height() - offset), ui::dividerColor(row));
                    }
                    QCOMPARE(image.pixelColor(x, image.height() - borderPixels - 1), palette.color(QPalette::Window));
                }
                row->setPalette(original);
            }
        }
    }
    void failedBookAndCloseReturnToStart() {
        Fixture f;
        QSignalSpy failed(&f.window, &desktop::Window::bookFailed);
        f.window.openBook(f.directory.path() + "/missing.epub");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(f.window.idle(), 10000);
        auto *title = f.window.findChild<QLabel *>("androidStartTitle");
        QVERIFY(title);
        QVERIFY(title->isVisible());
        const auto errorTitle = title->text();
        QSignalSpy opened(&f.window, &desktop::Window::bookOpened);
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        auto *back = f.window.readerWindow()->findChild<QPushButton *>("rrReturn");
        QVERIFY(back);
        back->click();
        QTRY_VERIFY_WITH_TIMEOUT(f.window.idle(), 30000);
        QTRY_VERIFY(title->isVisible());
        QVERIFY(title->text() != errorTitle);
        QVERIFY(f.window.isVisible());
        f.window.openBook(f.book());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
    }
    void closeDuringOpen() {
        Fixture f;
        f.window.openBook(f.book());
        f.window.close();
        QTRY_VERIFY_WITH_TIMEOUT(f.window.idle(), 30000);
        QTRY_VERIFY(!f.window.isVisible());
    }
};
int main(int argc, char **argv) {
    FontCopies fontCopies;
    copies = &fontCopies;
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setQuitOnLastWindowClosed(false);
    AssetBundle bundle(QStringLiteral(KOBALT_TEST_ASSET_ROOT), QStringLiteral(KOBALT_VERSION));
    assets = &bundle;
    if (!bundle.available() || !ui::initializeInterfaceFonts(bundle))
        return 2;
    for (const auto &file : bundledFontFiles(bundle))
        if (QFontDatabase::addApplicationFont(file) < 0)
            return 2;
    readingPpi.store(150);
    ui::setDensity(150);
    // Worker shutdown is connected to aboutToQuit, as in the application.
    DesktopTest test;
    QTimer::singleShot(0, &app, [&] {
        app.exit(QTest::qExec(&test, test::shardArguments(test, argc, argv)));
    });
    return app.exec();
}
#include "window_test.moc"
