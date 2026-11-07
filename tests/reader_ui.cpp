#include "app/reader/panels/choice_screen.h"
#include "ui/input/hold_timer.h"
#include "render/page_turn_paint.h"
#include "reading/page_navigation.h"
#include "ui/input/tap_filter.h"
#include "support/paint_regions.h"
#include "ui/controls/keyboard.h"
#include <QLineEdit>
#include <QDynamicPropertyChangeEvent>
#include "assets/bundled_fonts.h"
#include "app/library/library_view.h"
#include "app/about/about_page.h"
#include "app/reader/settings/acknowledgments.h"
#include "ui/controls/buttons.h"
#include "ui/controls/highlight_palette.h"
#include "ui/controls/options.h"
#include "ui/controls/loading.h"
#include "ui/controls/sliders.h"
#include "ui/controls/rows.h"
#include "ui/controls/panels.h"
#include "ui/controls/text.h"
#include "ui/controls/paging.h"
#include "ui/dialogs/modals.h"
#include "ui/dialogs/action_menu.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/scale.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "app/reader/views/opening.h"
#include "app/reader/views/reading_page.h"
#include "app/reader/views/navigation.h"
#include "platforms/kobo/display/navigation_refresh.h"
#include "app/reader/settings/header_footer_panel.h"
#include "app/reader/panels/bookmark_list.h"
#include "platforms/standalone/canvas.h"
#include "platforms/android/canvas_refresh.h"
#include "platforms/android/boox_refresh.h"
#include "platforms/standalone/start_screen.h"
#include "platforms/standalone/window_geometry.h"
#include "library/library_store.h"
#include "platforms/kobo/display/annotation_damage.h"
#include "platforms/kobo/display/panel_damage.h"
#include "support/host_presentation.h"
#include "support/reader_inputs.h"
#include "support/strings.h"
#include "engine/crengine/engine_document.h"
#include <QApplication>
#include <QtGui/private/qeventpoint_p.h>
#include <QFontDatabase>
#include <QLocale>
#include <QScopeGuard>
#include <QDir>
#include <QDialog>
#include <QTimer>
#include <QPaintEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QTemporaryDir>
#include <QStackedLayout>
#include <QTest>
#include <QtMath>
#include <utility>

using namespace kobalt;

namespace {
using test::PaintRegions;

class DeleteOnTouchCancel final : public QPushButton
{
public:
    using QPushButton::QPushButton;

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::TouchCancel) {
            delete this;
            return true;
        }
        return QPushButton::event(event);
    }
};

class Canvas final : public standalone::WidgetCanvas
{
public:
    Canvas(QWidget *surface, QSize size) : WidgetCanvas(nullptr)
    {
        surface->setFixedSize(size);
        setContent(surface);
        setSceneRect(QRectF(QPointF(), size));
    }

    void resizeEvent(QResizeEvent *event) override
    {
        WidgetCanvas::resizeEvent(event);
        fitInView(sceneRect(), Qt::KeepAspectRatio);
    }
};

bool clickAt(Canvas &canvas, QPoint point)
{
    QCoreApplication::processEvents();
    const auto local = canvas.mapFromScene(point);
    if (!canvas.viewport()->rect().contains(local))
        return false;
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, local);
    return true;
}

bool click(Canvas &canvas, QWidget *widget)
{
    QCoreApplication::processEvents();
    if (!widget || !widget->isVisible() || !widget->isEnabled())
        return false;
    const auto point = widget->mapTo(canvas.content, widget->rect().center());
    const auto *receiver = canvas.content->childAt(point);
    if (receiver != widget && (!receiver || !widget->isAncestorOf(receiver)))
        return false;
    return clickAt(canvas, point);
}

void pointerEvent(QWidget *widget, QEvent::Type type, ulong timestamp, QPoint point)
{
    QMouseEvent event(type, point, widget->mapToGlobal(point), Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    event.setTimestamp(timestamp);
    QApplication::sendEvent(widget, &event);
}

void variants()
{
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("ppi");
    for (bool dark : { false, true })
        for (int ppi : { 300, 212 }) {
            const auto name = QString("%1-%2").arg(dark ? "dark" : "light").arg(ppi);
            QTest::newRow(qPrintable(name)) << dark << ppi;
        }
}
} // namespace

class ReaderUiTests final : public QObject
{
    Q_OBJECT
private slots:

    // Checks Sweep frames in both directions: whole stationary bands, exact endpoints, sleep snapshots, cancellation and Off.
    void pageTurnFrames_data() { variants(); }

    void pageTurnFrames()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        const auto colors = ui::themeColors(dark);
        const auto sheet = [&](QSize size, const QString &title) {
            QImage image(size, QImage::Format_RGB32);
            image.fill(colors.paper);
            QPainter painter(&image);
            painter.setPen(colors.ink);
            painter.setFont(ui::brandFont(ui::px(42)));
            ui::drawInterfaceText(painter,
                                  image.rect().adjusted(ui::px(36), ui::px(40), -ui::px(36), 0),
                                  title, Qt::AlignLeft | Qt::AlignTop);
            painter.setFont(ui::interfaceFont(ui::px(26)));
            for (int y = ui::px(140); y < image.height() - ui::px(60); y += ui::px(50))
                ui::drawInterfaceText(painter,
                                      QRect(ui::px(36), y, image.width() - ui::px(72), ui::px(44)),
                                      "The next page waits beneath the turning paper.",
                                      Qt::AlignLeft | Qt::AlignTop);
            ui::drawInterfaceText(painter,
                                  QRect(ui::px(36), image.height() - ui::px(58),
                                        image.width() - ui::px(72), ui::px(44)),
                                  title, Qt::AlignRight | Qt::AlignVCenter);
            return image;
        };
        const auto capture = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
        for (const auto size :
             { QSize(ui::px(720), ui::px(1440)), QSize(ui::px(1440), ui::px(720)) }) {
            const auto oldPage = sheet(size, "Chapter one"), nextPage = sheet(size, "Chapter two");
            for (auto style : { pageTurn::Style::Sweep }) {
                const auto frame = [&](qreal progress, int direction) {
                    QImage image(size, QImage::Format_RGB32);
                    image.fill(Qt::magenta);
                    QPainter painter(&image);
                    render::paintPageTurn(painter, oldPage, nextPage, progress, direction, style);
                    painter.end();
                    return image;
                };
                for (int direction : { -1, 1 }) {
                    QCOMPARE(frame(0, direction), oldPage);
                    QCOMPARE(frame(1, direction), nextPage);
                    const auto middle = frame(.45, direction);
                    QVERIFY2(middle != oldPage && middle != nextPage,
                             qPrintable(pageTurn::value(style)));
                    QVERIFY(middle.pixelColor(size.width() / 2, size.height() / 2)
                            != QColor(Qt::magenta));
                    if (!capture.isEmpty())
                        QVERIFY(middle.save(capture
                                            + QString("/turn-%1-%2-%3-%4-%5.png")
                                                      .arg(ppi)
                                                      .arg(dark ? "dark" : "light")
                                                      .arg(size.width() > size.height()
                                                                   ? "landscape"
                                                                   : "portrait")
                                                      .arg(direction)
                                                      .arg(pageTurn::value(style))));
                }
            }
        }
        {
            QImage oldPage(101, 82, QImage::Format_RGB32),
                    nextPage(oldPage.size(), QImage::Format_RGB32);
            for (int y = 0; y < oldPage.height(); ++y)
                for (int x = 0; x < oldPage.width(); ++x) {
                    oldPage.setPixelColor(x, y, QColor(200, x, y));
                    nextPage.setPixelColor(x, y, QColor(5, x + 20, y + 50));
                }
            for (int direction : { -1, 1 }) {
                const auto frame = [&](qreal progress) {
                    QImage image(oldPage.size(), QImage::Format_RGB32);
                    QPainter painter(&image);
                    render::paintPageTurn(painter, oldPage, nextPage, progress, direction,
                                          pageTurn::Style::Sweep);
                    return image;
                };
                const auto banded = frame(.21);
                QCOMPARE(frame(.24), banded); // Both values reveal the same three of ten bands.
                for (int y = 0; y < oldPage.height(); ++y)
                    for (int x = 0; x < oldPage.width(); ++x)
                        QCOMPARE(banded.pixel(x, y),
                                 (direction > 0 ? x >= 70 : x < 30) ? nextPage.pixel(x, y)
                                                                    : oldPage.pixel(x, y));
            }
        }
        QCOMPARE(pageTurn::selected({}), pageTurn::Style::Off);
        QCOMPARE(pageTurn::parse("invalid"), pageTurn::Style::Off);
        QCOMPARE(pageTurn::parse("curl"), pageTurn::Style::Sweep);
        QCOMPARE(pageTurn::parse("fade"), pageTurn::Style::Sweep);
        QCOMPARE(pageTurn::parse("sweep"), pageTurn::Style::Sweep);
        QCOMPARE(pageTurn::parse("off"), pageTurn::Style::Off);
        QWidget host;
        host.setProperty("reader_dark", dark);
        host.setPalette(ui::readerPalette(host.palette(), dark));
        host.resize(ui::px(720), ui::px(1600));
        ReadingPage page(&host, [] { });
        page.setPalette(host.palette());
        page.resize(host.size());
        page.chrome = headerfooter::presetLayout(headerfooter::Preset::Phone);
        page.image =
                sheet(QSize(page.width(), page.height() - page.topHeight() - page.bottomHeight()),
                      "Chapter one");
        host.show();
        page.show();
        page.refreshContent();
        page.setPageAnimationStyle(pageTurn::Style::Sweep);
        const auto destination = sheet(page.image.size(), "Chapter two");
        page.image = destination;
        page.animationDirection = 1;
        page.refreshContent(true);
        auto *animation = page.findChild<QTimeLine *>("rrPageTurn");
        QVERIFY(animation);
        QCOMPARE(animation->duration(), 180);
        QCOMPARE(animation->easingCurve().type(), QEasingCurve::InOutSine);
        QVERIFY(animation->updateInterval() > 0 && animation->updateInterval() <= 17);
        QCOMPARE(animation->state(), QTimeLine::Running);
        QCOMPARE(host.property("reader_animation_direction").toInt(), 0);
        animation->setCurrentTime(animation->duration() / 2);
        QVERIFY(page.grab().toImage().convertToFormat(QImage::Format_RGB32) != page.snapshot());
        // Sleep snapshots must contain the destination, not a partial sweep.
        QCOMPARE(page.snapshot().copy(0, page.topHeight(), page.width(), destination.height()),
                 destination);
        page.cancelHold();
        QCOMPARE(animation->state(), QTimeLine::NotRunning);
        QCOMPARE(page.grab().toImage().convertToFormat(QImage::Format_RGB32), page.snapshot());
        page.image = sheet(destination.size(), "Chapter three");
        page.animationDirection = -1;
        page.refreshContent(true);
        QCOMPARE(animation->state(), QTimeLine::Running);
        page.hide();
        QCOMPARE(animation->state(), QTimeLine::NotRunning);
        page.show();
        page.setPageAnimationStyle(pageTurn::Style::Off);
        page.image = destination;
        page.animationDirection = 1;
        page.refreshContent(true);
        QCOMPARE(animation->state(), QTimeLine::NotRunning);
    }

    // Checks chapter-list swipes through the canvas: forward, backward and wrapped paging without selecting a row.
    void pagedListSwipes_data() { variants(); }

    void pagedListSwipes()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        auto *chapters = new ChapterContents(nullptr);
        chapters->setProperty("reader_dark", dark);
        Canvas canvas(chapters, QSize(ui::px(1072), ui::px(1448)));
        canvas.resize(536, 724);
        canvas.show();
        QVector<Choice> choices;
        for (int i = 0; i < 60; ++i)
            choices.append({ QString("Chapter %1").arg(i), QString::number(i),
                             QString("chapter%1").arg(i) });
        int selected = 0;
        chapters->selected = [&](const QString &) { ++selected; };
        chapters->showChapters("Test book", choices, {});
        QCoreApplication::processEvents();
        auto *counter = chapters->findChild<QLabel *>("rrContentsPage");
        auto *next = chapters->findChild<QPushButton *>("rrContentsNext");
        auto *previous = chapters->findChild<QPushButton *>("rrContentsPrevious");
        QVERIFY(counter && next && previous);
        const auto first = counter->text();
        auto *row = chapters->findChild<QPushButton *>("chapter0");
        QVERIFY(row && row->isVisible());
        auto swipe = [&](bool forward) {
            const QPoint center = row->mapTo(chapters, row->rect().center());
            const QPoint travel(ui::px(160), 0);
            const auto from = canvas.mapFromScene(center + (forward ? travel : -travel));
            const auto to = canvas.mapFromScene(center + (forward ? -travel : travel));
            QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, from);
            QTest::mouseMove(canvas.viewport(), to);
            QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, to);
            QCoreApplication::processEvents();
        };
        swipe(true);
        const auto second = counter->text();
        QVERIFY(second != first);
        QCOMPARE(selected, 0);
        swipe(false);
        QCOMPARE(counter->text(), first);
        QCOMPARE(selected, 0);
        QVERIFY(click(canvas, previous));
        const auto last = counter->text();
        QVERIFY(last != first && last != second);
        swipe(true);
        QCOMPARE(counter->text(), first);
        QVERIFY(click(canvas, next));
        QCOMPARE(counter->text(), second);
        swipe(false);
        QCOMPARE(counter->text(), first);
        QVERIFY(click(canvas, row));
        QCOMPARE(selected, 1);
    }

    // Checks that disclosure taps toggle groups and disclosure swipes only page the list.
    void pagedChapterGroups_data() { variants(); }

    void pagedChapterGroups()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        auto *chapters = new ChapterContents(nullptr);
        chapters->setProperty("reader_dark", dark);
        Canvas canvas(chapters, QSize(ui::px(1072), ui::px(1448)));
        canvas.resize(536, 724);
        canvas.show();
        QVector<Choice> choices;
        for (int i = 0; i < 60; ++i)
            choices.append({ QString::number(i),
                             QString::number(i),
                             QString("chapter%1").arg(i),
                             {},
                             i % 2,
                             i });
        int selected = 0;
        chapters->selected = [&](const QString &) { ++selected; };
        chapters->showChapters("Groups", choices, "chapter0", ChapterGroups::All);
        QCoreApplication::processEvents();
        auto *leaf = chapters->findChild<QPushButton *>("chapter1");
        QVERIFY(leaf);
        QCOMPARE(leaf->x(), 0);
        QCOMPARE(leaf->width(), chapters->width());
        const auto leafPoint =
                canvas.mapFromScene(leaf->mapTo(chapters, QPoint(ui::px(10), leaf->height() / 2)));
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, leafPoint);
        QVERIFY(leaf->isDown());
        const auto pressed = leaf->grab().toImage();
        QCOMPARE(pressed.pixelColor(2, 2), ui::themeColors(leaf).pressedPaper);
        QCOMPARE(pressed.pixelColor(leaf->width() - 3, 2), ui::themeColors(leaf).pressedPaper);
        QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, leafPoint);
        QCOMPARE(selected, 1);
        selected = 0;
        auto disclosure = [&] { return chapters->findChild<QPushButton *>("chapter0Toggle"); };
        QVERIFY(disclosure() && disclosure()->isChecked());
        QVERIFY(click(canvas, disclosure()));
        QVERIFY(!disclosure()->isChecked());
        QCOMPARE(chapters->currentPage(), 0);
        QCOMPARE(selected, 0);
        QVERIFY(click(canvas, disclosure()));
        QVERIFY(disclosure()->isChecked());
        const auto from = canvas.mapFromScene(disclosure()->geometry().center());
        const auto to = from + QPoint(160, 0);
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(canvas.viewport(), to);
        QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, to);
        QCoreApplication::processEvents();
        QVERIFY(chapters->currentPage() > 0);
        QCOMPARE(selected, 0);
        QVERIFY(click(canvas, chapters->findChild<QPushButton *>("rrContentsNext")));
        QCOMPARE(chapters->currentPage(), 0);
        QVERIFY(disclosure()->isChecked());
        QVERIFY(click(canvas, chapters->findChild<QPushButton *>("chapter0")));
        QCOMPARE(selected, 1);
    }

    // Checks that list swipes respect nested pagers, disabled arrows, small or vertical moves, held rows and gesture controls.
    void pagedSwipesRespectControls()
    {
        ui::setDensity(300);
        QWidget host;
        host.resize(1000, 1000);
        ui::PagerBar outer(&host, "outer");
        outer.setGeometry(0, 900, 1000, 100);
        outer.setPage(0, 3);
        QWidget nested(&host);
        nested.setGeometry(0, 0, 1000, 800);
        ui::PageControls inner(&nested, "inner");
        inner.setGeometry(0, 700, 1000, 100);
        inner.setPage(0, 2);
        ui::ListRow row(&nested);
        row.setGeometry(0, 0, 1000, 120);
        ui::SettingSlider slider(&nested, 0, 100, 1);
        slider.setGeometry(0, 150, 1000, 100);
        QLineEdit entry(&nested);
        entry.setGeometry(0, 300, 1000, 100);
        host.show();
        QCoreApplication::processEvents();
        QSignalSpy outerTurns(outer.next, &QPushButton::clicked);
        QSignalSpy innerTurns(inner.next, &QPushButton::clicked);
        QSignalSpy clicks(&row, &QPushButton::clicked);
        auto drag = [&](QWidget *widget, QPoint delta, ulong duration = 100) {
            const QPoint start(700, 50);
            pointerEvent(widget, QEvent::MouseButtonPress, 1000, start);
            pointerEvent(widget, QEvent::MouseMove, 1050, start + delta);
            pointerEvent(widget, QEvent::MouseButtonRelease, 1000 + duration, start + delta);
        };
        drag(&row, { -300, 0 });
        QCOMPARE(innerTurns.count(), 1);
        QCOMPARE(outerTurns.count(), 0);
        QCOMPARE(clicks.count(), 0);
        QVERIFY(!row.isDown());
        // Disabled arrows consume the swipe without activating a list entry.
        inner.next->setEnabled(false);
        drag(&row, { -300, 0 });
        QCOMPARE(innerTurns.count(), 1);
        QCOMPARE(clicks.count(), 0);
        inner.next->setEnabled(true);
        drag(&row, { -5, 0 });
        drag(&row, { -100, 200 });
        row.setProperty("reader_swipe_hold", true);
        drag(&row, { -300, 0 }, 700);
        row.setProperty("reader_swipe_hold", false);
        drag(&slider, { -300, 0 });
        QVERIFY(slider.value() > 0);
        drag(&entry, { -300, 0 });
        QCOMPARE(innerTurns.count(), 1);
        row.setProperty("reader_owns_swipes", true);
        drag(&row, { -300, 0 });
        QCOMPARE(innerTurns.count(), 1);
        row.setProperty("reader_owns_swipes", false);
        pointerEvent(&row, QEvent::MouseButtonPress, 1000, { 700, 50 });
        QEvent cancel(QEvent::TouchCancel);
        QApplication::sendEvent(&row, &cancel);
        pointerEvent(&row, QEvent::MouseButtonRelease, 1100, { 400, 50 });
        QCOMPARE(innerTurns.count(), 1);
        inner.hide();
        drag(&row, { -300, 0 });
        QCOMPARE(outerTurns.count(), 1);
    }

    // Checks font-cell swipes, default-font holds and delayed held drags, without selecting a font during a swipe.
    void pagedFontSwipesKeepHolds()
    {
        ui::setDensity(300);
        QWidget host;
        host.resize(1072, 1448);
        ChoiceScreen picker(&host);
        picker.setFontGeometry(QRect(0, 0, host.width(), picker.fontPanelHeight()));
        QVector<Choice> choices;
        for (int i = 0; i < 40; ++i)
            choices.append({ QString("Font %1").arg(i), "Libron", QString("font%1").arg(i) });
        int selections = 0, defaults = 0;
        picker.selected = [&](const QString &) { ++selections; };
        picker.defaultRequested = [&](const QString &, bool) { ++defaults; };
        host.show();
        picker.showChoices("Fonts", choices, {}, ChoiceScreen::Mode::Fonts);
        QCoreApplication::processEvents();
        auto *counter = picker.findChild<QLabel *>("rrFontPagerPage");
        auto *cell = picker.findChild<QPushButton *>("font0");
        auto *previous = picker.findChild<QPushButton *>("rrFontPagerPrevious");
        QVERIFY(counter && counter->isVisible() && cell && cell->isVisible() && previous);
        const auto first = counter->text();
        const auto center = cell->rect().center();
        const QPoint travel(80, 0);
        pointerEvent(cell, QEvent::MouseButtonPress, 1000, center + travel);
        pointerEvent(cell, QEvent::MouseMove, 1050, center - travel);
        pointerEvent(cell, QEvent::MouseButtonRelease, 1100, center - travel);
        QVERIFY(counter->text() != first);
        QCOMPARE(selections, 0);
        QCOMPARE(defaults, 0);
        previous->click();
        QCOMPARE(counter->text(), first);
        pointerEvent(cell, QEvent::MouseButtonPress, 2000, center);
        pointerEvent(cell, QEvent::MouseButtonRelease, 2700, center);
        QCOMPARE(defaults, 1);
        QCOMPARE(selections, 0);
        QCOMPARE(counter->text(), first);
        // A held drag is consumed even when the event queue delayed the hold.
        pointerEvent(cell, QEvent::MouseButtonPress, 3000, center + travel);
        pointerEvent(cell, QEvent::MouseButtonRelease, 3700, center - travel);
        QCOMPARE(defaults, 1);
        QCOMPARE(selections, 0);
        QCOMPARE(counter->text(), first);
    }

    void pageTurnTapFilter()
    {
        ui::TapFilter filter;
        QVERIFY(filter.accept(1000, 0));
        QVERIFY(!filter.accept(1130, 5000)); // A queued tap is still only 130 ms later.
        QVERIFY(!filter.accept(1199, 5100));
        QVERIFY(filter.accept(1200, 5101)); // Ignored taps do not extend the window.
        QVERIFY(filter.accept(2200, 5102)); // Widely spaced queued taps remain valid.
        QVERIFY(!filter.accept(2100, 5103)); // Reject stale input.
        ui::TapFilter wrap;
        QVERIFY(wrap.accept(0xffffffc0u, 0));
        QVERIFY(!wrap.accept(0x41u, 10));
        QVERIFY(wrap.accept(0x88u, 20)); // Exactly 200 ms across source-clock wrap.
        ui::TapFilter fallback;
        QVERIFY(fallback.accept(0, 0));
        QVERIFY(!fallback.accept(0, 199));
        QVERIFY(fallback.accept(0, 200));
        QVERIFY(fallback.accept(8000, 400));
        QVERIFY(!fallback.accept(0, 500)); // Mixed input uses delivery time.
    }

    void pageTurnFilterSeparatesInputClocks()
    {
        ui::TapFilter filter;
        using Clock = ui::InputClock;
        // Recorded Libra timestamps after suspension: touch includes time
        // asleep, while page buttons use a clock that excludes it.
        QVERIFY(filter.accept(4778052, 0, Clock::Pointer));
        QVERIFY(filter.accept(1233250, 930, Clock::Key));
        QVERIFY(!filter.accept(1233380, 5000, Clock::Key)); // Queued duplicate.
        QVERIFY(filter.accept(1236767, 6000, Clock::Key));
        QVERIFY(!filter.accept(4789322, 6100, Clock::Pointer)); // Mixed duplicate.
        QVERIFY(filter.accept(4789322, 6300, Clock::Pointer));
        QVERIFY(filter.accept(1245159, 7962, Clock::Key));
    }

    void holdSourceTime_data()
    {
        QTest::addColumn<quint32>("pressed");
        QTest::addColumn<quint32>("released");
        QTest::addColumn<bool>("due");
        QTest::newRow("short") << quint32(1000) << quint32(1085) << false;
        QTest::newRow("before-deadline") << quint32(1000) << quint32(1549) << false;
        QTest::newRow("deadline") << quint32(1000) << quint32(1550) << true;
        QTest::newRow("hold") << quint32(1000) << quint32(2000) << true;
        QTest::newRow("wrapped") << quint32(0xfffffff0u) << quint32(0x3d8u) << true;
        QTest::newRow("backwards") << quint32(1000) << quint32(900) << false;
    }

    void holdSourceTime()
    {
        QFETCH(quint32, pressed);
        QFETCH(quint32, released);
        QFETCH(bool, due);
        ui::HoldTimer timer;
        timer.setSingleShot(true);
        timer.setInterval(550);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(), QPointF(), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), QPointF(), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        press.setTimestamp(pressed);
        release.setTimestamp(released);
        timer.start(&press);
        QCOMPARE(timer.finish(&release), due);
        QVERIFY(!timer.finish(&release));
        timer.start(&press);
        timer.stop();
        QVERIFY(!timer.finish(&release));
        timer.start(&press);
        timer.stopTimeout();
        QCOMPARE(timer.finish(&release), due);
        timer.start(&press);
        timer.stopTimeout();
        timer.stop();
        QVERIFY(!timer.finish(&release));
    }

    // Checks that invalidating a hold target preserves duration, movement and cancellation rules for both input paths.
    void holdInvalidatedTarget_data()
    {
        QTest::addColumn<bool>("touch");
        QTest::addColumn<quint32>("pressed");
        QTest::addColumn<quint32>("released");
        QTest::addColumn<QPoint>("travel");
        QTest::addColumn<bool>("cancel");
        QTest::addColumn<int>("taps");
        QTest::addColumn<int>("swipes");
        for (bool touch : { false, true }) {
            const QByteArray prefix = touch ? "touch-" : "pointer-";
            const auto row = [&](const char *name, quint32 press, quint32 release, QPoint travel,
                                 bool cancel, int taps, int swipes) {
                QTest::newRow((prefix + name).constData())
                        << touch << press << release << travel << cancel << taps << swipes;
            };
            row("short", 1000, 1499, {}, false, 1, 0);
            row("threshold", 1000, 1500, {}, false, 0, 0);
            row("queued-long", 1000, 1709, {}, false, 0, 0);
            row("wrapped", 0xfffffff0u, 0x2b5u, {}, false, 0, 0);
            row("jitter", 1000, 1709, { 10, 10 }, false, 0, 0);
            row("moved", 1000, 1709, { 50, 0 }, false, 0, 0);
            row("swipe", 1000, 1709, { -180, 0 }, false, 0, 1);
            row("cancel", 1000, 1709, {}, true, 0, 0);
        }
    }

    // Checks that a stale long hold never navigates or selects old text and that the next hold still works.
    void holdInvalidatedTarget()
    {
        QFETCH(bool, touch);
        QFETCH(quint32, pressed);
        QFETCH(quint32, released);
        QFETCH(QPoint, travel);
        QFETCH(bool, cancel);
        QFETCH(int, taps);
        QFETCH(int, swipes);
        ui::setDensity(300);
        ReadingPage page(nullptr, [] { });
        page.resize(600, 800);
        int actualTaps = 0, actualSwipes = 0, holds = 0, images = 0;
        page.tapped = [&](QPoint) { ++actualTaps; };
        page.swiped = [&](int) { ++actualSwipes; };
        page.held = [&](QPoint) { ++holds; };
        page.imageHeld = [&](QPoint) {
            ++images;
            return false;
        };
        auto *device = QTest::createTouchDevice();
        const auto send = [&](bool down, quint32 timestamp, QPoint point) {
            if (!touch) {
                pointerEvent(&page, down ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
                             timestamp, point);
                return;
            }
            QEventPoint contact(0, down ? QEventPoint::Pressed : QEventPoint::Released, point,
                                page.mapToGlobal(point));
            QMutableEventPoint::setPosition(contact, point);
            QTouchEvent event(down ? QEvent::TouchBegin : QEvent::TouchEnd, device, Qt::NoModifier,
                              { contact });
            event.setTimestamp(timestamp);
            QApplication::sendEvent(&page, &event);
        };
        const QPoint point(300, 400);
        send(true, pressed, point);
        page.invalidateHoldTarget();
        page.invalidateHoldTarget(); // Repeated frame notifications must not change the outcome.
        if (cancel) {
            QTouchEvent event(QEvent::TouchCancel, device);
            QApplication::sendEvent(&page, &event);
        }
        send(false, released, point + travel);
        QCOMPARE(actualTaps, taps);
        QCOMPARE(actualSwipes, swipes);
        QCOMPARE(holds, 0);
        QCOMPARE(images, 0);
        send(true, 3000, point);
        send(false, 3709, point);
        QCOMPARE(holds, 1);
        QCOMPARE(images, 1);
        QCOMPARE(actualTaps, taps);
        QCOMPARE(actualSwipes, swipes);
    }

    // Checks that source time, not a timeout during blocked painting, decides an invalidated hold.
    void holdInvalidationStopsTimer()
    {
        ReadingPage page(nullptr, [] { });
        page.resize(600, 800);
        int taps = 0, holds = 0;
        page.tapped = [&](QPoint) { ++taps; };
        page.held = [&](QPoint) { ++holds; };
        auto *timer = page.findChild<QTimer *>("rrHoldTimer");
        QVERIFY(timer);
        QSignalSpy fired(timer, &QTimer::timeout);
        timer->setInterval(20);
        pointerEvent(&page, QEvent::MouseButtonPress, 1000, { 300, 400 });
        page.invalidateHoldTarget();
        QVERIFY(!fired.wait(50));
        pointerEvent(&page, QEvent::MouseButtonRelease, 1010, { 300, 400 });
        QCOMPARE(taps, 1);
        pointerEvent(&page, QEvent::MouseButtonPress, 2000, { 300, 400 });
        page.invalidateHoldTarget();
        pointerEvent(&page, QEvent::MouseButtonRelease, 2030, { 300, 400 });
        QCOMPARE(taps, 1);
        QCOMPARE(holds, 0);
    }

    void holdTimerDoesNotRepeatOnRelease()
    {
        ui::HoldTimer timer;
        timer.setSingleShot(true);
        timer.setInterval(20);
        QSignalSpy fired(&timer, &QTimer::timeout);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(), QPointF(), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), QPointF(), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        press.setTimestamp(1000);
        release.setTimestamp(2000);
        timer.start(&press);
        QTRY_COMPARE(fired.size(), 1);
        QVERIFY(!timer.finish(&release));
    }

    // Checks that clicking or pressing Space on the selected font keeps it checked, and that the help cell never becomes selected.
    void fontSelectionDoesNotToggle_data() { variants(); }

    void fontSelectionDoesNotToggle()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.resize(ui::px(1072), ui::px(1448));
        ui::applyReaderPalette(&host, ui::readerPalette(host.palette(), dark));
        ChoiceScreen picker(&host);
        picker.setFontGeometry(QRect(0, 0, host.width(), picker.fontPanelHeight()));
        QString current = "Libron";
        int changes = 0;
        picker.selected = [&](const QString &face) {
            if (face == current)
                return;
            current = face;
            ++changes;
            picker.selectValue(face);
        };
        host.show();
        Choice help{ "Aa", "", "fontHelp" };
        help.placeholder = true;
        picker.showChoices("Font",
                           { { "Publisher Fonts", "", "publisherFont" },
                             { "Libron", "Libron", "libronFont" },
                             help },
                           current, ChoiceScreen::Mode::Fonts);
        auto *libron = picker.findChild<QPushButton *>("libronFont");
        auto *publisher = picker.findChild<QPushButton *>("publisherFont");
        QVERIFY(libron && publisher);
        QVERIFY(libron->isChecked());
        for (int repeat = 0; repeat < 2; ++repeat) {
            QTest::mouseClick(libron, Qt::LeftButton);
            QCOMPARE(current, QString("Libron"));
            QVERIFY(libron->isChecked());
            QVERIFY(!publisher->isChecked());
            QCOMPARE(changes, 0);
        }
        QTest::mouseClick(publisher, Qt::LeftButton);
        QCOMPARE(current, QString());
        QCOMPARE(changes, 1);
        QVERIFY(publisher->isChecked());
        QVERIFY(!libron->isChecked());
        QTest::mouseClick(publisher, Qt::LeftButton);
        QCOMPARE(current, QString());
        QVERIFY(publisher->isChecked());
        QCOMPARE(changes, 1);
        QTest::keyClick(publisher, Qt::Key_Space);
        QVERIFY(publisher->isChecked());
        QCOMPARE(changes, 1);
        QTest::mouseClick(picker.findChild<QPushButton *>("fontHelp"), Qt::LeftButton);
        QVERIFY(publisher->isChecked());
        QVERIFY(!picker.findChild<QPushButton *>("fontHelp")->isChecked());
    }

    void fontHoldCancellation_data() { variants(); }

    void fontHoldCancellation()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.resize(ui::px(1072), ui::px(1448));
        ui::applyReaderPalette(&host, ui::readerPalette(host.palette(), dark));
        ChoiceScreen picker(&host);
        picker.setFontGeometry(QRect(0, 0, host.width(), picker.fontPanelHeight()));
        int selected = 0, defaults = 0;
        picker.selected = [&](const QString &) { ++selected; };
        QString requestedFace;
        bool instructions = false;
        picker.defaultRequested = [&](const QString &face, bool help) {
            ++defaults;
            requestedFace = face;
            instructions = help;
        };
        host.show();
        picker.showChoices("Font", { { "Libron", "Libron", "holdFont" } }, {},
                           ChoiceScreen::Mode::Fonts);
        QCoreApplication::processEvents();
        auto *cell = picker.findChild<QPushButton *>("holdFont");
        QVERIFY(cell && cell->isVisible());
        const auto point = cell->rect().center();
        pointerEvent(cell, QEvent::MouseButtonPress, 1000, point);
        QEvent cancel(QEvent::TouchCancel);
        QApplication::sendEvent(cell, &cancel);
        pointerEvent(cell, QEvent::MouseButtonRelease, 2000, point);
        QCOMPARE(selected, 0);
        QCOMPARE(defaults, 0);
        QVERIFY(!cell->isDown());
        pointerEvent(cell, QEvent::MouseButtonPress, 3000, point);
        pointerEvent(cell, QEvent::MouseButtonRelease, 3085, point);
        QCOMPARE(selected, 1);
        QCOMPARE(defaults, 0);
        pointerEvent(cell, QEvent::MouseButtonPress, 4000, point);
        pointerEvent(cell, QEvent::MouseButtonRelease, 5000, point);
        QCOMPARE(selected, 1);
        QCOMPARE(defaults, 1);
        pointerEvent(cell, QEvent::MouseButtonRelease, 5100, point);
        QCOMPARE(defaults, 1);
        Choice help{ "Aa", "", "fontHelp" };
        help.placeholder = true;
        picker.showChoices("Font", { { "Publisher Fonts", "", "publisherFont" }, help }, {},
                           ChoiceScreen::Mode::Fonts);
        cell = picker.findChild<QPushButton *>("publisherFont");
        QVERIFY(cell && cell->isVisible());
        pointerEvent(cell, QEvent::MouseButtonPress, 6000, cell->rect().center());
        pointerEvent(cell, QEvent::MouseButtonRelease, 7000, cell->rect().center());
        QCOMPARE(defaults, 2);
        QVERIFY(requestedFace.isEmpty());
        QVERIFY(!instructions);
        QCOMPARE(selected, 1);
        QTest::mouseClick(picker.findChild<QPushButton *>("fontHelp"), Qt::LeftButton);
        QCOMPARE(defaults, 3);
        QVERIFY(instructions);
        QCOMPARE(selected, 1);
    }

    void holdCallbackCanDeleteReader()
    {
        QPointer<ReadingPage> page = new ReadingPage(nullptr, [] { });
        page->resize(600, 800);
        page->show();
        page->held = [page](QPoint) { delete page.data(); };
        pointerEvent(page, QEvent::MouseButtonPress, 1000, QPoint(300, 400));
        pointerEvent(page, QEvent::MouseButtonRelease, 2000, QPoint(300, 400));
        QVERIFY(!page);
    }

    void canvasPreservesTouchTime()
    {
        class StampWidget final : public QWidget
        {
        public:
            ulong pressed = 0, released = 0;

            void mousePressEvent(QMouseEvent *event) override { pressed = event->timestamp(); }

            void mouseReleaseEvent(QMouseEvent *event) override { released = event->timestamp(); }
        };

        standalone::WidgetCanvas canvas(nullptr);
        auto *widget = new StampWidget;
        canvas.setContent(widget);
        auto *device = QTest::createTouchDevice();
        for (const auto type : { QEvent::TouchBegin, QEvent::TouchEnd }) {
            QTouchEvent event(type, device, Qt::NoModifier,
                              { QEventPoint(0,
                                            type == QEvent::TouchBegin ? QEventPoint::Pressed
                                                                       : QEventPoint::Released,
                                            QPointF(), QPointF()) });
            event.setTimestamp(type == QEvent::TouchBegin ? 1000 : 2000);
            QApplication::sendEvent(canvas.viewport(), &event);
        }
        QCOMPARE(widget->pressed, ulong(1000));
        QCOMPARE(widget->released, ulong(2000));
    }

    // Checks the widget keyboard: key fonts and layout, typing, symbols, selection replacement, deletion, submission and placement.
    void widgetKeyboardEntry_data() { variants(); }

    void widgetKeyboardEntry()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        host.resize(ui::px(1072), ui::px(1448));
        ui::applyReaderPalette(&host, ui::readerPalette(host.palette(), dark));
        QLineEdit editor(&host), foreign(&host);
        editor.setGeometry(20, 20, 400, 80);
        foreign.setGeometry(20, 110, 400, 80);
        auto keyboard = ui::makeWidgetKeyboard(&host, &editor);
        host.show();
        QVERIFY(keyboard->show());
        auto *panel = host.findChild<QWidget *>("widgetKeyboard");
        QVERIFY(panel && panel->isVisible());
        QVERIFY(host.rect().contains(panel->geometry()));
        QCOMPARE(host.property("reader_keyboard_bounds").toRect(), panel->geometry());
        QCOMPARE(host.property("reader_keyboard_editor_bounds").toRect(), editor.geometry());
        auto *firstKey = panel->findChild<QPushButton *>("widgetKey1_0");
        QVERIFY(firstKey);
        QCOMPARE(firstKey->font().family(), ui::interfaceFont().family());
        QCOMPARE(firstKey->font().pixelSize(), ui::px(48));
        QCOMPARE(panel->findChild<QPushButton *>("widgetKeyboardSymbols")->text(), QString("123"));
        QCOMPARE(firstKey->grab().toImage().pixelColor(5, 5), ui::themeColors(dark).paper);
        for (const auto *name :
             { "widgetKeyboardShift", "widgetKeyboardDelete", "widgetKeyboardDone" }) {
            auto *key = panel->findChild<QPushButton *>(name);
            QVERIFY(key && !key->icon().isNull());
            QVERIFY(!key->accessibleName().isEmpty());
            for (bool pressed : { false, true }) {
                key->setDown(pressed);
                const auto image = key->grab().toImage();
                const bool primary = key->objectName() == "widgetKeyboardDone";
                const bool inverted = primary ? !pressed : pressed;
                const auto background =
                        inverted ? ui::themeColors(dark).ink : ui::themeColors(dark).paper;
                const auto foreground =
                        inverted ? ui::themeColors(dark).paper : ui::themeColors(dark).ink;
                QCOMPARE(image.pixelColor(8, image.height() / 2), background);
                const auto keyboardImage = panel->grab().toImage();
                const auto corner = key->mapTo(panel, QPoint()) * keyboardImage.devicePixelRatio();
                QCOMPARE(keyboardImage.pixelColor(corner), ui::themeColors(dark).paper);
                int ink = 0;
                const auto area = image.rect().adjusted(image.width() / 4, image.height() / 4,
                                                        -image.width() / 4, -image.height() / 4);
                for (int y = area.top(); y <= area.bottom(); ++y)
                    for (int x = area.left(); x <= area.right(); ++x)
                        if (image.pixelColor(x, y) == foreground)
                            ++ink;
                QVERIFY2(ink > 4, name);
            }
            key->setDown(false);
        }
        auto *shift = panel->findChild<QPushButton *>("widgetKeyboardShift");
        shift->setChecked(true);
        const auto selectedShift = shift->grab().toImage();
        QCOMPARE(selectedShift.pixelColor(8, selectedShift.height() / 2),
                 ui::themeColors(dark).ink);
        shift->setChecked(false);
        auto *submit = panel->findChild<QPushButton *>("widgetKeyboardDone");
        auto *erase = panel->findChild<QPushButton *>("widgetKeyboardDelete");
        auto *symbols = panel->findChild<QPushButton *>("widgetKeyboardSymbols");
        QVERIFY(submit->width() > shift->width());
        QVERIFY(submit->width() > symbols->width());
        QVERIFY(erase->mapTo(panel, erase->rect().bottomRight()).y() < submit->geometry().top());
        QVERIFY(submit->geometry().contains(QPoint(erase->mapTo(panel, erase->rect().center()).x(),
                                                   submit->geometry().center().y())));
        int letters = 0;
        for (auto *key : panel->findChildren<QPushButton *>()) {
            const auto character = key->property("keyboard.character").toString();
            if (!key->isVisible() || character.isEmpty())
                continue;
            QVERIFY(character.front().isLetter());
            QCOMPARE(key->size(), firstKey->size());
            ++letters;
        }
        QCOMPARE(letters, 26);
        QString expected;
        QSet<QChar> characters;
        const auto typePage = [&] {
            for (auto *key : panel->findChildren<QPushButton *>()) {
                const auto character = key->property("keyboard.character").toString();
                if (!key->isVisible() || character.isEmpty())
                    continue;
                key->click();
                expected += character;
                characters.insert(character.front());
            }
        };
        const auto action = [&](const char *name) {
            auto *key = panel->findChild<QPushButton *>(name);
            if (key)
                key->click();
            return key != nullptr;
        };
        typePage();
        QVERIFY(action("widgetKeyboardShift"));
        QVERIFY(action("widgetKeyboardShift"));
        typePage();
        QVERIFY(action("widgetKeyboardSymbols"));
        QCOMPARE(panel->findChild<QPushButton *>("widgetKeyboardSymbols")->text(), QString("ABC"));
        typePage();
        QVERIFY(action("widgetKeyboardSpace"));
        expected += ' ';
        characters.insert(' ');
        for (ushort value = 32; value < 127; ++value)
            QVERIFY2(characters.contains(QChar(value)),
                     qPrintable(QString("Missing ASCII key %1").arg(value)));
        QCOMPARE(editor.text(), expected);
        QVERIFY(action("widgetKeyboardDelete"));
        expected.chop(1);
        QCOMPARE(editor.text(), expected);
        editor.setText("replace me");
        editor.selectAll();
        QVERIFY(action("widgetKeyboardSpace"));
        QCOMPARE(editor.text(), QString(" "));
        foreign.setFocus();
        QVERIFY(action("widgetKeyboardSpace"));
        QCOMPARE(foreign.text(), QString());
        QCOMPARE(editor.text(), QString("  "));
        keyboard->setSubmitText("Search");
        QCOMPARE(panel->findChild<QPushButton *>("widgetKeyboardDone")->text(), QString("Search"));
        QCOMPARE(panel->findChild<QPushButton *>("widgetKeyboardDone")->accessibleName(),
                 QString("Search"));
        int submitted = 0;
        keyboard->submitted = [&] { ++submitted; };
        QVERIFY(action("widgetKeyboardDone"));
        QCOMPARE(submitted, 1);
        QVERIFY(panel->isHidden());
        QVERIFY(host.property("reader_keyboard_bounds").toRect().isEmpty());
        QVERIFY(host.property("reader_keyboard_editor_bounds").toRect().isEmpty());
        QTest::mouseClick(&editor, Qt::LeftButton);
        QVERIFY(panel->isVisible());
        QVERIFY(keyboard->show());
        host.resize(800, 600);
        keyboard->place();
        QVERIFY(host.rect().contains(panel->geometry()));
        QCOMPARE(host.property("reader_keyboard_bounds").toRect(), panel->geometry());
        host.hide();
        QVERIFY(host.property("reader_keyboard_bounds").toRect().isEmpty());
        QVERIFY(host.property("reader_keyboard_editor_bounds").toRect().isEmpty());
        host.show();
        QPointer<QWidget> guardedPanel(panel);
        keyboard.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!guardedPanel);
        QVERIFY(host.property("reader_keyboard_bounds").toRect().isEmpty());
        QVERIFY(host.property("reader_keyboard_editor_bounds").toRect().isEmpty());
    }

    // Checks accent popups: delayed releases, uppercase variants, drag selection, Escape, cancellation, hiding and placement.
    void widgetKeyboardAccents_data() { variants(); }

    void widgetKeyboardAccents()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        host.resize(ui::px(1072), ui::px(1448));
        ui::applyReaderPalette(&host, ui::readerPalette(host.palette(), dark));
        QLineEdit editor(&host);
        auto keyboard = ui::makeWidgetKeyboard(&host, &editor);
        host.show();
        QVERIFY(keyboard->show());
        auto *panel = host.findChild<QWidget *>("widgetKeyboard");
        auto *e = panel->findChild<QPushButton *>("widgetKey1_2");
        auto *i = panel->findChild<QPushButton *>("widgetKey1_7");
        QVERIFY(e && i);
        const auto send = [](QPushButton *key, QEvent::Type type, ulong stamp) {
            const auto point = key->rect().center();
            QMouseEvent event(type, point, key->mapToGlobal(point), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            event.setTimestamp(stamp);
            QApplication::sendEvent(key, &event);
        };
        // A queued hold and release must open choices without typing the base
        // letter or choosing a variant underneath the original press point.
        send(e, QEvent::MouseButtonPress, 1000);
        send(e, QEvent::MouseButtonRelease, 2000);
        auto *popup = panel->findChild<QWidget *>("widgetKeyboardAccents");
        QVERIFY(popup && popup->isVisible());
        QVERIFY(panel->rect().contains(popup->geometry()));
        QCOMPARE(editor.text(), QString());
        auto *acute = popup->findChild<QPushButton *>("widgetAccent00e9");
        QVERIFY(acute);
        acute->click();
        QCOMPARE(editor.text(), QString::fromUtf8("é"));
        QVERIFY(!panel->findChild<QWidget *>("widgetKeyboardAccents"));
        panel->findChild<QPushButton *>("widgetKeyboardShift")->click();
        send(i, QEvent::MouseButtonPress, 3000);
        send(i, QEvent::MouseButtonRelease, 4000);
        popup = panel->findChild<QWidget *>("widgetKeyboardAccents");
        QVERIFY(popup);
        auto *dotted = popup->findChild<QPushButton *>("widgetAccent0130");
        QVERIFY(dotted);
        dotted->click();
        QCOMPARE(editor.text(), QString::fromUtf8("éİ"));
        panel->findChild<QPushButton *>("widgetKeyboardDelete")->click();
        QCOMPARE(editor.text(), QString::fromUtf8("é"));
        QTest::mousePress(e, Qt::LeftButton);
        QTRY_VERIFY(panel->findChild<QWidget *>("widgetKeyboardAccents"));
        QEvent cancel(QEvent::TouchCancel);
        QApplication::sendEvent(e, &cancel);
        QTest::mouseRelease(e, Qt::LeftButton);
        QVERIFY(!panel->findChild<QWidget *>("widgetKeyboardAccents"));
        QCOMPARE(editor.text(), QString::fromUtf8("é"));
        send(e, QEvent::MouseButtonPress, 4100);
        send(e, QEvent::MouseButtonRelease, 4700);
        QVERIFY(panel->findChild<QWidget *>("widgetKeyboardAccents"));
        QTest::keyClick(&editor, Qt::Key_Escape);
        QVERIFY(!panel->findChild<QWidget *>("widgetKeyboardAccents"));
        QCOMPARE(editor.text(), QString::fromUtf8("é"));
        send(e, QEvent::MouseButtonPress, 4800);
        QTRY_VERIFY(panel->findChild<QWidget *>("widgetKeyboardAccents"));
        popup = panel->findChild<QWidget *>("widgetKeyboardAccents");
        acute = popup->findChild<QPushButton *>("widgetAccent00e9");
        const auto destination = e->mapFromGlobal(acute->mapToGlobal(acute->rect().center()));
        QMouseEvent release(QEvent::MouseButtonRelease, destination, e->mapToGlobal(destination),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        release.setTimestamp(4900);
        QApplication::sendEvent(e, &release);
        QCOMPARE(editor.text(), QString::fromUtf8("éé"));
        QVERIFY(!panel->findChild<QWidget *>("widgetKeyboardAccents"));
        editor.setText(QString::fromUtf8("é"));
        send(e, QEvent::MouseButtonPress, 5000);
        send(e, QEvent::MouseButtonRelease, 5050);
        QCOMPARE(editor.text(), QString::fromUtf8("ée"));
        send(e, QEvent::MouseButtonPress, 6000);
        keyboard->hide();
        send(e, QEvent::MouseButtonRelease, 7000);
        QCOMPARE(editor.text(), QString::fromUtf8("ée"));
        QVERIFY(!panel->findChild<QWidget *>("widgetKeyboardAccents"));
    }

    // Checks the letter rows of each locale's keyboard layout and the switch to symbols and back.
    void widgetKeyboardLayouts_data()
    {
        QTest::addColumn<QString>("localeName");
        QTest::addColumn<QStringList>("rows");
        const QStringList qwerty{ "qwertyuiop", "asdfghjkl", "zxcvbnm" };
        QTest::newRow("Dutch") << QString("nl_NL") << qwerty;
        QTest::newRow("English") << QString("en_GB") << qwerty;
        QTest::newRow("German") << QString("de_DE")
                                << QStringList{ "qwertzuiop", "asdfghjkl", "yxcvbnm" };
        QTest::newRow("French") << QString("fr_FR")
                                << QStringList{ "azertyuiop", "qsdfghjklm", "wxcvbn" };
        QTest::newRow("French-Canada") << QString("fr_CA") << qwerty;
        QTest::newRow("fallback") << QString("ja_JP") << qwerty;
    }

    void widgetKeyboardLayouts()
    {
        QFETCH(QString, localeName);
        QFETCH(QStringList, rows);
        const QLocale original;
        const auto restore = qScopeGuard([original] { QLocale::setDefault(original); });
        QLocale::setDefault(QLocale(localeName));
        QWidget host;
        host.resize(1072, 1448);
        QLineEdit editor(&host);
        auto keyboard = ui::makeWidgetKeyboard(&host, &editor);
        host.show();
        QVERIFY(keyboard->show());
        auto *panel = host.findChild<QWidget *>("widgetKeyboard");
        for (int row = 0; row < rows.size(); ++row) {
            QString actual;
            for (int column = 0; column < rows[row].size(); ++column) {
                auto *key = panel->findChild<QPushButton *>(
                        QString("widgetKey%1_%2").arg(row + 1).arg(column));
                QVERIFY(key && key->isVisible());
                actual += key->property("keyboard.character").toString();
                key->click();
            }
            QCOMPARE(actual, rows[row]);
        }
        QCOMPARE(editor.text(), rows.join(""));
        auto *symbols = panel->findChild<QPushButton *>("widgetKeyboardSymbols");
        symbols->click();
        symbols->click();
        QCOMPARE(panel->findChild<QPushButton *>("widgetKey1_0")->text(), rows[0].left(1));
    }

    // Checks one-shot Shift, double-tap Caps Lock, missing or delayed timestamps, clock wrap and input at the length limit.
    void widgetKeyboardCase_data() { variants(); }

    void widgetKeyboardCase()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        host.resize(ui::px(1072), ui::px(1448));
        ui::applyReaderPalette(&host, ui::readerPalette(host.palette(), dark));
        QLineEdit editor(&host);
        auto keyboard = ui::makeWidgetKeyboard(&host, &editor);
        host.show();
        QVERIFY(keyboard->show());
        auto *panel = host.findChild<QWidget *>("widgetKeyboard");
        auto *shift = panel->findChild<QPushButton *>("widgetKeyboardShift");
        auto *q = panel->findChild<QPushButton *>("widgetKey1_0");
        auto *w = panel->findChild<QPushButton *>("widgetKey1_1");
        auto *symbols = panel->findChild<QPushButton *>("widgetKeyboardSymbols");
        auto *erase = panel->findChild<QPushButton *>("widgetKeyboardDelete");
        auto *space = panel->findChild<QPushButton *>("widgetKeyboardSpace");
        const auto tap = [&](ulong stamp, bool second = false) {
            pointerEvent(shift, second ? QEvent::MouseButtonDblClick : QEvent::MouseButtonPress,
                         stamp, shift->rect().center());
            pointerEvent(shift, QEvent::MouseButtonRelease, stamp + 20, shift->rect().center());
        };
        tap(1000);
        const auto once = shift->grab().toImage();
        QVERIFY(shift->isChecked());
        q->click();
        w->click();
        QCOMPARE(editor.text(), QString("Qw"));
        QVERIFY(!shift->isChecked());
        tap(2000);
        tap(2100, true);
        QVERIFY(shift->isChecked());
        QCOMPARE(shift->accessibleName(), strings::get("keyboard.caps_lock"));
        QVERIFY(shift->grab().toImage() != once);
        q->click();
        w->click();
        space->click();
        QCOMPARE(editor.text(), QString("QwQW "));
        QVERIFY(shift->isChecked());
        symbols->click();
        panel->findChild<QPushButton *>("widgetKey0_0")->click();
        symbols->click();
        q->click();
        QCOMPARE(editor.text(), QString("QwQW 1Q"));
        tap(2300);
        q->click();
        QVERIFY(editor.text().endsWith("Qq"));
        // Queued events must respect the real gap, not their delivery speed.
        tap(3000);
        tap(5000);
        QVERIFY(!shift->isChecked());
        // Unsigned source-clock wrap still permits a real double tap.
        tap(0xffffff00u);
        tap(0x20u);
        QCOMPARE(shift->accessibleName(), strings::get("keyboard.caps_lock"));
        tap(0x80u);
        tap(6000);
        erase->click();
        QVERIFY(shift->isChecked());
        symbols->click();
        panel->findChild<QPushButton *>("widgetKey0_0")->click();
        symbols->click();
        QVERIFY(shift->isChecked());
        q->click();
        w->click();
        QVERIFY(editor.text().endsWith("1Qw"));
        QVERIFY(!shift->isChecked());
        tap(7000);
        space->click();
        QVERIFY(!shift->isChecked());
        editor.setText("q");
        editor.selectAll();
        tap(8000);
        q->click();
        QCOMPARE(editor.text(), QString("Q"));
        QVERIFY(!shift->isChecked());
        editor.setMaxLength(1);
        tap(9000);
        w->click();
        QCOMPARE(editor.text(), QString("Q"));
        QVERIFY(shift->isChecked());
        erase->click();
        w->click();
        QCOMPARE(editor.text(), QString("W"));
        QVERIFY(!shift->isChecked());
        editor.setMaxLength(256);
        editor.clear();
        auto *e = panel->findChild<QPushButton *>("widgetKey1_2");
        tap(10000);
        pointerEvent(e, QEvent::MouseButtonPress, 11000, e->rect().center());
        pointerEvent(e, QEvent::MouseButtonRelease, 11600, e->rect().center());
        QVERIFY(panel->findChild<QWidget *>("widgetKeyboardAccents"));
        QTest::keyClick(&editor, Qt::Key_Escape);
        QVERIFY(shift->isChecked());
        QCOMPARE(editor.text(), QString());
        e->click();
        QCOMPARE(editor.text(), QString("E"));
        QVERIFY(!shift->isChecked());
        tap(12000);
        pointerEvent(e, QEvent::MouseButtonPress, 13000, e->rect().center());
        pointerEvent(e, QEvent::MouseButtonRelease, 13600, e->rect().center());
        auto *popup = panel->findChild<QWidget *>("widgetKeyboardAccents");
        QVERIFY(popup);
        auto *acute = popup->findChild<QPushButton *>("widgetAccent00c9");
        QVERIFY(acute);
        acute->click();
        QCOMPARE(editor.text(), QString::fromUtf8("EÉ"));
        QVERIFY(!shift->isChecked());
        const auto untimedTap = [&] {
            pointerEvent(shift, QEvent::MouseButtonPress, 0, shift->rect().center());
            pointerEvent(shift, QEvent::MouseButtonRelease, 0, shift->rect().center());
        };
        // The Qt5 touch fallback synthesizes mouse events without timestamps.
        untimedTap();
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        untimedTap();
        QVERIFY(!shift->isChecked());
        untimedTap();
        untimedTap();
        QCOMPARE(shift->accessibleName(), strings::get("keyboard.caps_lock"));
    }

    // Checks that the keyboard survives a deleted editor or parent, and destruction from its submit callback or a bounds callback in `show()`.
    void widgetKeyboardLifetime()
    {
        QWidget host;
        auto *editor = new QLineEdit(&host);
        auto keyboard = ui::makeWidgetKeyboard(&host, editor);
        host.show();
        QVERIFY(keyboard->show());
        delete editor;
        QVERIFY(!keyboard->show());
        keyboard->hide();
        keyboard.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QLineEdit survivingEditor;
        auto *parent = new QWidget;
        keyboard = ui::makeWidgetKeyboard(parent, &survivingEditor);
        delete parent;
        QVERIFY(!keyboard->show());
        keyboard->hide();
        keyboard->place();
        keyboard.reset();
        auto *nested = new QWidget(&host);
        keyboard = ui::makeWidgetKeyboard(nested, &survivingEditor);
        nested->show();
        QVERIFY(keyboard->show());
        delete nested;
        QVERIFY(host.property("reader_keyboard_bounds").toRect().isEmpty());
        QVERIFY(host.property("reader_keyboard_editor_bounds").toRect().isEmpty());
        keyboard.reset();
        QLineEdit submitEditor(&host);
        keyboard = ui::makeWidgetKeyboard(&host, &submitEditor);
        QVERIFY(keyboard->show());
        auto *done = host.findChild<QPushButton *>("widgetKeyboardDone");
        QVERIFY(done);
        int submissions = 0;
        keyboard->submitted = [&] {
            ++submissions;
            keyboard.reset();
        };
        done->click();
        QVERIFY(!keyboard);
        QVERIFY(!done->isVisible());
        done->click();
        QCOMPARE(submissions, 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        class CloseOnBounds final : public QObject
        {
        public:
            std::function<void()> close;

            bool eventFilter(QObject *object, QEvent *event) override
            {
                if (event->type() == QEvent::DynamicPropertyChange
                    && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()
                            == "reader_keyboard_bounds"
                    && !object->property("reader_keyboard_bounds").toRect().isEmpty()) {
                    const auto callback = std::exchange(close, {});
                    if (callback)
                        callback();
                }
                return false;
            }
        } closer;

        keyboard = ui::makeWidgetKeyboard(&host, &submitEditor);
        closer.close = [&] { keyboard.reset(); };
        host.installEventFilter(&closer);
        QVERIFY(!keyboard->show());
        QVERIFY(!keyboard);
        QVERIFY(host.property("reader_keyboard_bounds").toRect().isEmpty());
        QVERIFY(host.property("reader_keyboard_editor_bounds").toRect().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!host.findChild<QWidget *>("widgetKeyboard"));
    }

    void cancelledCanvasTouchRestoresSlider_data()
    {
        QTest::addColumn<bool>("secondFinger");
        QTest::newRow("touch-cancel") << false;
        QTest::newRow("second-finger") << true;
    }

    void cancelledCanvasTouchRestoresSlider()
    {
        QFETCH(bool, secondFinger);
        standalone::WidgetCanvas canvas(nullptr);
        canvas.resize(400, 300);
        auto *host = new QWidget;
        auto *slider = new ui::SettingSlider(host, 0, 100);
        slider->setGeometry(0, 0, 350, 100);
        slider->setValue(25);
        int commits = 0;
        slider->committed = [&](int) { ++commits; };
        canvas.setContent(host);
        canvas.fitContent();
        canvas.show();
        host->show();
        auto *device = QTest::createTouchDevice();
        QTouchEvent begin(QEvent::TouchBegin, device, Qt::NoModifier,
                          { QEventPoint(0, QEventPoint::Pressed, QPointF(), QPointF()) });
        QApplication::sendEvent(canvas.viewport(), &begin);
        QCOMPARE(slider->value(), 0);
        QTouchEvent cancelled(
                secondFinger ? QEvent::TouchUpdate : QEvent::TouchCancel, device, Qt::NoModifier,
                secondFinger ? QList<QEventPoint>{ QEventPoint(0, QEventPoint::Stationary,
                                                               QPointF(), QPointF()),
                                                   QEventPoint(1, QEventPoint::Pressed,
                                                               QPointF(10, 10), QPointF(10, 10)) }
                             : QList<QEventPoint>{});
        QApplication::sendEvent(canvas.viewport(), &cancelled);
        QCOMPARE(slider->value(), 25);
        QCOMPARE(commits, 0);
        QMouseEvent move(QEvent::MouseMove, QPointF(300, 50), QPointF(300, 50), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(slider, &move);
        QCOMPARE(slider->value(), 25);
    }

    void canvasCancellationCanDeleteTarget()
    {
        standalone::WidgetCanvas canvas(nullptr);
        canvas.resize(400, 300);
        auto *host = new QWidget;
        QPointer<DeleteOnTouchCancel> button = new DeleteOnTouchCancel(host);
        button->setGeometry(0, 0, 350, 100);
        canvas.setContent(host);
        canvas.fitContent();
        canvas.show();
        host->show();
        auto *device = QTest::createTouchDevice();
        QTouchEvent begin(QEvent::TouchBegin, device, Qt::NoModifier,
                          { QEventPoint(0, QEventPoint::Pressed, QPointF(), QPointF()) });
        QApplication::sendEvent(canvas.viewport(), &begin);
        QVERIFY(button->isDown());
        QTouchEvent cancel(QEvent::TouchCancel, device);
        QApplication::sendEvent(canvas.viewport(), &cancel);
        QVERIFY(button.isNull());
        QApplication::sendEvent(canvas.viewport(), &cancel);
    }

    void canvasCancellationKeepsReentrantGesture()
    {
        standalone::WidgetCanvas canvas(nullptr);
        canvas.resize(400, 300);
        auto *host = new QWidget;
        auto *first = new QPushButton(host);
        first->setGeometry(0, 0, 180, 100);
        auto *second = new QPushButton(host);
        second->setGeometry(200, 0, 180, 100);
        QSignalSpy firstClicks(first, &QPushButton::clicked),
                secondClicks(second, &QPushButton::clicked);
        canvas.setContent(host);
        canvas.fitContent();
        canvas.show();
        host->show();
        auto *device = QTest::createTouchDevice();
        const auto send = [&](QEvent::Type type, QPointF position) {
            QTouchEvent event(type, device, Qt::NoModifier,
                              { QEventPoint(0,
                                            type == QEvent::TouchBegin ? QEventPoint::Pressed
                                                                       : QEventPoint::Released,
                                            position, position) });
            QApplication::sendEvent(canvas.viewport(), &event);
        };
        send(QEvent::TouchBegin, QPointF(50, 50));
        QVERIFY(first->isDown());
        // Cancellation callbacks may begin the next gesture before cancel returns.
        bool firstWasReleased = false;
        canvas.cancelGesture = [&] {
            firstWasReleased = !first->isDown();
            first->move(200, 0);
            second->move(0, 0);
            send(QEvent::TouchBegin, QPointF());
        };
        QTouchEvent cancel(QEvent::TouchCancel, device);
        QApplication::sendEvent(canvas.viewport(), &cancel);
        QVERIFY(firstWasReleased);
        QVERIFY(second->isDown());
        send(QEvent::TouchEnd, QPointF());
        QCOMPARE(firstClicks.count(), 0);
        QCOMPARE(secondClicks.count(), 1);
        QVERIFY(!second->isDown());
    }

    void canvasReleaseCanDeleteWidgets_data()
    {
        QTest::addColumn<bool>("deleteContent");
        QTest::newRow("target") << false;
        QTest::newRow("content") << true;
    }

    void canvasReleaseCanDeleteWidgets()
    {
        QFETCH(bool, deleteContent);
        standalone::WidgetCanvas canvas(nullptr);
        canvas.resize(400, 300);
        QPointer<QWidget> host = new QWidget;
        QPointer<QPushButton> button = new QPushButton(host);
        button->setGeometry(0, 0, 350, 100);
        int clicks = 0;
        connect(button, &QPushButton::clicked, &canvas, [&] {
            ++clicks;
            if (deleteContent)
                delete host.data();
            else
                delete button.data();
        });
        canvas.setContent(host);
        canvas.fitContent();
        canvas.show();
        host->show();
        auto *device = QTest::createTouchDevice();
        QTouchEvent begin(
                QEvent::TouchBegin, device, Qt::NoModifier,
                { QEventPoint(0, QEventPoint::Pressed, QPointF(50, 50), QPointF(50, 50)) });
        QApplication::sendEvent(canvas.viewport(), &begin);
        QVERIFY(button->isDown());
        QTouchEvent end(
                QEvent::TouchEnd, device, Qt::NoModifier,
                { QEventPoint(0, QEventPoint::Released, QPointF(50, 50), QPointF(50, 50)) });
        QApplication::sendEvent(canvas.viewport(), &end);
        QCOMPARE(clicks, 1);
        QVERIFY(button.isNull());
        QCOMPARE(canvas.content.isNull(), deleteContent);
        QTouchEvent cancel(QEvent::TouchCancel, device);
        QApplication::sendEvent(canvas.viewport(), &cancel);
        QApplication::sendEvent(canvas.viewport(), &end);
        QCOMPARE(clicks, 1);
    }

    void canvasInputRespectsModalBarrier()
    {
        auto *surface = new QWidget;
        surface->resize(420, 600);
        auto *button = new ui::Button("Underlying action", surface);
        button->setGeometry(20, 20, 180, 60);
        QSignalSpy activated(button, &QPushButton::clicked);
        auto *overlay = new ui::NoticeModal(surface, { "Modal input barrier", "Notice", {} },
                                            { "OK", "testModalConfirm" });
        int dismissals = 0;
        overlay->dismissed = [&] { ++dismissals; };
        Canvas canvas(surface, QSize(420, 600));
        canvas.resize(420, 600);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(click(canvas, button));
        QCOMPARE(activated.count(), 1);
        button->setEnabled(false);
        QVERIFY(!click(canvas, button));
        QVERIFY(clickAt(canvas, button->geometry().center()));
        QCOMPARE(activated.count(), 1);
        button->setEnabled(true);
        button->hide();
        QVERIFY(!click(canvas, button));
        button->show();
        for (bool dark : { false, true }) {
            surface->setProperty("reader_dark", dark);
            auto colors = surface->palette();
            colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
            colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
            surface->setPalette(colors);
            overlay->fitCard();
            overlay->show();
            overlay->raise();
            QTRY_COMPARE(overlay->geometry(), surface->rect());
            QVERIFY(overlay->rect().contains(overlay->card()->geometry()));
            QVERIFY(!click(canvas, button));
            QCOMPARE(activated.count(), 1);
            const int before = dismissals;
            QVERIFY(clickAt(canvas, button->geometry().center()));
            QCOMPARE(dismissals, before + 1);
            QCOMPARE(activated.count(), 1);
            surface->setFixedSize(dark ? QSize(420, 600) : QSize(600, 420));
            QTRY_COMPARE(overlay->geometry(), surface->rect());
            QVERIFY(overlay->rect().contains(overlay->card()->geometry()));
            QVERIFY((overlay->card()->geometry().center() - overlay->rect().center())
                            .manhattanLength()
                    <= 2);
            QVERIFY(!click(canvas, button));
            QCOMPARE(activated.count(), 1);
            overlay->hide();
        }
        QVERIFY(click(canvas, button));
        QCOMPARE(activated.count(), 2);
    }

    void annotationModeNotice_data() { variants(); }

    void annotationModeNotice()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        ui::readerStyle(&host, ui::styles::reader());
        host.resize(ui::px(1072), ui::px(1448));
        ui::AnnotationModeNotice notice(&host);
        notice.centerIn(host.size());
        host.show();
        notice.show();
        notice.setModeEnabled(true);
        QCoreApplication::processEvents();
        const auto enabled = notice.grab().toImage();
        notice.setModeEnabled(false);
        const auto disabled = notice.grab().toImage();
        const QRect icon(ui::metrics::lightSwipe::inset,
                         (notice.height() - ui::metrics::lightSwipe::iconWidth) / 2,
                         ui::metrics::lightSwipe::iconWidth, ui::metrics::lightSwipe::iconWidth);
        QVERIFY(enabled.copy(icon) != disabled.copy(icon));
        QCOMPARE(notice.size(),
                 QSize(ui::metrics::lightSwipe::width, ui::metrics::lightSwipe::height));
        QVERIFY(notice.testAttribute(Qt::WA_TransparentForMouseEvents));
        const auto captures = qEnvironmentVariable("KOBALT_CAPTURE_MODALS");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            const auto suffix = QString("-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi);
            QVERIFY(enabled.save(captures + "/mode-on" + suffix));
            QVERIFY(disabled.save(captures + "/mode-off" + suffix));
        }
    }

    void regularIconWeight_data() { variants(); }

    void regularIconWeight()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        const auto bounds = [](const QImage &image) {
            QRect result;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) > 128)
                        result = result.united(QRect(x, y, 1, 1));
            return result;
        };
        for (qreal dpr : { 1., 1.5, 2. }) {
            qreal firstScale = 0;
            for (auto glyph :
                 { ui::ControlIcon::Search, ui::ControlIcon::Settings, ui::ControlIcon::Annotate,
                   ui::ControlIcon::Bookmark, ui::ControlIcon::FolderPlus, ui::ControlIcon::Folders,
                   ui::ControlIcon::Dictionary }) {
                ui::Button button;
                button.setProperty("reader_dark", dark);
                button.setIconConfiguration({ glyph, ui::IconRendition::Action });
                const int side = 256;
                ui::Button source;
                source.setProperty("reader_dark", dark);
                source.setIconConfiguration({ glyph, ui::IconRendition::OriginalAsset });
                const auto original = source.icon().pixmap(QSize(side, side), dpr).toImage();
                const auto action = button.icon().pixmap(QSize(side, side), dpr).toImage();
                ui::ListRow row(nullptr);
                row.setProperty("reader_dark", dark);
                row.setLeadingIcon(glyph);
                QCOMPARE(row.icon().pixmap(QSize(side, side), dpr).toImage(), action);
                const auto originalBounds = bounds(original), actionBounds = bounds(action);
                QVERIFY(!originalBounds.isEmpty() && !actionBounds.isEmpty());
                const qreal scale = qreal(qMax(actionBounds.width(), actionBounds.height()))
                        / qMax(originalBounds.width(), originalBounds.height());
                if (firstScale == 0)
                    firstScale = scale;
                QVERIFY2(
                        qAbs(scale - firstScale) < .015,
                        "Regular icons were resized by different factors, changing their stroke weight");
            }
        }
    }

    void libraryFolderPicker_data() { variants(); }

    void libraryFolderPicker()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        library::Index index;
        auto *view = new library::LibraryView(index, {});
        view->setDark(dark);
        Canvas canvas(view, QSize(ui::px(1072), ui::px(1448)));
        canvas.resize(view->size());
        canvas.show();
        QCoreApplication::processEvents();
        for (const auto &name : { "libraryChooseFolder", "libraryOpenBook", "libraryFoldersAdd",
                                  "libraryMoreAddFolder", "libraryMoreOpenBook" }) {
            if (QString::fromLatin1(name).startsWith("libraryMore")
                || QString::fromLatin1(name) == "libraryFoldersAdd") {
                view->findChild<QPushButton *>("libraryMore")->click();
                if (QString::fromLatin1(name) == "libraryFoldersAdd")
                    view->findChild<QPushButton *>("libraryMoreFolders")->click();
            }
            auto *button = view->findChild<QPushButton *>(name);
            QVERIFY(button && button->isVisible());
            QCoreApplication::processEvents();
            const QRect area(button->mapTo(view, QPoint()), button->size());
            const auto before = view->grab().toImage().copy(area);
            const bool remainsVisible = !QString::fromLatin1(name).startsWith("libraryMore");
            bool entered = false, updatesEnabled = false;
            QImage during;
            const auto pickerAction = [&] {
                // File pickers run a nested event loop inside the host callback.
                // Use a plain dialog so this check remains fully headless.
                QDialog picker(&canvas);
                QTimer::singleShot(0, &picker, [&] {
                    entered = true;
                    updatesEnabled = button->updatesEnabled();
                    if (remainsVisible)
                        during = view->grab().toImage().copy(area);
                    picker.reject();
                });
                picker.exec();
            };
            view->addFolder = pickerAction;
            view->openFile = pickerAction;
            QVERIFY(click(canvas, button));
            QTRY_VERIFY(entered);
            QVERIFY2(updatesEnabled,
                     "The folder button cannot paint while the native picker is open");
            if (remainsVisible) {
                QCOMPARE(during, before);
                QVERIFY(button->isVisible());
                QCOMPARE(view->grab().toImage().copy(area), before);
            }
            if (QString::fromLatin1(name) == "libraryFoldersAdd")
                view->findChild<QPushButton *>("libraryFoldersBack")->click();
        }
    }

    // Checks the Library action buttons and More menu: frames, labels in each state, icons, accessible names, dismissal and callbacks.
    void libraryActionButtons_data() { variants(); }

    void libraryActionButtons()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        library::Index index;
        library::LibraryView view(index, {});
        view.resize(ui::px(1072), ui::px(1448));
        view.setDark(dark);
        view.show();
        int added = 0, opened = 0, rescanned = 0;
        view.addFolder = [&] { ++added; };
        view.openFile = [&] { ++opened; };
        view.rescan = [&] { ++rescanned; };
        const QColor ink = dark ? Qt::white : Qt::black;
        const QColor paper = dark ? Qt::black : Qt::white;
        const auto checkButton = [&](const char *name, bool primary) {
            auto *button = view.findChild<QPushButton *>(name);
            QVERIFY(button);
            QVERIFY(button->isVisible());
            QCoreApplication::processEvents();
            const auto image = button->grab().toImage();
            // Standalone actions have a complete frame, unlike footer cells.
            const QColor border(ui::themeColors(dark).outline);
            QCOMPARE(image.pixelColor(0, image.height() / 2), border);
            QCOMPARE(image.pixelColor(image.width() - 1, image.height() / 2), border);
            QCOMPARE(image.pixelColor(image.width() / 2, image.height() - 1), border);
            QCOMPARE(image.pixelColor(image.width() / 2, ui::px(8)), primary ? ink : paper);
            QVERIFY(button->height() >= QFontMetrics(button->font()).height() + ui::px(16));
            const auto checkLabel = [&](const char *state) {
                const auto labeled = button->grab().toImage();
                const auto labelArea =
                        labeled.rect().adjusted(ui::px(12), ui::px(4), -ui::px(12), -ui::px(4));
                const auto label = button->text();
                button->setText({});
                const auto blank = button->grab().toImage();
                button->setText(label);
                QVERIFY2(
                        labeled.copy(labelArea) != blank.copy(labelArea),
                        qPrintable(QString("Button label is invisible: %1 (%2)").arg(name, state)));
            };
            checkLabel("normal");
            button->setDown(true);
            checkLabel("pressed");
            button->setDown(false);
            button->setEnabled(false);
            checkLabel("disabled");
            button->setEnabled(true);
            button->click();
        };
        const auto capture = [&](const QString &scene) {
            const auto directory = qEnvironmentVariable("KOBALT_CAPTURE_MODALS");
            if (directory.isEmpty())
                return;
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(view.grab().save(directory
                                     + QString("/library-%1-%2-%3.png")
                                               .arg(scene, dark ? "dark" : "light")
                                               .arg(ppi)));
        };
        QCoreApplication::processEvents();
        capture("empty");
        checkButton("libraryChooseFolder", false);
        checkButton("libraryOpenBook", true);
        QVERIFY(!view.findChild<QPushButton *>("libraryLeave"));
        QTRY_COMPARE(added, 1);
        QTRY_COMPARE(opened, 1);
        library::Book book;
        book.key = "overflow-test";
        book.title = "The Lantern Keeper";
        index.books.insert(book.key, book);
        view.refresh();
        const auto libraryImage = view.grab().toImage();
        view.findChild<QPushButton *>("libraryMore")->click();
        auto *menu = dynamic_cast<ui::ActionMenu *>(view.findChild<QWidget *>("libraryMoreSheet"));
        QVERIFY(menu && menu->inputBarrier());
        QCOMPARE(menu->geometry(), view.rect());
        QVERIFY(view.rect().contains(menu->card()->geometry()));
        const QRect backdrop(0, 0, ui::px(24), view.height());
        QVERIFY(view.grab().toImage().copy(backdrop) != libraryImage.copy(backdrop));
        for (const auto &item : { qMakePair("libraryMoreAddFolder", ui::ControlIcon::FolderPlus),
                                  qMakePair("libraryMoreOpenBook", ui::ControlIcon::Dictionary),
                                  qMakePair("libraryMoreFolders", ui::ControlIcon::Folders) }) {
            auto *row = menu->action(item.first);
            QVERIFY(row && row->isVisible());
            const int side = ui::metrics::control::iconSize;
            const QRect iconArea(ui::metrics::chrome::inset, (row->height() - side) / 2, side,
                                 side);
            QImage expected(side, side, QImage::Format_ARGB32);
            expected.fill(paper);
            {
                QPainter painter(&expected);
                ui::controlIcon(item.second, ink).paint(&painter, expected.rect());
            }
            QImage empty(side, side, QImage::Format_ARGB32);
            empty.fill(paper);
            QVERIFY(expected != empty);
            const auto labeled = row->grab().toImage();
            QCOMPARE(labeled.copy(iconArea).convertToFormat(QImage::Format_ARGB32), expected);
            const auto text = row->text();
            QVERIFY(!text.isEmpty());
            QCOMPARE(row->accessibleName(), text);
            row->setText({});
            const auto blank = row->grab().toImage();
            row->setText(text);
            const int labelLeft = iconArea.right() + 1 + ui::metrics::chrome::selectorGap;
            const QRect labelArea(labelLeft, 0, row->width() - labelLeft, row->height());
            QVERIFY(labeled.copy(labelArea) != blank.copy(labelArea));
            QCOMPARE(labeled.copy(0, 0, labelLeft, row->height()),
                     blank.copy(0, 0, labelLeft, row->height()));
        }
        capture("more");
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                          QPoint(ui::px(8), view.height() - ui::px(8)));
        QVERIFY(!menu->isVisible());
        QCOMPARE(added, 1);
        QCOMPARE(opened, 1);
        view.findChild<QPushButton *>("libraryMore")->click();
        QVERIFY(menu->isVisible());
        view.resize(ui::px(1448), ui::px(1072));
        QCoreApplication::processEvents();
        QCOMPARE(menu->geometry(), view.rect());
        QVERIFY(view.rect().contains(menu->card()->geometry()));
        view.findChild<QPushButton *>("libraryMoreFolders")->click();
        QVERIFY(!menu->isVisible());
        capture("folders");
        checkButton("libraryFoldersAdd", true);
        checkButton("libraryFoldersRescan", false);
        QTRY_COMPARE(added, 2);
        QCOMPARE(rescanned, 1);
    }

    // About opens without a book, keeps its metadata and controls reachable, and unwinds nested Back actions.
    void libraryAbout_data() { variants(); }

    void libraryAbout()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        for (qreal scale : { 1.0, 1.5 }) {
            ui::setLargeText(scale);
            const auto resetScale = qScopeGuard([] { ui::setLargeText(1); });
            library::Index index;
            library::LibraryView view(index, {});
            view.setDark(dark);
            view.resize(ui::px(1072), ui::px(1448));
            view.show();
            int opened = 0;
            view.openBook = [&](const QString &) { ++opened; };
            view.openFile = [&] { ++opened; };
            const auto showAbout = [&] {
                view.findChild<QPushButton *>("libraryMore")->click();
                auto *menu = dynamic_cast<ui::ActionMenu *>(
                        view.findChild<QWidget *>("libraryMoreSheet"));
                QVERIFY(menu);
                QCOMPARE(menu->actions().last()->objectName(), QString("libraryMoreAbout"));
                QCOMPARE(menu->actions().last()->accessibleName(), QString("About Kobalt"));
                menu->actions().last()->click();
                QVERIFY(!menu->isVisible());
            };
            showAbout();
            auto *about = view.findChild<QWidget *>("libraryAbout");
            QVERIFY(about && about->isVisible());
            const auto captureDirectory = qEnvironmentVariable("KOBALT_CAPTURE_MODALS");
            if (!captureDirectory.isEmpty() && scale == 1 && ppi == 300) {
                QVERIFY(QDir().mkpath(captureDirectory));
                QVERIFY(view.grab().save(captureDirectory
                                         + (dark ? "/about-dark.png" : "/about-light.png")));
            }
            AboutPage reference;
            reference.ensurePolished();
            for (const auto *name : { "rrAboutVersion", "rrAboutCommit" }) {
                auto *shown = about->findChild<QLabel *>(name);
                auto *expected = reference.findChild<QLabel *>(name);
                QVERIFY(shown && expected);
                QCOMPARE(shown->text(), expected->text());
                QCOMPARE(shown->font(), expected->font());
            }
            for (const auto size :
                 { QSize(1072, 1448), QSize(1448, 1072), QSize(600, 1000), QSize(1000, 600) }) {
                view.resize(ui::px(size.width()), ui::px(size.height()));
                QCoreApplication::processEvents();
                QCOMPARE(about->geometry(), view.rect());
                auto *hit = view.childAt(view.rect().center());
                QVERIFY2(hit && (hit == about || about->isAncestorOf(hit)),
                         "About must remain above the empty Library after resizing");
                QCOMPARE(about->palette().color(QPalette::Window),
                         QColor(ui::themeColors(dark).paper));
                auto *back = about->findChild<QPushButton *>("libraryAboutBack");
                QVERIFY(about->rect().contains(back->geometry()));
                auto *title = about->findChild<QWidget *>("libraryAboutTitle");
                QVERIFY(title);
                const auto inkCenter = [dark](const QImage &image) {
                    int top = image.height(), bottom = -1;
                    const int paper = dark ? 0 : 255;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x)
                            if (qAbs(qGray(image.pixel(x, y)) - paper) > 96) {
                                top = qMin(top, y);
                                bottom = qMax(bottom, y);
                            }
                    return (top + bottom) / 2.0;
                };
                const auto arrow = back->grab().toImage();
                // Ignore the Back cell's trailing vertical divider.
                const auto arrowInk = arrow.copy(0, 0, arrow.width() - ui::px(8), arrow.height());
                const qreal centerDifference =
                        qAbs(inkCenter(title->grab().toImage()) - inkCenter(arrowInk));
                QVERIFY2(centerDifference <= ui::px(2),
                         qPrintable(QString("Title/arrow ink centers differ by %1 pixels")
                                            .arg(centerDifference)));
                auto *body = about->findChild<QWidget *>("rrAbout");
                QVERIFY(body);
                auto *clip = body->parentWidget();
                const auto exposed = [clip](QWidget *widget) {
                    return clip->rect().contains(
                            QRect(widget->mapTo(clip, QPoint()), widget->size()));
                };
                auto *next = about->findChild<QPushButton *>("libraryAboutNext");
                auto *previous = about->findChild<QPushButton *>("libraryAboutPrevious");
                while (previous->isVisible() && previous->isEnabled())
                    previous->click();
                bool versionSeen = false, commitSeen = false, actionSeen = false, buildSeen = false;
                for (int turns = 0; turns < 20; ++turns) {
                    versionSeen |= exposed(about->findChild<QWidget *>("rrAboutVersion"));
                    commitSeen |= exposed(about->findChild<QWidget *>("rrAboutCommit"));
                    actionSeen |= exposed(about->findChild<QWidget *>("rrOpenAcknowledgments"));
                    buildSeen |= exposed(about->findChild<QWidget *>("rrAboutVersion"))
                            && exposed(about->findChild<QWidget *>("rrAboutCommit"));
                    if (!next->isVisible() || !next->isEnabled())
                        break;
                    view.pageButton(1);
                }
                QVERIFY2(
                        versionSeen && commitSeen && actionSeen && buildSeen,
                        qPrintable(
                                QString("%1x%2 scale %3: version %4, commit %5, action %6; body %7x%8, clip %9x%10")
                                        .arg(size.width())
                                        .arg(size.height())
                                        .arg(scale)
                                        .arg(versionSeen)
                                        .arg(commitSeen)
                                        .arg(actionSeen)
                                        .arg(body->width())
                                        .arg(body->height())
                                        .arg(clip->width())
                                        .arg(clip->height())));
            }
            auto *acknowledgments = dynamic_cast<Acknowledgments *>(
                    about->findChild<QWidget *>("rrAcknowledgments"));
            QVERIFY(acknowledgments);
            view.resize(ui::px(1072), ui::px(1448));
            QCoreApplication::processEvents();
            showAbout();
            about->findChild<QPushButton *>("rrOpenAcknowledgments")->click();
            QVERIFY(acknowledgments->isVisible());
            view.resize(ui::px(1000), ui::px(600));
            QCoreApplication::processEvents();
            auto *hint = about->findChild<QWidget *>("libraryAboutContinuationHint");
            QVERIFY(hint && hint->isVisible());
            const auto creditsFrame = about->grab().toImage();
            hint->hide();
            QCOMPARE(about->grab().toImage(), creditsFrame);
            acknowledgments->findChild<QPushButton *>("rrAcknowledgment0")->click();
            QVERIFY(acknowledgments->findChild<QWidget *>("rrLicenseText")->isVisible());
            view.resize(ui::px(1072), ui::px(1448));
            QCoreApplication::processEvents();
            view.resize(ui::px(1000), ui::px(600));
            QCoreApplication::processEvents();
            QVERIFY(hint->isVisible());
            const auto licenseFrame = about->grab().toImage();
            hint->hide();
            QCOMPARE(about->grab().toImage(), licenseFrame);
            QVERIFY(view.dismiss());
            QVERIFY(acknowledgments->isVisible());
            QVERIFY(!acknowledgments->findChild<QWidget *>("rrLicenseText")->isVisible());
            QVERIFY(view.dismiss());
            QVERIFY(!acknowledgments->isVisible());
            QVERIFY(about->isVisible());
            QVERIFY(view.dismiss());
            QVERIFY(!about->isVisible());
            QVERIFY(!view.dismiss());
            QCOMPARE(opened, 0);
            QVERIFY(index.books.isEmpty() && index.folders.isEmpty());
            showAbout();
            about->findChild<QPushButton *>("libraryAboutBack")->click();
            QVERIFY(!about->isVisible());
        }
    }

    // Checks the list layout: toolbar-to-pager bounds, even row dividers, hit targets to each row's last pixel and the pager's space as books change.
    void libraryListFillsPage_data() { variants(); }

    void libraryListFillsPage()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        library::Index index;
        index.settings.view = library::View::List;
        for (int i = 0; i < 40; ++i) {
            library::Book book;
            book.key = QString("book-%1").arg(i);
            book.title = QString("An invented book %1").arg(i);
            book.author = "Aven Teral";
            book.series = "The Neralis Cycle";
            index.books.insert(book.key, book);
        }
        library::LibraryView view(index, {});
        view.setDark(dark);
        view.show();
        auto *pages = view.findChild<QWidget *>("libraryPages");
        auto *counter = view.findChild<QLabel *>("libraryPage");
        QVERIFY(pages && counter);
        auto *pager = counter->parentWidget();
        QString opened;
        view.openBook = [&](const QString &key) { opened = key; };
        for (const auto size : { QSize(1072, 1448), QSize(1448, 1072), QSize(1072, 1451) }) {
            view.resize(ui::px(size.width()), ui::px(size.height()));
            QCoreApplication::processEvents();
            QCOMPARE(pages->y(), int(ui::metrics::chrome::actionHeight) + ui::px(88));
            QVERIFY(pager->isVisible());
            QCOMPARE(pages->geometry().bottom() + 1, pager->y());
            const auto image = pages->grab().toImage();
            const auto divider = ui::dividerColor(pages);
            QVector<int> boundaries{ 0 };
            for (int y = 0; y < image.height(); ++y)
                if (image.pixelColor(0, y) == divider
                    && (y + 1 == image.height() || image.pixelColor(0, y + 1) != divider))
                    boundaries.append(y + 1);
            QVERIFY(boundaries.size() > 2);
            QCOMPARE(boundaries.last(), image.height());
            int shortest = image.height(), tallest = 0;
            for (int i = 1; i < boundaries.size(); ++i) {
                const int height = boundaries[i] - boundaries[i - 1];
                shortest = qMin(shortest, height);
                tallest = qMax(tallest, height);
                opened.clear();
                // Even the final pixel of each row belongs to that row.
                QTest::mouseClick(pages, Qt::LeftButton, Qt::NoModifier,
                                  QPoint(1, boundaries[i] - 1));
                QVERIFY(!opened.isEmpty());
            }
            QVERIFY(tallest - shortest <= 1);
            const auto directory = qEnvironmentVariable("KOBALT_CAPTURE_MODALS");
            if (!directory.isEmpty()) {
                QVERIFY(QDir().mkpath(directory));
                QVERIFY(view.grab().save(directory
                                         + QString("/library-list-%1-%2-%3x%4.png")
                                                   .arg(dark ? "dark" : "light")
                                                   .arg(ppi)
                                                   .arg(size.width())
                                                   .arg(size.height())));
            }
        }
        // Five rows fit without a pager here, but not with its old space
        // still reserved. Removing books must reclaim that space immediately.
        view.resize(ui::px(1072), ui::px(1400));
        while (index.books.size() > 5) {
            auto last = index.books.end();
            index.books.erase(--last);
        }
        view.refresh();
        QTRY_VERIFY(!pager->isVisible());
        QCOMPARE(pages->geometry().bottom(), view.height() - 1);
        library::Book sixth;
        sixth.key = "another-book";
        sixth.title = "Another invented book";
        index.books.insert(sixth.key, sixth);
        view.refresh();
        QTRY_VERIFY(pager->isVisible());
        QCOMPARE(counter->text(), QString("1 of 2"));
        QCOMPARE(pages->geometry().bottom() + 1, pager->y());
    }

    // Checks that the startup widget and system-bar bands follow the Library palette through repeated theme changes.
    void libraryThemeSurface_data() { variants(); }

    void libraryThemeSurface()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        library::Store store(directory.path(), std::make_shared<library::LocalSource>());
        QWidget host;
        host.setAutoFillBackground(true);
        host.setPalette(ui::readerPalette(host.palette(), !dark));
        host.resize(600, 800);
        host.setContentsMargins(0, 60, 0, 40);
        standalone::StartScreen start(test::makeHostPresentation, store, {}, &host);
        start.setGeometry(host.contentsRect());
        host.show();
        start.show();
        for (bool next : { dark, !dark, dark }) {
            start.setDark(next);
            const auto paper = ui::themeColors(next).paper;
            QCOMPARE(start.library()->palette().color(QPalette::Window), paper);
            QCOMPARE(start.palette().color(QPalette::Window), paper);
            host.setPalette(start.palette());
            QCoreApplication::processEvents();
            QCOMPARE(host.grab().toImage().pixelColor(2, 2), paper);
            QCOMPARE(host.grab().toImage().pixelColor(2, host.height() - 2), paper);
        }
    }

    // Checks that window size, safe-area margins, the Library canvas and its system-bar rules follow portrait, landscape and cutout surfaces.
    void libraryCanvasFollowsViewport_data() { variants(); }

    void libraryCanvasFollowsViewport()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        library::Store store(directory.path(), std::make_shared<library::LocalSource>());
        QWidget window;
        window.setWindowFlag(Qt::ExpandedClientAreaHint);
        auto *layout = new QStackedLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *start = new standalone::StartScreen(test::makeHostPresentation, store);
        start->setDark(dark);
        start->setLibrarySystemBarBorders(true);
        layout->addWidget(start);
        // Reproduce a landscape Qt window inside a portrait Android surface.
        window.show();
        window.resize(1680, 1264);
        auto *canvas = start->libraryCanvas();
        auto *view = start->library();
        for (const auto pixels :
             { QSize(1264, 1680), QSize(1680, 1264), QSize(1264, 1680), QSize(600, 400) }) {
            const int side = pixels.width() > pixels.height() ? 84 : 0;
            const bool rightCutout = side && pixels.width() == 600;
            const QMargins safePixels(rightCutout ? 0 : side, 84, rightCutout ? side : 0, 78);
            standalone::fitWindowToSurface(window, pixels, safePixels);
            const auto ratio = window.devicePixelRatioF();
            QCOMPARE(window.size(),
                     QSize(qRound(pixels.width() / ratio), qRound(pixels.height() / ratio)));
            QCOMPARE(window.contentsMargins(),
                     QMargins(qCeil(side / ratio), qCeil(84 / ratio), qCeil(side / ratio),
                              qCeil(78 / ratio)));
            QVERIFY(!window.testAttribute(Qt::WA_ContentsMarginsRespectsSafeArea));
            QVERIFY(standalone::contentFitsSurface(window, pixels - QSize(2 * side, 84 + 78)));
            QVERIFY(!standalone::contentFitsSurface(window, pixels));
            QWidget readerWindow;
            standalone::fitWindowToSurface(readerWindow, pixels, QMargins(side, 0, side, 0));
            QCOMPARE(window.contentsRect().left(), readerWindow.contentsRect().left());
            QCOMPARE(window.contentsRect().right(), readerWindow.contentsRect().right());
            QCoreApplication::processEvents();
            QCOMPARE(start->geometry(), window.contentsRect());
            QTRY_COMPARE(view->size(), canvas->pixelSize());
            QVERIFY(canvas->viewport()->y() > 0);
            QVERIFY(canvas->viewport()->geometry().bottom() < canvas->height() - 1);
            const auto frame = canvas->grab().toImage();
            const int paper = qGray(view->palette().color(QPalette::Window).rgb());
            const int rule = qGray(view->palette().color(QPalette::Mid).rgb());
            const int bottomRows =
                    qCeil((canvas->height() - canvas->viewport()->geometry().bottom() - 1)
                          * frame.devicePixelRatio());
            // Qt antialiases the frame at fractional display scales. Every
            // pixel must still carry the rule across the whole header width.
            for (int x = 0; x < frame.width(); ++x) {
                QVERIFY(qAbs(qGray(frame.pixel(x, 0)) - paper) >= qAbs(rule - paper) / 2);
                // The final image row can cover only part of a logical pixel.
                // Measure the rule within the actual bottom frame strip.
                int contrast = 0;
                for (int y = frame.height() - bottomRows; y < frame.height(); ++y)
                    contrast = qMax(contrast, qAbs(qGray(frame.pixel(x, y)) - paper));
                QVERIFY(contrast >= qAbs(rule - paper) / 2);
            }
            QCOMPARE(canvas->sceneRect().size().toSize(), canvas->pixelSize());
            const auto *more = view->findChild<QWidget *>("libraryMore");
            QVERIFY(more);
            QVERIFY(view->rect().contains(more->geometry()));
            const auto visible = QRectF(QPointF(), canvas->viewport()->size());
            // Fractional display scales round the scene to a physical pixel.
            const auto pixel = 1.0 / ratio;
            QVERIFY(visible.adjusted(-pixel, -pixel, pixel, pixel)
                            .contains(
                                    canvas->viewportTransform().mapRect(QRectF(more->geometry()))));
            const auto *button = view->findChild<QPushButton *>("libraryChooseFolder");
            QVERIFY(button);
            const QRect area(button->mapTo(view, QPoint()), button->size());
            QVERIFY(area.left() >= 0 && area.right() < view->width());
            const auto *empty = view->findChild<QScrollArea *>("libraryEmpty");
            QVERIFY(empty);
            const auto center = empty->viewport()->mapTo(view, empty->viewport()->rect().center());
            QVERIFY(qAbs(area.center().x() - center.x()) <= 1);
        }
        // Insets change when entering/leaving a book even if surface size stays
        // the same. Cold launch must reserve them without a fullscreen toggle.
        const auto pixels = QSize(1264, 1680);
        standalone::fitWindowToSurface(window, pixels, {});
        QCOMPARE(window.contentsMargins(), QMargins());
        QCoreApplication::processEvents();
        QCOMPARE(start->geometry(), window.rect());
        standalone::fitWindowToSurface(window, pixels, QMargins(0, 84, 0, 78));
        QCoreApplication::processEvents();
        QCOMPARE(start->geometry(), window.contentsRect());
        QVERIFY(start->y() > 0);
        const auto previous = window.size();
        standalone::fitWindowToSurface(window, QSize(0, 1680));
        standalone::fitWindowToSurface(window, QSize(-1, -1));
        QCOMPARE(window.size(), previous);
        start->showOpening();
        QVERIFY(!canvas->isVisible());
        start->showLibrary();
        QVERIFY(canvas->isVisible());
        start->setLibrarySystemBarBorders(false);
        QTRY_COMPARE(canvas->viewport()->y(), 0);
        QCOMPARE(canvas->viewport()->height(), canvas->height());
    }

    // Checks BOOX app canvases: Library and opening frames, retries after rejected submissions, and pause and resume.
    void booxAppCanvases_data() { variants(); }

    void booxAppCanvases()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QTemporaryDir directory;
        library::Store store(directory.path(), std::make_shared<library::LocalSource>());
        bool available = false;
        bool targetAvailable = true;
        int rejected = 0;
        QVector<android::RefreshFrame> frames;
        standalone::StartScreen start(test::makeHostPresentation, store);
        start.setDark(dark);
        start.resize(600, 800);
        auto attach = [&](standalone::WidgetCanvas *canvas) {
            new android::CanvasRefresh(
                    canvas, [&] { return available; },
                    [&](const auto &frame) {
                        if (!targetAvailable) {
                            ++rejected;
                            return false;
                        }
                        frames.append(frame);
                        return available;
                    });
            // The windowless platform does not activate an Android activity.
            qApp->applicationStateChanged(Qt::ApplicationActive);
        };
        attach(start.libraryCanvas());
        start.show();
        QTest::qWait(30);
        QVERIFY(frames.isEmpty());
        available = true;
        start.libraryCanvas()->viewport()->repaint();
        QTRY_VERIFY(!frames.isEmpty());
        QVERIFY(frames.last().clean);
        QVERIFY(frames.last().textPixels.isEmpty());
        frames.clear();
        start.libraryCanvas()->viewport()->repaint(QRect(20, 20, 30, 30));
        QCOMPARE(frames.size(), 1);
        QVERIFY(!frames.first().clean);
        QVERIFY(!frames.first().text);
        // Loading progress uses the same Surface transport, including a second open.
        for (int cycle = 0; cycle < 2; ++cycle) {
            start.showOpening();
            auto *opening = start.openingCanvas();
            QVERIFY(opening);
            attach(opening);
            QCoreApplication::processEvents();
            frames.clear();
            opening->viewport()->repaint();
            QVERIFY(!frames.isEmpty());
            QVERIFY(!frames.last().text);
            QVERIFY(frames.last().textPixels.isEmpty());
            QCOMPARE(opening->content->size(), opening->pixelSize());
            auto *dialog = opening->content->findChild<QDialog *>("rrOpening");
            QVERIFY(dialog);
            QVERIFY(!dialog->isWindow());
            QVERIFY(!dialog->windowHandle());
            QCOMPARE(dialog->geometry(), opening->content->rect());
            for (const auto *name : { "rrOpeningBrand", "rrOpeningLabel", "rrOpeningProgress" }) {
                const auto *item = dialog->findChild<QWidget *>(name);
                QVERIFY(item);
                QVERIFY(dialog->rect().contains(item->geometry()));
            }
            frames.clear();
            start.setProgress(70);
            QTRY_VERIFY(!frames.isEmpty());
            for (const auto &frame : frames) {
                QVERIFY(!frame.text);
                QVERIFY(frame.textPixels.isEmpty());
            }
            frames.clear();
            start.showLibrary();
            QTRY_VERIFY(!frames.isEmpty());
            QVERIFY(frames.first().clean);
            QTest::qWait(80);
            QCOMPARE(std::count_if(frames.begin(), frames.end(),
                                   [](const auto &frame) { return frame.clean; }),
                     1);
        }
        // The Surface can disappear while Android restores the system bars.
        // Its return must retry the clean frame without requiring another tap.
        for (bool backgrounded : { false, true }) {
            start.hide();
            QCoreApplication::processEvents();
            frames.clear();
            rejected = 0;
            targetAvailable = false;
            start.show();
            QTRY_VERIFY(rejected > 0);
            QTest::qWait(100);
            QVERIFY(frames.isEmpty());
            if (backgrounded) {
                // Keep the canvas visible, as Android can do while suspended.
                qApp->applicationStateChanged(Qt::ApplicationInactive);
                const int beforePause = rejected;
                QTest::qWait(250);
                const bool stopped = rejected == beforePause;
                targetAvailable = true;
                QTest::qWait(250);
                const bool held = frames.isEmpty();
                qApp->applicationStateChanged(Qt::ApplicationActive);
                QVERIFY(stopped);
                QVERIFY(held);
            } else {
                targetAvailable = true;
            }
            QTRY_COMPARE_WITH_TIMEOUT(frames.size(), 1, 1000);
            QVERIFY(frames.first().clean);
            QTest::qWait(100);
            QCOMPARE(frames.size(), 1);
            start.libraryCanvas()->viewport()->repaint(QRect(20, 20, 30, 30));
            QCOMPARE(frames.size(), 2);
            QVERIFY(!frames.last().clean);
        }
        // A rejected text frame must keep its mode without assigning REGAL
        // to unrelated UI damage that arrives before the Surface recovers.
        frames.clear();
        rejected = 0;
        targetAvailable = false;
        auto *canvas = start.libraryCanvas();
        canvas->content->setProperty("boox_text_frame", true);
        canvas->viewport()->repaint(QRect(20, 20, 30, 30));
        QVERIFY(rejected > 0);
        canvas->viewport()->repaint(QRect(100, 100, 30, 30));
        QVERIFY(frames.isEmpty());
        targetAvailable = true;
        QTRY_COMPARE_WITH_TIMEOUT(frames.size(), 2, 1000);
        QVERIFY(frames.first().text);
        QVERIFY(!frames.first().clean);
        QCOMPARE(frames.first().textPixels, frames.first().screenPixels);
        QVERIFY(!frames.last().text);
        QVERIFY(!frames.last().clean);
        QVERIFY(frames.last().textPixels.isEmpty());
        QVERIFY((frames.first().screenPixels & frames.last().screenPixels).isEmpty());
        QTest::qWait(100);
        QCOMPARE(frames.size(), 2);
    }

    // Checks the reader-to-Library handoff behind a captured reader frame, with one clean reveal at the final size.
    void booxLibraryHandoff_data() { variants(); }

    void booxLibraryHandoff()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QTemporaryDir directory;
        library::Store store(directory.path(), std::make_shared<library::LocalSource>());
        QVector<android::RefreshFrame> frames;
        QWidget window;
        auto *layout = new QStackedLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *reader = new QWidget;
        reader->setAutoFillBackground(true);
        auto palette = reader->palette();
        palette.setColor(QPalette::Window, QColor(80, 90, 100));
        reader->setPalette(palette);
        auto *start = new standalone::StartScreen(test::makeHostPresentation, store);
        start->setDark(dark);
        layout->addWidget(reader);
        layout->addWidget(start);
        window.resize(600, 800);
        window.show();
        QCoreApplication::processEvents();
        new android::CanvasRefresh(
                start->libraryCanvas(), [] { return true; },
                [&](const auto &frame) {
                    frames.append(frame);
                    return true;
                });
        standalone::SurfaceHandoff handoff(window);
        const auto previous = window.grab().toImage();
        QVector<android::RefreshFrame> heldFrames;
        handoff.hold([&](standalone::WidgetCanvas *surface) {
            new android::CanvasRefresh(
                    surface, [] { return true; },
                    [&](const auto &frame) {
                        heldFrames.append(frame);
                        return true;
                    },
                    false);
        });
        QVERIFY(!heldFrames.isEmpty());
        for (const auto &frame : heldFrames)
            QVERIFY(!frame.clean);
        // Reader deletion can precede asynchronous resource cleanup.
        delete reader;
        start->showLibrary();
        QCoreApplication::processEvents();
        QVERIFY(handoff.active());
        QVERIFY(window.updatesEnabled());
        QVERIFY(frames.isEmpty());
        QCOMPARE(window.grab().toImage(), previous);
        QVERIFY(!handoff.present(false));
        // Android reports final content bounds before Qt applies its insets.
        const auto ratio = window.devicePixelRatioF();
        const QSize expected(qRound(window.width() * ratio),
                             qRound((window.height() - 85) * ratio));
        QVERIFY(!standalone::contentFitsSurface(window, expected));
        QVERIFY(!standalone::contentFitsSurface(window, {}));
        // Restore the bars and lay out the final safe area behind the snapshot.
        window.setContentsMargins(0, 44, 0, 41);
        QVERIFY(standalone::contentFitsSurface(window, expected));
        layout->activate();
        QCoreApplication::processEvents();
        QVERIFY(frames.isEmpty());
        QCOMPARE(window.grab().toImage().pixelColor(window.width() / 2, window.height() / 2),
                 previous.pixelColor(window.width() / 2, window.height() / 2));
        layout->setCurrentWidget(start);
        QVERIFY(handoff.present(true));
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 1000);
        QVERIFY(frames.first().clean);
        QCOMPARE(start->geometry(), window.contentsRect());
        QCOMPARE(start->library()->size(), start->libraryCanvas()->pixelSize());
        QTest::qWait(80);
        const int count = frames.size();
        QVERIFY(!handoff.present(true));
        QTest::qWait(80);
        QCOMPARE(frames.size(), count);
    }

    // Checks that onboarding heading ink clears the label edges, with scrolling only on short screens.
    void libraryOnboardingLayout_data() { variants(); }

    void libraryOnboardingLayout()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        library::Index index;
        library::LibraryView view(index, {});
        view.setDark(dark);
        view.show();
        auto *empty = view.findChild<QScrollArea *>("libraryEmpty");
        QVERIFY(empty);
        for (const auto size : { QSize(1072, 1448), QSize(1448, 1072), QSize(600, 400) }) {
            view.resize(ui::px(size.width()), ui::px(size.height()));
            QCoreApplication::processEvents();
            if (size.height() >= 1072)
                QTRY_COMPARE(empty->verticalScrollBar()->maximum(), 0);
            else
                QTRY_VERIFY(empty->verticalScrollBar()->maximum() > 0);
            QCOMPARE(empty->horizontalScrollBar()->maximum(), 0);
            auto *heading = view.findChild<QLabel *>("libraryEmptyTitle");
            QVERIFY(heading);
            const auto headingImage =
                    heading->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const QColor paper = dark ? Qt::black : Qt::white;
            // Italic ink can extend past its advance and line metrics. Keep
            // a clear edge around the label so its glyphs cannot be clipped.
            const int guard = qMax(1, ui::px(2));
            for (int y = 0; y < headingImage.height(); ++y)
                for (int x = 0; x < headingImage.width(); ++x)
                    if (x < guard || x >= headingImage.width() - guard || y < guard
                        || y >= headingImage.height() - guard)
                        QCOMPARE(headingImage.pixelColor(x, y), paper);
            for (const auto *name : { "libraryOpenBook", "libraryChooseFolder" }) {
                auto *button = view.findChild<QPushButton *>(name);
                QVERIFY(button && button->isVisible());
                QVERIFY(button->width()
                        >= QFontMetrics(button->font()).horizontalAdvance(button->text())
                                + ui::px(24));
                QVERIFY(button->parentWidget()->rect().contains(button->geometry()));
            }
            const auto directory = qEnvironmentVariable("KOBALT_CAPTURE_MODALS");
            if (!directory.isEmpty()) {
                QVERIFY(QDir().mkpath(directory));
                QVERIFY(view.grab().save(directory
                                         + QString("/library-onboarding-%1-%2-%3x%4.png")
                                                   .arg(dark ? "dark" : "light")
                                                   .arg(ppi)
                                                   .arg(size.width())
                                                   .arg(size.height())));
            }
        }
    }

    // Checks that the scan caption never overlaps the six-square indicator or its trailing gap.
    void libraryScanCaption_data() { variants(); }

    void libraryScanCaption()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        library::Index index;
        library::LibraryView view(index, {});
        view.resize(ui::px(1072), ui::px(1448));
        view.setDark(dark);
        view.show();
        view.setScanning(true, 4);
        QCoreApplication::processEvents();
        auto *progress = dynamic_cast<ui::LoadingIndicator *>(
                view.findChild<QWidget *>("libraryScanProgress"));
        auto *sort = view.findChild<QPushButton *>("librarySort");
        QVERIFY(progress && sort);
        QTRY_VERIFY(progress->isVisible() && sort->isVisible());
        const auto image = sort->grab().toImage();
        const QColor paper = dark ? Qt::black : Qt::white;
        // Render the caption without its sibling indicator. The indicator and
        // the padding after it must contain no caption pixels in any frame.
        const int right = progress->geometry().right() + ui::px(24);
        int inkAfter = 0;
        for (int y = ui::px(8); y < image.height() - ui::px(8); ++y)
            for (int x = 0; x < image.width(); ++x) {
                if (x <= right)
                    QCOMPARE(image.pixelColor(x, y), paper);
                else
                    inkAfter += image.pixelColor(x, y) != paper;
            }
        QVERIFY(inkAfter > 20);
        view.setScanning(false, 99);
        QVERIFY(!progress->isVisible());
        const auto directory = qEnvironmentVariable("KOBALT_CAPTURE_MODALS");
        if (!directory.isEmpty()) {
            view.setScanning(true, 99);
            QTRY_VERIFY(progress->isVisible());
            QTest::qWait(50);
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(view.grab().save(
                    directory
                    + QString("/library-scan-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        }
    }

    void connectedModalFooter_data() { variants(); }

    void connectedModalFooter()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        auto colors = host.palette();
        const QColor paper = dark ? Qt::black : Qt::white;
        const QColor ink = dark ? Qt::white : Qt::black;
        colors.setColor(QPalette::Window, paper);
        colors.setColor(QPalette::WindowText, ink);
        host.setPalette(colors);
        ui::readerStyle(&host, ui::styles::reader());
        host.resize(ui::px(1072), ui::px(1448));
        host.show();
        ui::ModalOverlay reference(&host);
        reference.setDimmed(true);
        reference.show();
        QCoreApplication::processEvents();
        const auto backdrop = reference.grab().toImage();
        reference.hide();
        ui::ChoiceModal choice(&host,
                               { "The saved passage will be removed from this book.",
                                 "Delete this highlight?",
                                 {} },
                               { "Cancel", "modalCancel" }, { "Delete", "modalDelete" });
        choice.show();
        QCoreApplication::processEvents();
        auto *card = choice.card();
        auto *cancel = choice.secondaryButton();
        auto *remove = choice.primaryButton();
        auto *heading = card->findChild<QLabel *>("rrModalTitle");
        auto *header = heading->parentWidget();
        QCOMPARE(header, choice.descriptionLabel()->parentWidget());
        QCOMPARE(header->layout()->contentsMargins().top(),
                 int(ui::metrics::notice::cardMargins.top()));
        QCOMPARE(header->layout()->contentsMargins().left(), int(ui::metrics::chrome::inset));
        QCOMPARE(choice.descriptionLabel()->y() - heading->geometry().bottom() - 1,
                 int(ui::metrics::notice::headingGap));
        QCOMPARE(cancel->y(), remove->y());
        QCOMPARE(cancel->geometry().right() + 1, remove->x());
        QCOMPARE(cancel->x(), 2);
        QCOMPARE(remove->geometry().right(), card->width() - 3);
        QCOMPARE(remove->geometry().bottom(), card->height() - 3);
        const auto frame = card->grab().toImage();
        QCOMPARE(frame.pixelColor(remove->geometry().center().x(), remove->y()),
                 ui::dividerColor(card));
        QCOMPARE(frame.pixelColor(remove->x(), remove->y() + ui::px(10)), ui::dividerColor(card));
        QCOMPARE(frame.pixelColor(remove->x() + ui::px(10), remove->y() + ui::px(10)), paper);
        const auto actual = choice.grab().toImage();
        int stripes = 0;
        for (int y = 0; y < ui::px(100); ++y) {
            QCOMPARE(actual.pixelColor(0, y), backdrop.pixelColor(0, y));
            stripes += actual.pixelColor(0, y) != paper;
        }
        QVERIFY(stripes > 0);
        const auto captures = qEnvironmentVariable("KOBALT_CAPTURE_MODALS");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(choice.grab().save(
                    captures
                    + QString("/connected-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        }
        remove->setText("Use this reading font as the default for every new book");
        card->fitToWidth(ui::px(400));
        choice.centerCard();
        QCoreApplication::processEvents();
        QVERIFY(remove->y() < cancel->y());
        QCOMPARE(remove->geometry().bottom() + 1, cancel->y());
        QCOMPARE(remove->x(), cancel->x());
        QCOMPARE(remove->width(), cancel->width());
        const auto labelBounds =
                QFontMetrics(remove->font())
                        .boundingRect(QRect(0, 0,
                                            remove->width()
                                                    - 2
                                                            * int(ui::metrics::control::
                                                                          outlinedHorizontalPadding
                                                                  + ui::metrics::control::border),
                                            10000),
                                      Qt::TextWordWrap, remove->text());
        QVERIFY(remove->height() >= labelBounds.height()
                        + 2
                                * int(ui::metrics::control::outlinedVerticalPadding
                                      + ui::metrics::control::border));
        if (!captures.isEmpty())
            QVERIFY(choice.grab().save(
                    captures
                    + QString("/stacked-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        remove->hide();
        card->fitToWidth(ui::px(760));
        QCoreApplication::processEvents();
        QCOMPARE(cancel->width(), card->width() - 4);
        remove->show();
        remove->setText("Delete");
        card->fitToWidth(ui::px(760));
        QCoreApplication::processEvents();
        QCOMPARE(cancel->y(), remove->y());
        choice.hide();
        ui::NoticeModal notice(&host, { {}, "Formatting warning", {} }, { "OK", "modalOk" });
        notice.show();
        QCoreApplication::processEvents();
        QCOMPARE(notice.primaryButton()->width(), notice.card()->width() - 4);
        QCOMPARE(notice.primaryButton()->geometry().bottom(), notice.card()->height() - 3);
        QCOMPARE(notice.grab().toImage().pixelColor(0, 0), backdrop.pixelColor(0, 0));
    }

    // Checks that the continuation-hint area repaints blank after paging to the last page, a one-page list, a smaller gap or a page reset.
    void continuationHintClears_data() { variants(); }

    void continuationHintClears()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        ui::Panel owner;
        auto colors = owner.palette();
        const QColor paper = dark ? Qt::black : Qt::white;
        colors.setColor(QPalette::Window, paper);
        colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        owner.setPalette(colors);
        owner.resize(ui::px(1072), ui::px(800));
        ui::PagerBar pager(&owner, "hint");
        pager.setGeometry(0, owner.height() - pager.height(), owner.width(), pager.height());
        const QRect gap(ui::px(40), ui::px(400), owner.width() - ui::px(80), ui::px(200));
        auto *hint = owner.findChild<QLabel *>("hintContinuationHint");
        QVERIFY(hint);
        PaintRegions paints;
        owner.installEventFilter(&paints);
        owner.show();
        for (int change = 0; change < 4; ++change) {
            pager.setPage(0, 2);
            pager.setBlankArea(gap);
            QTRY_VERIFY(hint->isVisible());
            QCoreApplication::processEvents();
            const auto visible = owner.grab().toImage().copy(gap);
            QImage blank(visible.size(), visible.format());
            blank.fill(paper);
            QVERIFY(visible != blank);
            paints.painted = {};
            if (change == 0)
                pager.setPage(1, 2);
            else if (change == 1)
                pager.setPage(0, 1);
            else if (change == 2)
                pager.setBlankArea(QRect(gap.topLeft(), QSize(gap.width(), 1)));
            else
                pager.setPage(0, 2);
            QVERIFY(hint->isHidden());
            // Check the actual damage before grab() can force a fresh render.
            QTRY_VERIFY(paints.painted.contains(gap));
            QCOMPARE(owner.grab().toImage().copy(gap), blank);
        }
    }

    // Checks that rows, trailing row actions and menu actions press gray, while a selected row and a pressed toolbar cell fill with ink.
    void listPressFeedback_data() { variants(); }

    void listPressFeedback()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        host.setPalette(ui::readerPalette(host.palette(), dark));
        host.resize(ui::px(1072), ui::px(1448));
        ui::ListRow row(&host);
        row.resize(ui::px(700), ui::px(160));
        auto *more = row.addAction("more", "More");
        row.setActionIcon(ui::ControlIcon::More);
        ui::ActionMenu menu(
                &host, {},
                { ui::actionItem("Modify bookmark", "modify", ui::ControlIcon::Pin, {}) });
        menu.setPalette(host.palette());
        menu.card()->setPalette(host.palette());
        menu.fitCard();
        const auto background = [](QWidget *widget) {
            return widget->grab().toImage().pixelColor(ui::px(8), ui::px(8));
        };
        const auto gray = ui::themeColors(dark).pressedPaper;
        row.setDown(true);
        QCOMPARE(background(&row), gray);
        more->setDown(true);
        QCOMPARE(background(more), gray);
        auto *action = menu.action("modify");
        action->setDown(true);
        QCOMPARE(background(action), gray);
        action->setDown(false);
        QCOMPARE(background(action), ui::themeColors(dark).paper);
        row.setCheckable(true);
        row.setChecked(true);
        QCOMPARE(background(&row), ui::themeColors(dark).ink);
        ui::CellButton toolbar(ui::ControlIcon::Search, &host);
        toolbar.resize(ui::px(112), ui::px(112));
        toolbar.setDown(true);
        QCOMPARE(background(&toolbar), ui::themeColors(dark).ink);
    }

    // Checks the gray press on checkbox rows and color swatches, its cancellation and the unchanged disabled controls.
    void optionAndSwatchPressFeedback_data() { variants(); }

    void optionAndSwatchPressFeedback()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        host.setPalette(ui::readerPalette(host.palette(), dark));
        host.resize(ui::px(800), ui::px(500));
        ui::DescribedOption option("Check at start", "Scan configured folders", &host);
        auto look = option.presentation();
        look.indicator = ui::DescribedOption::Indicator::Square;
        option.setPresentation(look);
        option.resize(ui::px(700), ui::px(160));
        ui::HighlightSwatch swatch("yellow", "Yellow", &host);
        swatch.move(0, ui::px(200));
        swatch.resize(ui::px(160), swatch.height());
        host.show();
        for (auto *control :
             { static_cast<QAbstractButton *>(&option), static_cast<QAbstractButton *>(&swatch) }) {
            for (bool checked : { false, true }) {
                control->setChecked(checked);
                const auto normal = control->grab().toImage();
                QTest::mousePress(control, Qt::LeftButton);
                QVERIFY(control->isDown());
                QCOMPARE(control->grab().toImage().pixelColor(ui::px(4), ui::px(4)),
                         ui::themeColors(dark).pressedPaper);
                QCOMPARE(control->isChecked(), checked);
                QTest::mouseRelease(control, Qt::LeftButton, Qt::NoModifier, QPoint(-1, -1));
                QVERIFY(!control->isDown());
                QCOMPARE(control->grab().toImage(), normal);
            }
            control->setEnabled(false);
            control->setDown(true);
            QCOMPARE(control->grab().toImage().pixelColor(ui::px(4), ui::px(4)),
                     ui::themeColors(dark).paper);
        }
    }

    void marksSortingAndAnnotationRows_data() { variants(); }

    void marksSortingAndAnnotationRows()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget owner;
        owner.setProperty("reader_dark", dark);
        auto colors = owner.palette();
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        colors.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
        owner.setPalette(colors);
        ui::readerStyle(&owner, ui::styles::reader());
        owner.resize(ui::px(1072), ui::px(1448));
        BookmarkList list(&owner);
        list.setExcerptFont(QStringLiteral("Libron"));
        list.setGeometry(owner.rect());
        const auto early = QDateTime::fromString("2026-10-20T09:12:00Z", Qt::ISODate);
        const auto late = early.addDays(1);
        BookmarkRow a{
            "first", "Chapter One",
            QStringLiteral("A lengthy quotation with enough words to wrap across several lines. ")
                    .repeated(5),
            late, 2
        };
        a.sourceOrder = 0;
        a.style = "dotted";
        a.color = "blue";
        const auto sampleFont = ui::interfaceFont(ui::px(32));
        QImage wrapped(300, 8 * ui::px(44), QImage::Format_RGB32);
        wrapped.fill(Qt::white);
        {
            QPainter painter(&wrapped);
            painter.setFont(sampleFont);
            painter.setPen(Qt::black);
            QVector<QRect> lines;
            ui::drawWrappedInterfaceText(painter, wrapped.rect(), a.excerpt,
                                         Qt::AlignLeft | Qt::AlignTop, 2, ui::px(44), &lines);
            QCOMPARE(lines.size(), 2);
            QCOMPARE(lines[1].top() - lines[0].top(), int(ui::px(44)));
        }
        for (int y = 2 * ui::px(44) + 2; y < wrapped.height(); ++y)
            for (int x = 0; x < wrapped.width(); ++x)
                QCOMPARE(wrapped.pixelColor(x, y), QColor(Qt::white));
        BookmarkRow b{ "second", "Chapter One", "Second passage", early, 2 };
        b.sourceOrder = 1;
        b.style = "underline";
        b.color = "green";
        BookmarkRow c{ "third", "Chapter Two", "Later in the book", late.addSecs(60), 8 };
        auto *summary = list.findChild<QLabel *>("rrBookmarkListSummary");
        QVERIFY(summary);
        list.showAnnotations({});
        QCOMPARE(summary->text(), QString("0 annotations"));
        list.showAnnotations({ a });
        QCOMPARE(summary->text(), QString("1 annotation"));
        list.showAnnotations({ a, b });
        QCOMPARE(summary->text(), QString("2 annotations"));
        QString selected, changed;
        list.selected = [&](const QString &id) { selected = id; };
        list.sortRequested = [&](const QString &value) { changed = value; };
        owner.show();
        list.showAnnotations({ c, b, a });
        QCoreApplication::processEvents();
        const auto row = [&]() {
            return static_cast<ui::ListRow *>(list.findChild<QWidget *>("rrAnnotation0"));
        };
        QVERIFY(row());
        row()->click();
        QCOMPARE(selected, QString("first"));
        QCOMPARE(row()->detailLabel()->text(), a.excerpt);
        QVERIFY(row()->titleLabel()->font().pixelSize() < row()->detailLabel()->font().pixelSize());
        QVERIFY(row()->detailLabel()->y() - row()->titleLabel()->geometry().bottom() >= ui::px(12));
        QVERIFY(row()->detailLabel()->wordWrap());
        QVERIFY(row()->detailLabel()->height()
                > 2 * QFontMetrics(row()->detailLabel()->font()).height());
        QVERIFY(!row()->captionLabel()->text().contains("Dotted underline"));
        QVERIFY(row()->captionLabel()->text().contains(
                QLocale().toString(late.toLocalTime(), "HH:mm")));
        const int annotationHeight = row()->height();
        const auto annotationImage = row()->grab().toImage();
        auto pinned = a;
        pinned.pinName = "Reference";
        pinned.pinIcon = "map";
        list.showBookmarks({ pinned });
        list.showAnnotations({ c, b, a });
        QCoreApplication::processEvents();
        QCOMPARE(row()->grab().toImage(), annotationImage);
        QCOMPARE(list.findChild<QPushButton *>("rrMarksSort")->y(),
                 list.findChild<QPushButton *>("rrAnnotationsTab")->y());
        auto *sortControl = list.findChild<QPushButton *>("rrMarksSort");
        const auto sortImage = sortControl->grab().toImage();
        const auto iconArea = QRect(sortImage.rect().center() - QPoint(ui::px(20), ui::px(20)),
                                    QSize(ui::px(40), ui::px(40)));
        int iconPixels = 0;
        const auto ink = colors.color(QPalette::WindowText);
        for (int y = iconArea.top(); y <= iconArea.bottom(); ++y)
            for (int x = iconArea.left(); x <= iconArea.right(); ++x)
                iconPixels += sortImage.pixelColor(x, y) == ink;
        QVERIFY(iconPixels > 10);
        QVERIFY(list.findChild<QPushButton *>("rrMarksSort")->x()
                >= list.findChild<QPushButton *>("rrBookmarksTab")->geometry().right());
        QVector<QImage> appearances;
        for (const auto *style : { "shaded", "underline", "dotted" }) {
            auto item = a;
            item.style = style;
            list.showAnnotations({ item });
            QCoreApplication::processEvents();
            appearances.append(row()->detailLabel()->grab().toImage());
        }
        QVERIFY(appearances[0] != appearances[1]);
        QVERIFY(appearances[1] != appearances[2]);
        QVERIFY(appearances[0] != appearances[2]);
        list.showAnnotations({ c, b, a });
        auto *shortRow = list.findChild<QWidget *>("rrAnnotation1");
        QVERIFY(shortRow && shortRow->height() < annotationHeight);
        const auto shortSample =
                static_cast<ui::ListRow *>(shortRow)->detailLabel()->grab().toImage();
        bool hasGreenUnderline = false;
        for (int y = 0; y < shortSample.height() && !hasGreenUnderline; ++y)
            for (int x = 0; x < shortSample.width(); ++x)
                hasGreenUnderline =
                        hasGreenUnderline || shortSample.pixelColor(x, y) == QColor("#b8d2aa");
        QVERIFY(hasGreenUnderline);
        const auto captureRoot = qEnvironmentVariable("KOBALT_CAPTURE_MARKS");
        if (!captureRoot.isEmpty()) {
            QVERIFY(QDir().mkpath(captureRoot));
            QVERIFY(owner.grab().save(
                    captureRoot
                    + QString("/annotations-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        }
        list.findChild<QPushButton *>("rrMarksSort")->click();
        QCoreApplication::processEvents();
        if (!captureRoot.isEmpty())
            QVERIFY(owner.grab().save(
                    captureRoot
                    + QString("/sort-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        QCOMPARE(list.findChild<QWidget *>("rrModalCard")->palette().color(QPalette::Window),
                 colors.color(QPalette::Window));
        auto *newestOption = list.findChild<QAbstractButton *>("rrMarksSort_newest");
        QCOMPARE(newestOption->palette().color(QPalette::Window), colors.color(QPalette::Window));
        QCOMPARE(newestOption->palette().color(QPalette::WindowText),
                 colors.color(QPalette::WindowText));
        QVERIFY(!newestOption->accessibleDescription().isEmpty());
        list.findChild<QAbstractButton *>("rrMarksSort_newest")->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(changed, QString("newest"));
        row()->click();
        QCOMPARE(selected, QString("third"));
        list.findChild<QPushButton *>("rrMarksSort")->click();
        QCoreApplication::processEvents();
        list.findChild<QAbstractButton *>("rrMarksSort_oldest")->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        row()->click();
        QCOMPARE(selected, QString("second"));
        list.showAnnotations({ a, c, b });
        QCOMPARE(list.sortOrder(), QString("oldest"));
        row()->click();
        QCOMPARE(selected, QString("second"));
        list.setSortOrder("position");
        list.showBookmarks({ c, b, a });
        auto *bookmark = static_cast<ui::ListRow *>(list.findChild<QWidget *>("rrBookmark0"));
        QVERIFY(bookmark);
        bookmark->click();
        QCOMPARE(selected, QString("first"));
        QVERIFY(bookmark->detailLabel()->wordWrap());
        QCOMPARE(bookmark->height(), annotationHeight - (int(ui::px(12)) - int(ui::px(4))));
        QCOMPARE(bookmark->titleLabel()->font().pixelSize(), int(ui::px(24)));
        QVERIFY(bookmark->detailLabel()->y() - bookmark->titleLabel()->geometry().bottom()
                >= ui::px(4));
        if (!captureRoot.isEmpty())
            QVERIFY(owner.grab().save(
                    captureRoot
                    + QString("/bookmarks-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        QVERIFY(bookmark->captionLabel()->text().contains(
                QLocale().toString(late.toLocalTime(), "HH:mm")));
        QVector<BookmarkRow> many;
        for (int i = 0; i < 24; ++i) {
            auto item = i % 2 ? a : b;
            item.position = QString::number(i);
            item.page = i;
            many.append(item);
        }
        list.showAnnotations(many);
        QSet<QString> visited;
        for (int turn = 0; turn < 24 && visited.size() < many.size(); ++turn) {
            for (auto *button : list.findChildren<QPushButton *>()) {
                const auto name = button->objectName();
                bool numberedRow = false;
                const int index =
                        name.mid(QStringLiteral("rrAnnotation").size()).toInt(&numberedRow);
                if (!button->isVisible() || !name.startsWith("rrAnnotation") || !numberedRow
                    || index < 0)
                    continue;
                button->click();
                QVERIFY(!visited.contains(selected));
                visited.insert(selected);
            }
            if (visited.size() < many.size())
                list.findChild<QPushButton *>("rrBookmarkListNext")->click();
        }
        QCOMPARE(visited.size(), many.size());
        // Sorting moves this short middle row without changing its contents
        // or height, because the preceding long row becomes a short row.
        a.page = 0;
        b.page = 1;
        c.page = 2;
        auto d = a;
        d.position = "fourth";
        d.page = 3;
        a.created = early.addSecs(20);
        b.created = early.addSecs(30);
        c.created = early.addSecs(40);
        d.created = early.addSecs(10);
        QString removed;
        list.removeRequested = [&](const QString &id) { removed = id; };
        for (bool annotations : { true, false }) {
            list.setSortOrder("position");
            if (annotations)
                list.showAnnotations({ a, b, c, d });
            else
                list.showBookmarks({ a, b, c, d });
            QCoreApplication::processEvents();
            const QString prefix = annotations ? "rrAnnotation" : "rrBookmark";
            auto *middle = list.findChild<QWidget *>(prefix + "1");
            const QString actionPrefix = prefix + (annotations ? "Remove" : "More");
            auto *removeMiddle = list.findChild<QPushButton *>(actionPrefix + "1");
            QVERIFY(middle && removeMiddle);
            const int middleY = middle->y(), middleHeight = middle->height();
            for (const auto &order : { QStringLiteral("newest"), QStringLiteral("oldest"),
                                       QStringLiteral("position") }) {
                list.setSortOrder(order);
                QCoreApplication::processEvents();
                for (int index = 0; index < 4; ++index) {
                    auto *item = list.findChild<QWidget *>(prefix + QString::number(index));
                    auto *remove = list.findChild<QWidget *>(actionPrefix + QString::number(index));
                    QVERIFY(item && remove && item->isVisible() && remove->isVisible());
                    QCOMPARE(remove->geometry(),
                             QRect(item->geometry().right() + 1 - ui::metrics::chrome::cellWidth,
                                   item->y(), ui::metrics::chrome::cellWidth, item->height()));
                }
                if (order == "newest") {
                    QVERIFY(middle->y() < middleY);
                    QCOMPARE(middle->height(), middleHeight);
                    removed.clear();
                    removeMiddle->click();
                    if (!annotations) {
                        auto *remove = owner.findChild<QPushButton *>("rrBookmarkDelete");
                        QVERIFY(remove && remove->isVisible());
                        remove->click();
                    }
                    QCOMPARE(removed, QString("second"));
                }
            }
        }
        // A list that has already laid out these excerpts must paginate exactly
        // like a fresh list after each change to text or wrapping metrics.
        auto geometry = [&](BookmarkList &panel, const QVector<BookmarkRow> &entries) {
            QStringList result;
            QString activated;
            QSet<QString> seen;
            panel.selected = [&](const QString &id) { activated = id; };
            panel.hide();
            panel.showAnnotations(entries);
            for (int turn = 0; turn < entries.size() && seen.size() < entries.size(); ++turn) {
                for (int index = 0; index < entries.size(); ++index) {
                    auto *button = static_cast<ui::ListRow *>(
                            panel.findChild<QWidget *>("rrAnnotation" + QString::number(index)));
                    if (!button || !button->isVisible())
                        continue;
                    button->click();
                    if (seen.contains(activated))
                        break;
                    seen.insert(activated);
                    const auto bounds = button->geometry();
                    result.append(QString("%1:%2:%3,%4,%5,%6:%7")
                                          .arg(turn)
                                          .arg(activated)
                                          .arg(bounds.x())
                                          .arg(bounds.y())
                                          .arg(bounds.width())
                                          .arg(bounds.height())
                                          .arg(button->detailLabel()->height()));
                }
                if (seen.size() < entries.size())
                    panel.findChild<QPushButton *>("rrBookmarkListNext")->click();
            }
            panel.selected = {};
            return result;
        };
        auto cachedRows = many;
        QString family = "Libron", ordering = "position";
        for (int change = 0; change < 7; ++change) {
            if (change == 1)
                ordering = "newest";
            if (change == 2)
                cachedRows[0].excerpt = a.excerpt + "Changed long excerpt";
            if (change == 3)
                cachedRows[1].excerpt = "Changed short excerpt";
            if (change == 4)
                list.resize(ui::px(420), owner.height());
            if (change == 5)
                family = ui::interfaceFont().family();
            if (change == 6)
                cachedRows.remove(0, 7);
            list.setExcerptFont(family);
            list.setSortOrder(ordering);
            const auto warm = geometry(list, cachedRows);
            QCOMPARE(warm.size(), cachedRows.size());
            BookmarkList fresh(&owner);
            fresh.setGeometry(list.geometry());
            fresh.setExcerptFont(family);
            fresh.setSortOrder(ordering);
            const auto cold = geometry(fresh, cachedRows);
            QCOMPARE(warm, cold);
        }
    }

    void annotationDetailsOnlyLoadVisiblePage_data() { variants(); }

    void annotationDetailsOnlyLoadVisiblePage()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget owner;
        owner.setProperty("reader_dark", dark);
        ui::readerStyle(&owner, ui::styles::reader());
        owner.resize(ui::px(1072), ui::px(1448));
        BookmarkList list(&owner);
        list.setGeometry(owner.rect());
        QVector<BookmarkRow> compact;
        const auto date = QDateTime::fromString("2026-10-20T09:12:00Z", Qt::ISODate);
        for (int i = 0; i < 2000; ++i) {
            BookmarkRow row;
            row.position = QString::number(i);
            row.page = row.sourceOrder = i;
            row.created = date.addSecs(i);
            row.style = "dotted";
            row.color = "blue";
            compact.append(row);
        }
        QVector<QStringList> requests;
        bool fail = false, wrongId = false;
        const auto excerpt = [](const QString &id) {
            return id
                    + QStringLiteral(" A long quotation with enough words to fill two lines. ")
                              .repeated(6);
        };
        list.annotationDetails = [&](const QStringList &ids, QVector<BookmarkRow> *details) {
            requests.append(ids);
            for (const auto &id : ids) {
                BookmarkRow row;
                row.position = wrongId ? "wrong-id" : id;
                row.chapter = "Chapter " + id;
                row.excerpt = excerpt(id);
                details->append(row);
            }
            return !fail;
        };
        QString selected, removed;
        list.selected = [&](const QString &id) { selected = id; };
        list.removeRequested = [&](const QString &id) { removed = id; };
        list.showAnnotations(compact);
        QVERIFY(requests.isEmpty()); // The parent has not been shown.
        owner.show();
        QCoreApplication::processEvents();
        QVERIFY(!requests.isEmpty());
        auto displayed = [&]() {
            QStringList ids;
            for (auto *button : list.findChildren<QPushButton *>()) {
                const auto name = button->objectName();
                bool numberedRow = false;
                const int index =
                        name.mid(QStringLiteral("rrAnnotation").size()).toInt(&numberedRow);
                if (!button->isVisible() || !name.startsWith("rrAnnotation") || !numberedRow
                    || index < 0)
                    continue;
                button->click();
                ids.append(selected);
            }
            return ids;
        };
        const auto first = requests.last();
        QVERIFY(first.size() > 0 && first.size() < 20);
        QCOMPARE(displayed(), first);
        for (int i = 0; i < first.size(); ++i)
            QCOMPARE(first[i], QString::number(i));
        auto *firstRow = static_cast<ui::ListRow *>(list.findChild<QWidget *>("rrAnnotation0"));
        QVERIFY(firstRow);
        QCOMPARE(firstRow->detailLabel()->text(), excerpt("0"));
        // Hydration must preserve the compact row's appearance, not the default
        // style and color in the returned detail record.
        BookmarkList full(&owner);
        full.setGeometry(list.geometry());
        auto complete = compact.first();
        complete.chapter = "Chapter 0";
        complete.excerpt = excerpt("0");
        full.showAnnotations({ complete });
        QCoreApplication::processEvents();
        auto *fullRow = static_cast<ui::ListRow *>(full.findChild<QWidget *>("rrAnnotation0"));
        QVERIFY(fullRow);
        QCOMPARE(firstRow->detailLabel()->grab().toImage(),
                 fullRow->detailLabel()->grab().toImage());
        full.hide();
        requests.clear();
        list.findChild<QPushButton *>("rrBookmarkListNext")->click();
        QCOMPARE(requests.size(), 1);
        QCOMPARE(displayed(), requests.last());
        QVERIFY(!requests.last().contains(first.first()));
        for (auto *label : list.findChildren<QLabel *>())
            QVERIFY(label->text() != excerpt("0"));
        requests.clear();
        list.setSortOrder("newest");
        QCOMPARE(requests.size(), 1);
        QCOMPARE(displayed(), requests.last());
        for (int i = 0; i < requests.last().size(); ++i)
            QCOMPARE(requests.last()[i], QString::number(1999 - i));
        requests.clear();
        list.setExcerptFont("Libron");
        QVERIFY(requests.isEmpty());
        list.resize(ui::px(650), owner.height());
        QVERIFY(requests.isEmpty());
        list.resize(ui::px(650), ui::px(900));
        QCOMPARE(requests.size(), 1);
        QCOMPARE(displayed(), requests.last());
        list.hide();
        requests.clear();
        list.setSortOrder("oldest");
        list.setExcerptFont(ui::interfaceFont().family());
        list.resize(owner.size());
        QVERIFY(requests.isEmpty());
        for (auto *label : list.findChildren<QLabel *>())
            QVERIFY(!label->text().contains("A long quotation"));
        list.showAnnotations(compact, true);
        QVERIFY(requests.isEmpty());
        list.showAnnotations(compact);
        QCOMPARE(requests.size(), 1);
        for (int failure = 0; failure < 2; ++failure) {
            fail = failure == 0;
            wrongId = failure == 1;
            selected.clear();
            removed.clear();
            requests.clear();
            list.showAnnotations(compact);
            QCOMPARE(requests.size(), 1);
            auto *row = static_cast<ui::ListRow *>(list.findChild<QWidget *>("rrAnnotation0"));
            auto *remove = list.findChild<QPushButton *>("rrAnnotationRemove0");
            QVERIFY(row && remove && !row->isEnabled() && !remove->isEnabled());
            QCOMPARE(
                    row->detailLabel()->text(),
                    QStringLiteral(
                            "Saved annotations could not be read. Editing is disabled. Reopen this book to try again."));
            row->click();
            remove->click();
            QVERIFY(selected.isEmpty() && removed.isEmpty());
        }
        fail = wrongId = false;
        list.showAnnotations(compact);
        QVERIFY(firstRow->isEnabled());
        firstRow->click();
        QCOMPARE(selected, QString("0"));
        requests.clear();
        list.showBookmarks({ complete });
        QVERIFY(requests.isEmpty());
    }

    void navigationPressKeepsDamageInItsTab_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("panelOpen");
        QTest::addColumn<bool>("selected");
        for (bool dark : { false, true })
            for (bool open : { false, true })
                for (bool selected : { false, true })
                    QTest::newRow(qPrintable(QString("dark-%1-panel-%2-selected-%3")
                                                     .arg(dark)
                                                     .arg(open)
                                                     .arg(selected)))
                            << dark << open << selected;
    }

    // Checks that pressing or cancelling a tab leaves unrelated toolbar pixels unpainted.
    void navigationPressKeepsDamageInItsTab()
    {
        QFETCH(bool, dark);
        QFETCH(bool, panelOpen);
        QFETCH(bool, selected);
        QWidget owner;
        owner.resize(1072, 1448);
        owner.setProperty("reader_dark", dark);
        owner.setPalette(ui::readerPalette(owner.palette(), dark));
        reader::BottomNavigation bar(&owner);
        bar.setReturnLabel({}, {});
        bar.setGeometry(0, owner.height() - bar.barHeight(), owner.width(), bar.barHeight());
        bar.setPanelOpen(panelOpen);
        bar.select(-1, true, true);
        bar.show();
        owner.show();
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
        PaintRegions paints;
        bar.installEventFilter(&paints);
        for (const char *name : { "rrNavChapters", "rrNavSeek", "rrNavText", "rrNavLayout",
                                  "rrNavLight", "rrNavStats" }) {
            auto *tab = bar.findChild<QPushButton *>(name);
            QVERIFY(tab && tab->isVisible());
            tab->setChecked(selected);
            tab->setFocus();
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
            const auto before = bar.grab().toImage();
            const QRect column(tab->mapTo(&bar, QPoint()).x(), 0, tab->width(), bar.height());
            paints.painted = {};
            QTest::mousePress(tab, Qt::LeftButton);
            QCoreApplication::processEvents();
            QVERIFY(tab->isDown());
            QVERIFY2(paints.painted.subtracted(column).isEmpty(),
                     "Pressing one tab repaints the rest of the navigation bar");
            paints.painted = {};
            // Cancel outside the tab so its pressed fill must be cleared.
            QTest::mouseMove(tab, QPoint(-10, -10));
            QTest::mouseRelease(tab, Qt::LeftButton, Qt::NoModifier, QPoint(-10, -10));
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
            QVERIFY(!tab->isDown());
            QVERIFY2(paints.painted.subtracted(column).isEmpty(),
                     "Cancelling one tab repaints the rest of the navigation bar");
            QCOMPARE(bar.grab().toImage(), before);
            tab->setChecked(false);
        }
    }

    // Checks partial tab refresh coverage without changing panel or overlay cleanup policies.
    void navigationRefreshKeepsOverlayAndPanelPolicies()
    {
        QWidget owner;
        owner.resize(300, 400);
        QWidget bar(&owner);
        bar.setProperty("reader_navigation_bar", true);
        bar.setGeometry(0, 340, 300, 60);
        QWidget overlay(&owner);
        overlay.setGeometry(100, 320, 100, 80);
        owner.show();
        bar.show();
        overlay.show();
        overlay.raise();
        const auto navigation = navigationRefreshRegion(&owner);
        QCOMPARE(navigation, QRegion(bar.geometry()).subtracted(overlay.geometry()));
        using Policies = QVector<QPair<QRect, quint64>>;
        constexpr quint64 text = 16, repair = 32, full = 2, partial = 1, modes = 3;
        const QRect combined(0, 200, 300, 200);
        const auto selected =
                partialNavigationRefresh(Policies{ { combined, text | full } }, combined,
                                         navigation, text | full, modes, partial);
        QRegion coverage, partialCoverage;
        for (const auto &entry : selected) {
            QVERIFY(coverage.intersected(entry.first).isEmpty());
            coverage += entry.first;
            if (entry.second == (text | partial))
                partialCoverage += entry.first;
            else
                QCOMPARE(entry.second, text | full);
        }
        QCOMPARE(coverage, QRegion(combined));
        QCOMPARE(partialCoverage, navigation);
        coverage = {};
        partialCoverage = {};
        submitNavigationRefresh(
                Policies{ { combined, text | full }, { bar.geometry(), text | full } }, combined,
                navigation, text | full, modes, partial, 64,
                [&](const QRegion &damage, const Policies &policies) {
                    QVERIFY(coverage.intersected(damage).isEmpty());
                    coverage += damage;
                    const bool tabs = damage.subtracted(navigation).isEmpty();
                    QVERIFY(tabs || damage.intersected(navigation).isEmpty());
                    for (const auto &entry : policies) {
                        QVERIFY(damage.contains(entry.first));
                        QCOMPARE(entry.second, text | (tabs ? partial : full) | quint64(64));
                    }
                    if (tabs)
                        partialCoverage += damage;
                });
        QCOMPARE(coverage, QRegion(combined));
        QCOMPARE(partialCoverage, navigation);
        const Policies clean{ { combined, repair | full } };
        QCOMPARE(partialNavigationRefresh(clean, combined, navigation, text | full, modes, partial),
                 clean);
        int unchangedCalls = 0;
        submitNavigationRefresh(clean, combined, navigation, text | full, modes, partial, 64,
                                [&](const QRegion &damage, const Policies &policies) {
                                    ++unchangedCalls;
                                    QCOMPARE(damage, QRegion(combined));
                                    QCOMPARE(policies, clean);
                                });
        QCOMPARE(unchangedCalls, 1);
        for (bool plain : { true, false }) {
            int calls = 0;
            const bool handled = submitNavigationDismissal(
                    Policies{ { combined, text | full } }, combined, navigation, text | full,
                    repair | full, modes, partial, full, 64, plain,
                    [&](const QRegion &damage, const Policies &policies) {
                        ++calls;
                        QCOMPARE(damage, QRegion(combined));
                        QCOMPARE(policies.size(), 1);
                        QCOMPARE(policies[0].second, text | (plain ? partial : full) | quint64(64));
                    });
            QVERIFY(handled);
            QCOMPARE(calls, 1);
        }
        const Policies mixed{ { QRect(0, 200, 300, 140), repair | full },
                              { bar.geometry(), text | full } };
        int mixedCalls = 0;
        QVERIFY(submitNavigationDismissal(
                mixed, combined, navigation, text | full, repair | full, modes, partial, full, 64,
                true, [&](const QRegion &damage, const Policies &policies) {
                    ++mixedCalls;
                    QCOMPARE(damage, QRegion(combined));
                    QCOMPARE(policies.size(), mixed.size());
                    for (int i = 0; i < policies.size(); ++i) {
                        QCOMPARE(policies[i].first, mixed[i].first);
                        QCOMPARE(policies[i].second, mixed[i].second | quint64(64));
                    }
                }));
        QCOMPARE(mixedCalls, 1);
        bool unfamiliarSubmitted = false;
        QVERIFY(!submitNavigationDismissal(
                Policies{ { combined, quint64(128) | full } }, combined, navigation, text | full,
                repair | full, modes, partial, full, 64, true,
                [&](const QRegion &, const Policies &) { unfamiliarSubmitted = true; }));
        QVERIFY(!unfamiliarSubmitted);
        // A masked overlay covers only its painted/input region, not its gaps.
        overlay.setMask(QRegion(0, 0, 100, 40));
        QCOMPARE(navigationRefreshRegion(&owner),
                 QRegion(bar.geometry()).subtracted(QRect(100, 340, 100, 20)));
        overlay.hide();
        QCOMPARE(navigationRefreshRegion(&owner), QRegion(bar.geometry()));
        bar.hide();
        QVERIFY(navigationRefreshRegion(&owner).isEmpty());
    }

    void hidingSelectionMenuKeepsDamageCompact_data()
    {
        QTest::addColumn<bool>("barrier");
        QTest::newRow("blocking-with-grip-holes") << true;
        QTest::newRow("nonblocking-card-only") << false;
    }

    void hidingSelectionMenuKeepsDamageCompact()
    {
        QFETCH(bool, barrier);
        // Use a native backing store. A graphics proxy redraws its cached surface
        // and conceals the fragmented exposure caused by circular input holes.
        QWidget owner;
        owner.resize(1264, 1680);
        ReadingPage paper(&owner, [] { });
        paper.setGeometry(owner.rect());
        paper.image = QImage(owner.size(), QImage::Format_RGB32);
        paper.image.fill(Qt::white);
        paper.setAnnotationInputEnabled(true);
        AnnotationGeometry geometry;
        geometry.rectangles = { QRect(200, 500, 120, 30), QRect(900, 1150, 120, 30) };
        geometry.start = { QPoint(200, 500), QPoint(200, 530), QPoint(-1, 0), true };
        geometry.end = { QPoint(1020, 1150), QPoint(1020, 1180), QPoint(1, 0), true };
        paper.setAnnotationHandles(geometry);
        ui::ModalOverlay overlay(&owner);
        overlay.setInputBarrier(barrier);
        overlay.card()->resize(700, 300);
        overlay.anchorTo(QRect(550, 650, 100, 50));
        overlay.setInputExclusions(paper.annotationHandleHitRegion());
        PaintRegions paints;
        paper.installEventFilter(&paints);
        owner.show();
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        QCoreApplication::processEvents();
        QRegion shownMask;
        for (int repeat = 0; repeat < 2; ++repeat) {
            overlay.show();
            QCoreApplication::processEvents();
            if (barrier)
                QVERIFY(overlay.mask().rectCount() > 20);
            if (repeat == 0)
                shownMask = overlay.mask();
            else
                QCOMPARE(overlay.mask(), shownMask);
            for (const bool start : { true, false }) {
                const auto grip = paper.annotationHandleRect(start).center();
                QCOMPARE(owner.childAt(grip), &paper);
            }
            QVERIFY(overlay.mask().contains(overlay.card()->geometry().center()));
            paints.count = 0;
            paints.painted = {};
            overlay.hide();
            QCoreApplication::processEvents();
            QVERIFY(paints.count > 0);
            const auto damage = panelDamage(&owner, &paper, paints.painted);
            QVERIFY2(damage.size() <= 4,
                     qPrintable(QString("Menu dismissal produced %1 screen-update rectangles")
                                        .arg(damage.size())));
            if (!barrier)
                QVERIFY(shownMask.boundingRect().contains(paints.painted.boundingRect()));
        }
    }

    void delayedProgressPaintsAfterTheEventLoop_data() { variants(); }

    void delayedProgressPaintsAfterTheEventLoop()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        QWidget owner;
        owner.setProperty("reader_dark", dark);
        auto colors = owner.palette();
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        owner.setPalette(colors);
        ui::readerStyle(&owner, ui::styles::reader());
        owner.resize(ui::px(1072), ui::px(1448));
        owner.show();
        ui::ModalOverlay loading(&owner);
        loading.setDimmed(true);
        loading.card()->message("Applying appearance changes", "rrUpdatingText");
        loading.fitCard();
        QVERIFY(loading.progressUpdateRect(35, "Applying appearance changes").isEmpty());
        loading.setDelayedProgress(true, 1100, 20, "Applying appearance changes");
        QCoreApplication::processEvents();
        auto *bar =
                dynamic_cast<ui::ProgressBar *>(loading.findChild<QWidget *>("rrApplyingProgress"));
        QVERIFY(bar);
        QVERIFY(bar->isVisible());
        const auto captures = qEnvironmentVariable("KOBALT_CAPTURE_MODALS");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(owner.grab().save(
                    captures
                    + QString("/relayout-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        }
        QCOMPARE(loading.progressUpdateRect(35, "Applying appearance changes"),
                 QRect(bar->mapTo(loading.window(), QPoint()), bar->size()));
        QVERIFY(loading.progressUpdateRect(20, "Applying appearance changes").isEmpty());
        QVERIFY(loading.progressUpdateRect(35, "Updating page layout").isEmpty());
        PaintRegions paints;
        bar->installEventFilter(&paints);
        PaintRegions ownerPaints;
        owner.installEventFilter(&ownerPaints);

        loading.setDelayedProgress(true, 1100, 35, "Applying appearance changes");
        QCOMPARE(bar->value(), 35);
        QCOMPARE(paints.count, 0);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);
        QVERIFY(paints.painted.boundingRect().width() < bar->width() / 2);
        QVERIFY((ownerPaints.painted - QRegion(QRect(bar->mapTo(&owner, QPoint()), bar->size())))
                        .isEmpty());

        paints.count = 0;
        paints.painted = {};
        loading.setDelayedProgress(true, 1100, 35, "Applying appearance changes");
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);

        loading.setDelayedProgress(true, 1100, 50, "Applying appearance changes");
        loading.setDelayedProgress(true, 1100, 65, "Applying appearance changes");
        QCOMPARE(bar->value(), 65);
        QCOMPARE(paints.count, 0);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);

        owner.setUpdatesEnabled(false);
        loading.setDelayedProgress(true, 1100, 80, "Applying appearance changes");
        owner.setUpdatesEnabled(true);
        QVERIFY(bar->updatesEnabled());
        QCoreApplication::processEvents();

        paints.count = 0;
        loading.setDelayedProgress(false, 1100, 100);
        QCOMPARE(bar->value(), 0);
        QVERIFY(!bar->isVisible());
        QCOMPARE(paints.count, 0);
        QVERIFY(loading.progressUpdateRect(35, "Applying appearance changes").isEmpty());
    }

    void progressPaintsOnlyChangedPixels_data()
    {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("rtl");
        for (const int ppi : { 300, 212 }) {
            for (const bool dark : { false, true }) {
                for (const bool rtl : { false, true }) {
                    const auto name = QString("%1-%2-%3")
                                              .arg(ppi)
                                              .arg(dark ? "dark" : "light")
                                              .arg(rtl ? "rtl" : "ltr");
                    QTest::newRow(qPrintable(name)) << ppi << dark << rtl;
                }
            }
        }
    }

    void progressPaintsOnlyChangedPixels()
    {
        QFETCH(int, ppi);
        QFETCH(bool, dark);
        QFETCH(bool, rtl);

        struct RestoreDensity
        {
            int value = ui::density();

            ~RestoreDensity() { ui::setDensity(value); }
        } restore;

        ui::setDensity(ppi);
        QWidget owner;
        owner.resize(600, 800);
        ui::ProgressBar bar(&owner);
        bar.resize(ui::px(472), bar.height());
        bar.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
        auto palette = bar.palette();
        palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        bar.setPalette(palette);
        owner.show();
        QCoreApplication::processEvents();
        PaintRegions paints, ownerPaints;
        bar.installEventFilter(&paints);
        owner.installEventFilter(&ownerPaints);
        for (const int next : { 20, 35, 65, 40, 0, 100 }) {
            const auto before = bar.grab().toImage();
            paints.painted = {};
            paints.count = ownerPaints.count = 0;
            bar.setValue(next);
            QCOMPARE(paints.count, 0);
            QCoreApplication::processEvents();
            QCOMPARE(paints.count, 1);
            QCOMPARE(ownerPaints.count, 0);
            const auto damage = paints.painted;
            const auto after = bar.grab().toImage();
            QRect changed;
            for (int y = 0; y < before.height(); ++y) {
                for (int x = 0; x < before.width(); ++x) {
                    if (before.pixel(x, y) != after.pixel(x, y)) {
                        changed = changed.united(QRect(x, y, 1, 1));
                    }
                }
            }
            QVERIFY(!changed.isEmpty());
            QCOMPARE(damage, QRegion(changed));
            const int border = ui::metrics::control::border;
            QVERIFY(bar.rect().adjusted(border, border, -border, -border).contains(changed));
        }
        paints.count = 0;
        bar.setValue(200);
        QCoreApplication::processEvents();
        QCOMPARE(bar.value(), 100);
        QCOMPARE(paints.count, 0);
        bar.setValue(-1);
        QCoreApplication::processEvents();
        QCOMPARE(bar.value(), 0);
        bar.resize(2 * int(ui::metrics::control::border) + 12, bar.height());
        QCoreApplication::processEvents();
        paints.count = 0;
        bar.setValue(1);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        bar.setValue(9);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);
    }

    void openingProgressPaintsAfterTheEventLoop()
    {
        QWidget owner;
        owner.resize(600, 800);
        auto opening = createEmbeddedOpening(&owner, test::makeHostPresentation);
        opening->window()->setGeometry(owner.rect());
        owner.show();
        opening->window()->show();
        QCoreApplication::processEvents();
        auto *bar = opening->window()->findChild<QProgressBar *>("rrOpeningProgress");
        QVERIFY(bar);
        QVERIFY(bar->isVisible());
        PaintRegions paints;
        bar->installEventFilter(&paints);
        opening->setProgress(20);
        opening->setProgress(40);
        QCOMPARE(bar->value(), 40);
        QCOMPARE(paints.count, 0);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);
        paints.count = 0;
        opening->setProgress(40);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        opening->setProgress(60);
        opening->setProgress(100);
        opening->window()->hide();
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
    }

    // Checks cover placement, immediate branding, cached covers and covers retained or delivered during handoff.
    void openingCoverLayout()
    {
        for (const QSize screen : { QSize(1072, 1448), QSize(1448, 1072), QSize(600, 800) }) {
            QWidget owner;
            owner.resize(screen);
            {
                auto seeded = createEmbeddedOpening(&owner, test::makeHostPresentation);
                auto *view = seeded->window();
                view->setGeometry(owner.rect());
                QImage seed(80, 120, QImage::Format_RGB32);
                seed.fill(Qt::blue);
                view->setProperty("kobalt_tile_cover_probe", true);
                seeded->setCover(seed);
                owner.show();
                view->show();
                QTRY_COMPARE(view->property("kobalt_first_cover_key").toString(),
                             QString::number(seed.cacheKey()));
                QVERIFY(!view->findChild<QLabel *>("rrOpeningBrand")->isVisible());
            }
            auto opening = createEmbeddedOpening(&owner, test::makeHostPresentation);
            auto *view = opening->window();
            view->setGeometry(owner.rect());
            owner.show();
            view->show();
            QVERIFY(view->findChild<QLabel *>("rrOpeningBrand")->isVisible());
            for (const QSize size : { QSize(200, 300), QSize(300, 100), QSize(100, 400) }) {
                // Each shape is a separate opening, not a lower-quality replacement.
                opening = createEmbeddedOpening(&owner, test::makeHostPresentation);
                view = opening->window();
                view->setGeometry(owner.rect());
                view->show();
                QImage cover(size, QImage::Format_RGB32);
                cover.fill(Qt::red);
                opening->setCover(cover);
                opening->setProgress(45);
                QCoreApplication::processEvents();
                QVERIFY(!view->findChild<QLabel *>("rrOpeningBrand")->isVisible());
                auto *progress = view->findChild<QProgressBar *>("rrOpeningProgress");
                auto *label = view->findChild<QLabel *>("rrOpeningLabel");
                auto *hint = view->findChild<QLabel *>("rrOpeningHint");
                QVERIFY(progress->isVisible());
                QCOMPARE(progress->value(), 45);
                QVERIFY(label->isVisible());
                QVERIFY(label->geometry().bottom() < progress->geometry().top());
                QVERIFY(progress->geometry().bottom() < hint->geometry().top());
                const qreal scale = qMin(screen.width() / ui::metrics::opening::referenceWidth,
                                         screen.height() / ui::metrics::opening::referenceHeight);
                const QPointF origin(
                        (screen.width() - ui::metrics::opening::referenceWidth * scale) / 2,
                        (screen.height() - ui::metrics::opening::referenceHeight * scale) / 2);
                const auto area = ui::metrics::opening::cover;
                const auto fit = QSizeF(size).scaled(area.size(), Qt::KeepAspectRatio);
                const QRectF bounds(
                        origin
                                + (area.center() - QPointF(fit.width() / 2, fit.height() / 2))
                                        * scale,
                        fit * scale);
                const auto pixels = view->grab().toImage();
                QCOMPARE(pixels.pixelColor(bounds.center().toPoint()), QColor(Qt::red));
                QVERIFY(bounds.bottom() < label->y());
                QCOMPARE(pixels.pixelColor(
                                 QPoint(qRound(bounds.left()) - 2, qRound(bounds.center().y()))),
                         view->palette().color(QPalette::Window));
            }
            opening->setCover({});
            QVERIFY(!view->findChild<QLabel *>("rrOpeningBrand")->isVisible());
            opening->showFallback(OpeningFallback::Protected);
            QImage late(200, 300, QImage::Format_RGB32);
            late.fill(Qt::red);
            opening->setCover(late);
            QVERIFY(!view->findChild<QLabel *>("rrOpeningBrand")->isVisible());
            QVERIFY(!view->findChild<QProgressBar *>("rrOpeningProgress")->isVisible());

            // A cached cover can arrive after the routing decision, without a session.
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.filePath("cover.png");
            QVERIFY(late.save(path));
            auto cached = createEmbeddedOpening(&owner, test::makeHostPresentation);
            cached->window()->setGeometry(owner.rect());
            cached->window()->show();
            cached->showFallback(OpeningFallback::Remembered);
            cached->loadCover(path);
            const qreal scale = qMin(screen.width() / ui::metrics::opening::referenceWidth,
                                     screen.height() / ui::metrics::opening::referenceHeight);
            const QPoint center(
                    screen.width() / 2,
                    qRound((screen.height() - ui::metrics::opening::referenceHeight * scale) / 2
                           + ui::metrics::opening::cover.center().y() * scale));
            QTRY_COMPARE(cached->window()->grab().toImage().pixelColor(center), QColor(Qt::red));
            QVERIFY(!cached->window()->findChild<QLabel *>("rrOpeningBrand")->isVisible());

            auto unavailable = createEmbeddedOpening(&owner, test::makeHostPresentation);
            unavailable->window()->setGeometry(owner.rect());
            unavailable->window()->show();
            unavailable->setCover({});
            QVERIFY(unavailable->window()->findChild<QLabel *>("rrOpeningBrand")->isVisible());
            unavailable->showFallback(OpeningFallback::Protected);
            unavailable->setCover(late);
            QCOMPARE(unavailable->window()->grab().toImage().pixelColor(center), QColor(Qt::red));
            QVERIFY(!unavailable->window()->findChild<QLabel *>("rrOpeningBrand")->isVisible());
        }
    }

    void openingCoverUpgrade()
    {
        QWidget owner;
        owner.resize(600, 800);
        const qreal scale = qMin(owner.width() / ui::metrics::opening::referenceWidth,
                                 owner.height() / ui::metrics::opening::referenceHeight);
        const QPoint center(
                owner.width() / 2,
                qRound((owner.height() - ui::metrics::opening::referenceHeight * scale) / 2
                       + ui::metrics::opening::cover.center().y() * scale));
        QImage thumbnail(100, 150, QImage::Format_RGB32);
        thumbnail.fill(Qt::blue);
        QImage full(400, 600, QImage::Format_RGB32);
        full.fill(Qt::red);
        auto opening = createEmbeddedOpening(&owner, test::makeHostPresentation);
        opening->window()->setGeometry(owner.rect());
        opening->setCover(full);
        owner.show();
        opening->window()->show();
        opening->setCover(thumbnail);
        QCOMPARE(opening->window()->grab().toImage().pixelColor(center), QColor(Qt::red));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("full.png");
        QVERIFY(full.save(path));
        auto cached = createEmbeddedOpening(&owner, test::makeHostPresentation);
        cached->window()->setGeometry(owner.rect());
        cached->setCover(thumbnail);
        cached->window()->show();
        cached->loadCover(path);
        QTRY_COMPARE(cached->window()->grab().toImage().pixelColor(center), QColor(Qt::red));
    }

    void openingFirstPaintCallback()
    {
        QWidget owner;
        owner.resize(600, 800);
        auto opening = createEmbeddedOpening(&owner, test::makeHostPresentation);
        opening->window()->setGeometry(owner.rect());
        int calls = 0;
        opening->afterFirstPaint([&] { ++calls; });
        QCoreApplication::processEvents();
        QCOMPARE(calls, 0);
        owner.show();
        opening->window()->show();
        QTRY_COMPARE(calls, 1);
        opening->window()->update();
        QCoreApplication::processEvents();
        QCOMPARE(calls, 1);

        QWidget otherOwner;
        otherOwner.resize(600, 800);
        auto discarded = createEmbeddedOpening(&otherOwner, test::makeHostPresentation);
        discarded->afterFirstPaint([&] { ++calls; });
        discarded.reset();
        otherOwner.show();
        QCoreApplication::processEvents();
        QCOMPARE(calls, 1);
    }

    // Checks that strip changes leave the reading body unpainted, strip captures match full-page crops and a rejected BOOX frame retries unchanged.
    void readingStripChangesLeaveBodyUnpainted_data() { variants(); }

    void readingStripChangesLeaveBodyUnpainted()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        ui::setDensity(ppi);
        ReadingPage page(nullptr, [] { });
        auto colors = page.palette();
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        page.setPalette(colors);
        page.resize(400, 600);
        page.image = QImage(400, 480, QImage::Format_RGB32);
        page.image.fill(Qt::white);
        page.slotContents[0].text = "09:00";
        page.slotContents[4].text = "1 / 10";
        page.refreshContent();
        page.show();
        QCoreApplication::processEvents();
        PaintRegions paints;
        page.installEventFilter(&paints);
        page.slotContents[0].text = "09:01";
        page.slotContents[4].text = "2 / 10";
        page.refreshContent();
        QCoreApplication::processEvents();
        QVERIFY(!paints.painted.isEmpty());
        QVERIFY(!paints.painted.intersects(
                QRect(0, page.topHeight(), page.width(),
                      page.height() - page.topHeight() - page.bottomHeight())));
        QVERIFY(!page.property("reader_page_turn").toBool());
        paints.painted = {};
        page.refreshContent();
        QCoreApplication::processEvents();
        QVERIFY(paints.painted.isEmpty());
        for (const auto size : { QSize(400, 600), QSize(600, 400) }) {
            page.resize(size);
            for (const auto &stripSize : { QStringLiteral("small"), QStringLiteral("large") }) {
                page.chrome.headerSize = page.chrome.footerSize =
                        headerfooter::legacySize(stripSize);
                for (const bool bookmarked : { false, true }) {
                    page.bookmarked = bookmarked;
                    page.refreshContent();
                    const auto frame = page.snapshot();
                    QCOMPARE(page.stripSnapshot(false),
                             frame.copy(0, 0, page.width(), page.topHeight()));
                    QCOMPARE(page.stripSnapshot(true),
                             frame.copy(0, page.height() - page.bottomHeight(), page.width(),
                                        page.bottomHeight()));
                }
            }
        }
        android::BooxRefreshPolicy policy;
        const auto damage = QRegion(page.rect());
        QVERIFY(policy.prepare(&page, &page, &page, damage, [](const auto &) { return true; }));
        page.slotContents[0].text = "10:01";
        page.image = page.image.copy();
        page.refreshContent(true);
        page.setProperty("boox_text_frame", true);
        android::RefreshFrame rejected;
        QVERIFY(!policy.prepare(&page, &page, &page, damage, [&](const auto &frame) {
            rejected = frame;
            return false;
        }));
        QVERIFY(page.property("boox_text_frame").toBool());
        android::RefreshFrame accepted;
        QVERIFY(policy.prepare(&page, &page, &page, damage, [&](const auto &frame) {
            accepted = frame;
            return true;
        }));
        QCOMPARE(accepted.screenPixels, rejected.screenPixels);
        QCOMPARE(accepted.textPixels, rejected.textPixels);
        page.chrome.slotFields.fill(headerfooter::Field::Nothing);
        for (const bool progress : { true, false }) {
            page.chrome.progressBar = progress;
            page.refreshContent();
            QVERIFY(page.stripSnapshot(false).isNull());
            if (progress)
                QCOMPARE(page.stripSnapshot(true),
                         page.snapshot().copy(0, page.height() - page.bottomHeight(), page.width(),
                                              page.bottomHeight()));
            else
                QVERIFY(page.stripSnapshot(true).isNull());
        }
    }

    // Checks that chapter page counts stay complete in every strip slot when a long chapter title must be shortened.
    void chapterPageSurvivesLongTitles_data() { variants(); }

    void chapterPageSurvivesLongTitles()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const int previousDensity = ui::density();

        struct Restore
        {
            int density;

            ~Restore() { ui::setDensity(density); }
        } restore{ previousDensity };

        ui::setDensity(ppi);
        ReadingPage page(nullptr, [] { });
        page.resize(ui::px(500), ui::px(700));
        page.setFont(ui::headerFooterFont());
        page.image = QImage(page.size(), QImage::Format_RGB32);
        page.image.fill(dark ? Qt::black : Qt::white);
        auto palette = page.palette();
        palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        page.setPalette(palette);
        page.chrome.progressBar = false;
        reader::SlotMap preview(nullptr, false);
        preview.setPalette(palette);
        preview.resize(ui::px(1000), preview.sizeHint().height());
        preview.show();
        headerfooter::Values values;
        values.chapter = QStringLiteral("A techjogok munkavállalói jogok: a Para és a tuvlapok ")
                                 .repeated(4);
        values.chapterPage = 123;
        values.chapterPages = 456;
        for (const auto &size : { QStringLiteral("small"), QStringLiteral("large") }) {
            page.chrome.headerSize = page.chrome.footerSize = headerfooter::legacySize(size);
            for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
                page.chrome.slotFields.fill(headerfooter::Field::Nothing);
                page.slotContents = {};
                page.chrome.slotFields[slot] = headerfooter::Field::ChapterAndPage;
                const auto content = headerfooter::content(headerfooter::Field::ChapterAndPage,
                                                           values, slot % 2);
                page.slotContents[slot] = content;
                auto font = page.font();
                font.setPixelSize(slot < 3 ? page.headerTextPixels() : page.footerTextPixels());
                const int ribbon = slot < 3
                        ? qRound(page.width() * .036) + ui::metrics::reading::headerTextGap
                        : 0;
                const int room = page.width() - 2 * ui::metrics::reading::headerSide
                        - (slot % 3 == 1           ? 2 * ribbon
                                   : slot % 3 == 2 ? ribbon
                                                   : 0);
                const auto fitted = ui::headerFooterText(content, font, room);
                QVERIFY(fitted.endsWith(content.chapterPosition));
                QVERIFY(fitted.contains(QChar(0x2026)));
                QVERIFY(ui::interfaceTextWidth(fitted, font) <= room);
                const auto ellipsisAndPage =
                        strings::get("reader.chapter_position")
                                .arg(QString(QChar(0x2026)), content.chapterPosition);
                QCOMPARE(ui::headerFooterText(content, font,
                                              ui::interfaceTextWidth(ellipsisAndPage, font)),
                         ellipsisAndPage);
                QCOMPARE(
                        ui::headerFooterText(content, font,
                                             ui::interfaceTextWidth(content.chapterPosition, font)),
                        content.chapterPosition);
                QCOMPARE(ui::headerFooterText(content, font,
                                              ui::interfaceTextWidth(content.text, font)),
                         content.text);
                page.refreshContent();
                const auto actual = page.snapshot();
                const auto captures = qEnvironmentVariable("KOBALT_CAPTURE_HEADER_FOOTER");
                if (!captures.isEmpty()) {
                    QVERIFY(QDir().mkpath(captures));
                    const auto band = slot < 3 ? page.headerTextRect() : page.footerTextRect();
                    QVERIFY(actual.copy(band).save(captures
                                                   + QString("/strip-%1-%2-%3-%4.png")
                                                             .arg(dark ? "dark" : "light")
                                                             .arg(ppi)
                                                             .arg(size)
                                                             .arg(slot)));
                }
                // A plain field with the complete page suffix must produce the same pixels.
                page.slotContents[slot] = {};
                page.slotContents[slot].text = fitted;
                page.refreshContent();
                QCOMPARE(actual, page.snapshot());

                reader::SlotContents contents{};
                contents[slot] = content;
                preview.setSlots(page.chrome, contents);
                auto *cell = preview.cell(slot);
                namespace headerFooterMetrics = ui::metrics::headerFooter;
                const int left = slot % 3 == 0 ? int(headerFooterMetrics::mapSideInset)
                                               : headerFooterMetrics::slotGap / 2;
                const int right = slot % 3 == 2 ? int(headerFooterMetrics::mapSideInset)
                                                : headerFooterMetrics::slotGap / 2;
                const int previewRoom = cell->width() - left - right
                        - 2 * (ui::metrics::list::lineInset - headerFooterMetrics::slotGap / 2);
                const auto previewImage = cell->grab().toImage();
                contents[slot] = {};
                contents[slot].text = ui::headerFooterText(content, font, previewRoom);
                QVERIFY(contents[slot].text.endsWith(content.chapterPosition));
                preview.setSlots(page.chrome, contents);
                QCOMPARE(previewImage, cell->grab().toImage());
            }
        }
        auto font = ui::headerFooterFont(24);
        for (const auto &title :
             { QStringLiteral("第一章　とても長い章の名前").repeated(20),
               QStringLiteral("A title containing · 9 of 10 · and more text ").repeated(10) }) {
            values.chapter = title;
            const auto content =
                    headerfooter::content(headerfooter::Field::ChapterAndPage, values, false);
            const auto fitted = ui::headerFooterText(content, font, ui::px(250));
            QVERIFY(fitted.endsWith(content.chapterPosition));
            QVERIFY(ui::interfaceTextWidth(fitted, font) <= ui::px(250));
        }
    }

    void batteryTextAlignment_data() { variants(); }

    void batteryTextAlignment()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const int previousDensity = ui::density();

        struct Restore
        {
            int density;

            ~Restore() { ui::setDensity(density); }
        } restore{ previousDensity };

        ui::setDensity(ppi);
        ReadingPage page(nullptr, [] { });
        page.resize(ui::px(800), ui::px(1000));
        page.image = QImage(page.size(), QImage::Format_RGB32);
        const auto paper = dark ? Qt::black : Qt::white;
        page.image.fill(paper);
        auto palette = page.palette();
        palette.setColor(QPalette::Window, paper);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        page.setPalette(palette);
        page.chrome.slotFields.fill(headerfooter::Field::Nothing);
        page.chrome.slotFields[2] = page.chrome.slotFields[5] = headerfooter::Field::BatteryPercent;
        page.chrome.progressBar = false;
        const auto inkBounds = [dark](const QImage &image, QRect area) {
            QRect ink;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x)
                    if (dark ? qGray(image.pixel(x, y)) > 128 : qGray(image.pixel(x, y)) < 128)
                        ink = ink.united(QRect(x, y, 1, 1));
            return ink;
        };
        ui::BatteryIndicator status(nullptr);
        status.setPalette(ui::readerPalette(status.palette(), dark));
        status.setFont(ui::interfaceFont(ui::metrics::panel::batteryText));
        for (int scale : { 1, 2 }) {
            for (int percent : { -1, 9, 50, 100 }) {
                for (bool charging : { false, true }) {
                    status.setStatus(percent, charging);
                    QImage image(status.size() * scale, QImage::Format_RGB32);
                    image.setDevicePixelRatio(scale);
                    image.fill(paper);
                    QPainter painter(&image);
                    status.render(&painter);
                    painter.end();
                    const int split =
                            (status.width() - ui::metrics::panel::batteryIconSize) * scale;
                    const auto text = inkBounds(image, QRect(0, 0, split, image.height()));
                    const auto icon = inkBounds(
                            image, QRect(split, 0, image.width() - split, image.height()));
                    QVERIFY(!text.isEmpty() && !icon.isEmpty());
                    const auto context =
                            QString("status %1 charging=%2 scale=%3 text=%4,%5 icon=%6,%7")
                                    .arg(percent)
                                    .arg(charging)
                                    .arg(scale)
                                    .arg(text.top())
                                    .arg(text.height())
                                    .arg(icon.top())
                                    .arg(icon.height())
                                    .toUtf8();
                    QVERIFY2(qAbs(text.top() + text.bottom() - icon.top() - icon.bottom())
                                     <= 2 * scale,
                             context.constData());
                    const auto capture = qEnvironmentVariable("KOBALT_STATUS_BATTERY_CAPTURE");
                    if (!capture.isEmpty()) {
                        QVERIFY(QDir().mkpath(capture));
                        QVERIFY(image.save(capture
                                           + QString("/status-%1-%2-%3x-%4-%5.png")
                                                     .arg(dark ? "dark" : "light")
                                                     .arg(ppi)
                                                     .arg(scale)
                                                     .arg(percent)
                                                     .arg(charging ? "charging" : "battery")));
                    }
                }
            }
        }
        for (const auto &family : { QStringLiteral("Libron"), QStringLiteral("Ancizar Serif"),
                                    ui::chromeFont(24).family() }) {
            auto font = ui::headerFooterFont(24);
            font.setFamily(family);
            page.setFont(font);
            int previousIconHeight = 0, smallIconHeight = 0;
            for (const int size : { 24, 26, 28, 48 }) {
                page.chrome.headerSize = page.chrome.footerSize = size;
                const int step = (size - 24) / 2;
                QCOMPARE(page.topHeight(), ui::px(48 + step * 4));
                QCOMPARE(page.bottomHeight(), ui::px(48 + step * 4));
                QCOMPARE(page.headerTextPixels(), ui::px(24 + step * 2));
                QCOMPARE(page.footerTextPixels(), ui::px(24 + step * 2));
                int iconHeight = 0;
                int textHeights[2] = {};
                for (int percent : { 9, 50, 100 }) {
                    headerfooter::Values values;
                    values.deviceStatus = true;
                    values.battery = percent;
                    page.slotContents[2] = page.slotContents[5] = headerfooter::content(
                            headerfooter::Field::BatteryPercent, values, false);
                    page.refreshContent();
                    const auto image = page.snapshot();
                    for (bool footer : { false, true }) {
                        const auto band = footer ? page.footerTextRect() : page.headerTextRect();
                        const int pixels =
                                footer ? page.footerTextPixels() : page.headerTextPixels();
                        const int iconLeft = page.width() - ui::metrics::reading::headerSide
                                - qCeil(ui::batteryWidth(qRound(pixels * .75)));
                        const auto text = inkBounds(
                                image,
                                QRect(0, band.top(), iconLeft - ui::metrics::reading::batteryGap,
                                      band.height()));
                        const auto icon = inkBounds(image,
                                                    QRect(iconLeft, band.top(),
                                                          page.width() - iconLeft, band.height()));
                        QVERIFY(!text.isEmpty() && !icon.isEmpty());
                        const auto context = QString("%1 %2 %3 footer=%4 text=%5,%6 icon=%7,%8")
                                                     .arg(family)
                                                     .arg(size)
                                                     .arg(percent)
                                                     .arg(footer)
                                                     .arg(text.top())
                                                     .arg(text.height())
                                                     .arg(icon.top())
                                                     .arg(icon.height())
                                                     .toUtf8();
                        QVERIFY2(qAbs(text.top() + text.bottom() - icon.top() - icon.bottom()) <= 2,
                                 context.constData());
                        QVERIFY2(qAbs(text.height() - icon.height()) <= 3, context.constData());
                        if (textHeights[footer])
                            QVERIFY2(qAbs(text.height() - textHeights[footer]) <= 1,
                                     context.constData());
                        textHeights[footer] = text.height();
                        if (!footer) {
                            if (iconHeight)
                                QCOMPARE(icon.height(), iconHeight);
                            iconHeight = icon.height();
                        }
                    }
                }
                QVERIFY(iconHeight >= previousIconHeight);
                if (size == 24)
                    smallIconHeight = iconHeight;
                if (size >= 28)
                    QVERIFY(iconHeight > smallIconHeight);
                previousIconHeight = iconHeight;
            }
        }
    }

    void annotationPaintPreservesText_data() { variants(); }

    void annotationPaintPreservesText()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const int density = ui::density();

        struct Restore
        {
            int density;

            ~Restore() { ui::setDensity(density); }
        } restore{ density };

        ui::setDensity(ppi);
        ReadingPage page(nullptr, [] { });
        page.resize(500, 600);
        page.show();
        QCoreApplication::processEvents();
        page.image = QImage(500, 500, QImage::Format_RGB32);
        const QRect passage(100, 100, 200, 60);
        page.setProperty("reader_dark", dark);
        QPalette palette = page.palette();
        palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        page.setPalette(palette);
        page.image.fill(dark ? Qt::black : Qt::white);
        const QPoint interior = passage.center() + QPoint(0, page.topHeight());
        page.image.setPixelColor(passage.center(), dark ? Qt::white : Qt::black);
        page.refreshContent();
        page.annotate({}, {});
        const QImage plain = page.snapshot();
        page.annotate({ passage }, {});
        const QImage draft = page.snapshot();
        QCOMPARE(draft.pixelColor(interior), plain.pixelColor(interior));
        const int y = passage.top() + page.topHeight();
        const int x = passage.center().x();
        const int baseline = passage.bottom() + page.topHeight();
        const QRect outer = passage.translated(0, page.topHeight()).adjusted(-2, -2, 2, 2);
        const QRect inner = passage.translated(0, page.topHeight()).adjusted(2, 2, -2, -2);
        for (int row = 0; row < draft.height(); ++row)
            for (int col = 0; col < draft.width(); ++col)
                if (!outer.contains(col, row) || inner.contains(col, row))
                    QCOMPARE(draft.pixelColor(col, row), plain.pixelColor(col, row));
        for (const QPoint &edge :
             { QPoint(x, y), QPoint(x, baseline), QPoint(passage.left(), interior.y()),
               QPoint(passage.right(), interior.y()) })
            QVERIFY(draft.pixelColor(edge) != plain.pixelColor(edge));
        QCoreApplication::processEvents();
        PaintRegions repairs;
        page.installEventFilter(&repairs);
        page.annotate({ passage }, {});
        QCoreApplication::processEvents();
        QCOMPARE(repairs.count, 0);
        const QRect longer = passage.adjusted(0, 0, 40, 0);
        page.annotate({ longer }, {});
        QCoreApplication::processEvents();
        const QImage preview = page.snapshot();
        const QPoint previewExtension(passage.right() + 30, baseline);
        QVERIFY(repairs.count > 0);
        QVERIFY(repairs.painted.contains(previewExtension));
        QVERIFY(preview != draft);
        repairs.painted = {};
        repairs.count = 0;
        page.annotate({ passage }, {});
        QCoreApplication::processEvents();
        QVERIFY(repairs.count > 0);
        QVERIFY(repairs.painted.contains(previewExtension));
        QCOMPARE(page.snapshot(), draft);
        page.annotate({ passage }, {}, { QRect(320, 220, 80, 30) });
        QVERIFY(page.snapshot() != draft);
        page.annotate({ passage }, {});
        QCOMPARE(page.snapshot(), draft);
        page.annotate({}, { passage });
        const QImage saved = page.snapshot();
        const QColor gray = saved.pixelColor(x + 10, y + 10);
        QVERIFY(gray.red() > 0 && gray.red() < 255);
        QCOMPARE(gray.red(), gray.green());
        QCOMPARE(gray.green(), gray.blue());
        QCOMPARE(saved.pixelColor(interior), plain.pixelColor(interior));
        page.setSavedHighlights({ { QVector<QRect>{ passage, passage }, "yellow" } });
        const QImage yellow = page.snapshot();
        QCOMPARE(yellow.pixelColor(interior), plain.pixelColor(interior));
        QVERIFY(yellow.pixelColor(x + 10, y + 10) != gray);
        QVERIFY(page.property("reader_colour_regions").toBool());
        page.setSavedHighlights({ { QVector<QRect>{ passage }, "blue" } });
        const QImage blue = page.snapshot();
        QCOMPARE(blue.pixelColor(interior), plain.pixelColor(interior));
        QVERIFY(blue.pixelColor(x + 10, y + 10) != yellow.pixelColor(x + 10, y + 10));
        page.setSavedHighlights({ { QVector<QRect>{ passage }, "gray", "underline" } });
        const auto underlined = page.snapshot();
        QCOMPARE(underlined.pixelColor(interior), plain.pixelColor(interior));
        QCOMPARE(underlined.pixelColor(x + 10, y + 10), plain.pixelColor(x + 10, y + 10));
        QVERIFY(underlined != plain && underlined != saved);
        page.setSavedHighlights({ { QVector<QRect>{ passage }, "gray", "dotted" } });
        const auto dotted = page.snapshot();
        QCOMPARE(dotted.pixelColor(interior), plain.pixelColor(interior));
        QVERIFY(dotted != plain && dotted != underlined && dotted != saved);
        bool hasInk = false, hasGap = false;
        for (int at = passage.left(); at <= passage.right(); ++at) {
            const QPoint point(at, baseline);
            hasInk |= dotted.pixelColor(point) != plain.pixelColor(point);
            hasGap |= dotted.pixelColor(point) == plain.pixelColor(point);
        }
        QVERIFY(hasInk && hasGap);
        page.setSavedHighlights({ { QVector<QRect>{ passage }, "blue", "underline" } });
        const auto blueUnderline = page.snapshot();
        QVERIFY(blueUnderline != underlined);
        const QColor lineColor = blueUnderline.pixelColor(passage.left() + 10, baseline);
        QVERIFY(lineColor.blue() > lineColor.red());
        page.setSavedHighlights({ { QVector<QRect>{ passage }, "blue", "dotted" } });
        QVERIFY(page.snapshot() != dotted);
        page.setSavedHighlights(
                { { QVector<QRect>{ passage }, "yellow" }, { QVector<QRect>{ passage }, "gray" } });
        QCOMPARE(page.snapshot(), saved);
        QVERIFY(!page.property("reader_colour_regions").toBool());
        QVERIFY(!page.property("reader_annotation_repairs").value<QRegion>().isEmpty());
        page.annotate({}, {});
        QCOMPARE(page.snapshot(), plain);
        QVERIFY(page.property("reader_annotation_regions").value<QRegion>().isEmpty());
        const auto outgoing = annotationRepairRegions(&page);
        QVERIFY(!outgoing.isEmpty());
        consumeAnnotationRepairs(&page, outgoing);
        QVERIFY(annotationRepairRegions(&page).isEmpty());
    }

    void annotationDamageBatchesLogicalUpdate_data() { variants(); }

    void annotationDamageBatchesLogicalUpdate()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const int originalDensity = ui::density();

        struct Restore
        {
            int value;

            ~Restore() { ui::setDensity(value); }
        } restore{ originalDensity };

        ui::setDensity(ppi);
        ReadingPage page(nullptr, [] { });
        page.resize(600, 800);
        page.setProperty("reader_dark", dark);
        page.image = QImage(600, 700, QImage::Format_RGB32);
        page.image.fill(dark ? Qt::black : Qt::white);
        page.refreshContent();
        page.show();
        QCoreApplication::processEvents();
        PaintRegions paints;
        page.installEventFilter(&paints);
        QVector<QRect> lines;
        for (int i = 0; i < 6; ++i) {
            lines.append(QRect(30, 70 + i * 45, 480 - i * 20, 25));
        }
        page.annotate(lines, {});
        QTRY_VERIFY(paints.count > 0);
        QCOMPARE(paints.count, 1);
        QCOMPARE(paints.painted.rectCount(), 1);
        QVERIFY(paints.painted.contains(lines.first().translated(0, page.topHeight())));
        QVERIFY(paints.painted.contains(lines.last().translated(0, page.topHeight())));
        const QVector<HighlightGeometry> blue{ { lines, "blue" } };
        const QVector<HighlightGeometry> pink{ { lines.mid(0, 3), "pink" },
                                               { lines.mid(3), "green" } };
        const QVector<std::function<void()>> changes{
            [&] { page.annotate(lines.mid(0, 2), {}, lines.mid(3)); },
            [&] { page.annotate(lines, {}); },
            [&] { page.annotate({}, lines); },
            [&] { page.setSavedHighlights(blue); },
            [&] { page.setSavedHighlights(pink); },
            [&] { page.annotate(lines.mid(1, 4), lines); },
            [&] { page.annotate({}, lines); },
            [&] {
                page.setSavedHighlights({});
                page.annotate({}, {});
            }
        };
        for (const auto &change : changes) {
            paints.count = 0;
            paints.painted = {};
            change();
            QTRY_VERIFY(paints.count > 0);
            QCOMPARE(paints.count, 1);
            QCOMPARE(paints.painted.rectCount(), 1);
            QVERIFY(paints.painted.boundingRect().width() < page.width());
            QVERIFY(paints.painted.contains(lines.first().translated(0, page.topHeight())));
            QVERIFY(paints.painted.contains(lines.last().translated(0, page.topHeight())));
        }
        paints.count = 0;
        page.annotate({}, {});
        page.setSavedHighlights({});
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
    }

    void annotationFrameDamageJoinsRawDamage_data() { variants(); }

    void annotationFrameDamageJoinsRawDamage()
    {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const int density = ui::density();

        struct Restore
        {
            int density;

            ~Restore() { ui::setDensity(density); }
        } restore{ density };

        ui::setDensity(ppi);
        ReadingPage page(nullptr, [] { });
        page.resize(600, 800);
        page.setProperty("reader_dark", dark);
        page.image = QImage(600, 700, QImage::Format_RGB32);
        page.image.fill(dark ? Qt::black : Qt::white);
        page.refreshContent();
        page.show();
        QCoreApplication::processEvents();
        const QImage plain = page.snapshot();
        PaintRegions paints;
        page.installEventFilter(&paints);
        const QVector<QRect> lines{ { 30, 70, 450, 25 }, { 30, 300, 420, 25 } };
        const QVector<HighlightGeometry> blue{ { lines, "blue" } };
        page.installAnnotationFrame({}, blue);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        page.preparedFrom = page.image.cacheKey();
        page.preparedDamage = {};
        page.refreshContent(true, true);
        QCOMPARE(paints.count, 1);
        QVERIFY(paints.painted.contains(lines.first().translated(0, page.topHeight())));
        QVERIFY(paints.painted.contains(lines.last().translated(0, page.topHeight())));
        QVERIFY(page.snapshot() != plain);
        QVERIFY(page.window()->property("reader_colour_regions").toBool());
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);
        paints.count = 0;
        paints.painted = {};
        page.installAnnotationFrame(lines, {});
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        page.preparedFrom = page.image.cacheKey();
        page.preparedDamage = QRect(520, 500, 20, 20);
        page.refreshContent(true, true);
        QCOMPARE(paints.count, 1);
        QVERIFY(paints.painted.contains(QRect(520, 500, 20, 20).translated(0, page.topHeight())));
        QVERIFY(paints.painted.contains(lines.last().translated(0, page.topHeight())));
        QVERIFY(!page.window()->property("reader_colour_regions").toBool());
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);
        paints.count = 0;
        page.installAnnotationFrame({}, {});
        page.refreshContent(true, true);
        QCOMPARE(paints.count, 1);
        QCOMPARE(page.snapshot(), plain);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);
    }

    void annotationHandlesDragAndCancel()
    {
        ReadingPage page(nullptr, [] { });
        page.resize(600, 800);
        page.show();
        page.image = QImage(600, 700, QImage::Format_RGB32);
        page.image.fill(Qt::white);
        page.refreshContent();
        page.setAnnotationInputEnabled(true);
        AnnotationGeometry geometry;
        geometry.rectangles = { { 100, 100, 200, 40 } };
        geometry.start = { { 100, 100 }, { 100, 140 }, { -1, 0 }, true };
        geometry.end = { { 300, 100 }, { 300, 140 }, { 1, 0 }, true };
        page.annotate(geometry.rectangles, {});
        page.setAnnotationHandles(geometry);
        const auto first = page.annotationHandleRect(true), last = page.annotationHandleRect(false);
        QVERIFY(!first.isEmpty() && !last.isEmpty());
        QVERIFY(first.bottom() <= geometry.rectangles.first().top() + page.topHeight());
        QVERIFY(last.top() >= geometry.rectangles.last().bottom() + page.topHeight());
        int boundary = -2, taps = 0, swipes = 0, finishes = 0;
        bool cancelled = false;
        QPoint moved;
        page.tapped = [&](QPoint) { ++taps; };
        page.swiped = [&](int) { ++swipes; };
        page.annotationDragStarted = [&](QPoint, int value) {
            boundary = value;
            return true;
        };
        page.annotationDragMoved = [&](QPoint point) { moved = point; };
        page.annotationDragFinished = [&](bool value) {
            cancelled = value;
            ++finishes;
        };
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, first.center());
        QCOMPARE(boundary, -2);
        QTest::mouseMove(&page, first.center() + QPoint(150, 80));
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier,
                            first.center() + QPoint(150, 80));
        QCOMPARE(boundary, 0);
        QCOMPARE(finishes, 1);
        QVERIFY(!cancelled);
        QVERIFY(!moved.isNull());
        QCOMPARE(taps, 0);
        QCOMPARE(swipes, 0);
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, last.center());
        QCOMPARE(boundary, 0);
        QTest::mouseMove(&page, last.center() + QPoint(80, 40));
        QCOMPARE(boundary, 1);
        page.cancelHold();
        QCOMPARE(finishes, 2);
        QVERIFY(cancelled);
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, last.center());
        QCOMPARE(finishes, 2);
        QCOMPARE(taps, 0);
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *device = QTest::createTouchDevice();
        auto touch = QTest::touchEvent(&page, device, false);
        touch.press(0, last.center(), &page).commit();
        QCOMPARE(boundary, 1);
        touch.move(0, last.center() + QPoint(80, 40), &page).commit();
        touch.release(0, last.center() + QPoint(80, 40), &page).commit();
        QCOMPARE(finishes, 3);
        QVERIFY(!cancelled);
        QCOMPARE(taps + swipes, 0);
        touch.press(0, first.center(), &page).commit();
        touch.move(0, first.center() + QPoint(80, 40), &page).commit();
        QCOMPARE(boundary, 0);
        QTouchEvent cancel(QEvent::TouchCancel, device);
        QApplication::sendEvent(&page, &cancel);
        QCOMPARE(finishes, 4);
        QVERIFY(cancelled);
        touch.release(0, first.center(), &page).commit();
        QCOMPARE(finishes, 4);
        const int previousFinishes = finishes;
        QTest::mouseClick(&page, Qt::LeftButton, Qt::NoModifier,
                          QPoint(geometry.rectangles.last().right() - 2,
                                 geometry.rectangles.last().bottom() - 2)
                                  + QPoint(0, page.topHeight()));
        QCOMPARE(taps, 1);
        QCOMPARE(finishes, previousFinishes);
        geometry.start.visible = false;
        page.setAnnotationHandles(geometry);
        QVERIFY(page.annotationHandleRect(true).isEmpty());
        QVERIFY(!page.annotationHandleRect(false).isEmpty());
        page.annotate({}, {});
        QVERIFY(page.annotationHandleRect(false).isEmpty());
        // A normal text swipe keeps page navigation; only a hold arms selection.
        const QPoint textPoint(380, 400);
        boundary = -2;
        const int previousSwipes = swipes;
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, textPoint);
        QTest::mouseMove(&page, textPoint - QPoint(180, 0));
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, textPoint - QPoint(180, 0));
        QCOMPARE(swipes, previousSwipes + 1);
        QCOMPARE(boundary, -2);
        const int beforeHold = finishes;
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, textPoint);
        QTRY_COMPARE(boundary, -1);
        QTest::mouseMove(&page, textPoint - QPoint(180, 0));
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, textPoint - QPoint(180, 0));
        QCOMPARE(finishes, beforeHold + 1);
        QCOMPARE(swipes, previousSwipes + 1);
        QVERIFY(!cancelled);
        boundary = -2;
        touch.press(0, textPoint, &page).commit();
        QTRY_COMPARE(boundary, -1);
        touch.move(0, textPoint + QPoint(80, 0), &page).commit();
        touch.release(0, textPoint + QPoint(80, 0), &page).commit();
        QCOMPARE(finishes, beforeHold + 2);
        QCOMPARE(swipes, previousSwipes + 1);
        page.setAnnotationInputEnabled(false);
        page.setAnnotationHandles(geometry);
        QVERIFY(page.annotationHandleRect(false).isEmpty());
    }

    void annotationContourKeepsParagraphGaps()
    {
        const QVector<QRect> lines{ QRect(100, 100, 200, 30), QRect(100, 138, 180, 30),
                                    QRect(100, 210, 200, 30) };
        const auto contour = ReadingPage::annotationContour(lines);
        QVERIFY(contour.contains(QPoint(160, 133)));
        QVERIFY(!contour.contains(QPoint(160, 188)));
        QVERIFY(!contour.contains(QPoint(90, 133)));
        QVERIFY(contour.contains(QPoint(160, 220)));
        QVector<QRect> vertical;
        for (const auto &line : lines) {
            vertical.append(QRect(line.y(), line.x(), line.height(), line.width()));
        }
        const auto verticalContour = ReadingPage::annotationContour(vertical, true);
        QVERIFY(verticalContour.contains(QPoint(133, 160)));
        QVERIFY(!verticalContour.contains(QPoint(188, 160)));
        const auto shortVertical = ReadingPage::annotationContour({ QRect(483, 24, 69, 46) }, true);
        QCOMPARE(shortVertical.boundingRect(), QRect(483, 24, 69, 46));
        const auto transposedHorizontal =
                ReadingPage::annotationContour({ QRect(24, 483, 46, 69) }, false);
        QCOMPARE(shortVertical, QTransform(0, 1, 1, 0, 0, 0).map(transposedHorizontal));
        // A temporary candidate is separate from the draft and must not join it.
        ReadingPage page(nullptr, [] { });
        page.annotate({ lines.first() }, {}, { lines[1] });
        QVERIFY(page.annotationContains(QPoint(160, 115)));
        QVERIFY(!page.annotationContains(QPoint(160, 133)));
        QVERIFY(!page.annotationContains(QPoint(160, 150)));
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    QApplication app(argc, argv);
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    if (QGuiApplication::platformName() != "minimal")
        return 2;
    app.setStyle("Fusion");
    auto &assets = test::readerAssets();
    if (!assets.available() || !ui::initializeInterfaceFonts(assets))
        return 2;
    for (const auto &path : bundledFontFiles(assets))
        if (QFontDatabase::addApplicationFont(path) < 0)
            return 2;
    ui::setDensity(300);
    app.setFont(ui::interfaceFont());
    ReaderUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "reader_ui.moc"
