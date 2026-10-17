#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/sliders.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "assets/assets.h"
#include "assets/font_catalog.h"
#include "reader_inputs.h"
#include "interfaces/reader/views/opening.h"
#include "host_presentation.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "interfaces/reader/ui/style/fonts.h"
#include <QFontInfo>
#include <QFontDatabase>
#include <QJsonObject>
#include <QLabel>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include "storage/reader_storage.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/styles.h"
#include "interfaces/reader/ui/controls/paged_text.h"
#include "interfaces/reader/panels/dictionary_panel.h"
#include "interfaces/reader/settings/dictionaries_page.h"
#include "interfaces/reader/panels/link_panel.h"
#include "platform/nickel/common/browser_handoff.h"
#include "interfaces/reader/panels/chapter_graph.h"
#include "interfaces/reader/panels/choice_screen.h"
#include <QScrollBar>
#include <QApplication>
#include <QVariant>
#include <QMouseEvent>
#include <QPointer>
#include <QPaintEvent>
#include <QPainter>
#include <QAbstractTextDocumentLayout>
#include <QTextLayout>
#include <QTextBlock>
#include <QtMath>
#include <QKeyEvent>
#include <QProgressBar>
#include <QRawFont>
#include <cstdio>
#include <cstdlib>
#include "check.h"
#include "ui_private_text.h"

// Geometry checks run without the device refresh backend.
namespace kobalt::platform {
void textRefresh(QWidget *) {}
}

class PaintCountingButton : public kobalt::ui::Button {
public:
    using Button::Button;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override {
        ++paints;
        Button::paintEvent(event);
    }
};
class PaintCountingCaptionButton : public kobalt::ui::CaptionButton {
public:
    using CaptionButton::CaptionButton;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override {
        ++paints;
        CaptionButton::paintEvent(event);
    }
};
class PaintCountingOption : public kobalt::ui::DescribedOption {
public:
    using DescribedOption::DescribedOption;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override {
        ++paints;
        DescribedOption::paintEvent(event);
    }
};
class PaintCountingListRow : public kobalt::ui::ListRow {
public:
    using ListRow::ListRow;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override {
        ++paints;
        ListRow::paintEvent(event);
    }
};
class StyleCountingObserver : public QObject {
public:
    int changes = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::StyleChange)
            ++changes;
        return false;
    }
};
class PaintCountingObserver : public QObject {
public:
    int paints = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint)
            ++paints;
        return false;
    }
};
class PaintCountingPanel : public kobalt::ui::Panel {
public:
    QRegion painted;

protected:
    void paintEvent(QPaintEvent *event) override {
        painted += event->region();
        Panel::paintEvent(event);
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    {
        // The loading view is built before the whole bundle is verified. A
        // font used before then must not stop the faces registering later.
        const auto early = kobalt::ui::interfaceFont();
        Q_UNUSED(early);
        kobalt::AssetBundle opening(kobalt::test::readerAssets().root(), QStringLiteral(KOBALT_VERSION));
        require(kobalt::ui::initializeOpeningFonts(opening), "Cannot load the opening view's fonts");
        require(QFontInfo(kobalt::ui::interfaceFont()).family() == "Kobalt UI Sans" &&
                    QFontInfo(kobalt::ui::brandFont(32)).family() == "Kobalt UI Serif",
                "Opening view does not draw in the interface fonts");
    }
    require(kobalt::test::readerAssets().available(), "Component assets failed manifest verification");
    bool facesChanged = true;
    require(kobalt::ui::initializeInterfaceFonts(kobalt::test::readerAssets(), &facesChanged),
            "Cannot load verified UI font bytes");
    require(!facesChanged, "Full font initialization replaced the opening faces and repaints the loading view");
    require(QFontInfo(kobalt::ui::interfaceFont()).family() == "Kobalt UI Sans" &&
                QFontInfo(kobalt::ui::chromeFont()).family() == "Kobalt UI Mono" &&
                QFontInfo(kobalt::ui::brandFont(32)).family() == "Kobalt UI Serif",
            "Component font substitution");
    checkPrivateInterfaceText(kobalt::test::readerAssets());
    {
        QWidget host;
        host.resize(1264, 1680);
        kobalt::ui::LightSwipeNotice notice(&host);
        notice.centerIn(host.size());
        notice.show();
        require(notice.testAttribute(Qt::WA_TransparentForMouseEvents), "Light indicator intercepts swipe input");
        auto *value = notice.findChild<QLabel *>("rrLightSwipeValue");
        auto *caption = notice.findChild<QLabel *>("rrLightSwipeCaption");
        for (bool warm : {false, true})
            for (int percent : {0, 30, 50, 80, 100}) {
                notice.setLevel(warm, percent);
                require(value->text() == QString::number(percent) + '%',
                        "Light indicator percentage differs from the service");
                require(caption->text() ==
                            kobalt::strings::get(warm ? "light_panel.warmth" : "light_panel.brightness").toUpper(),
                        "Light indicator names the wrong control");
                require(notice.rect().contains(value->geometry()) && notice.rect().contains(caption->geometry()),
                        "Light indicator labels exceed the card");
                const auto image = notice.grab().toImage();
                const int middleX = image.width() / 2, middleY = image.height() / 2;
                const auto ink = notice.palette().color(QPalette::WindowText);
                for (int offset = 0; offset < int(kobalt::ui::metrics::chrome::heavyRule); ++offset)
                    require(image.pixel(offset, middleY) == ink.rgba() &&
                                image.pixel(image.width() - 1 - offset, middleY) == ink.rgba() &&
                                image.pixel(middleX, offset) == ink.rgba() &&
                                image.pixel(middleX, image.height() - 1 - offset) == ink.rgba(),
                            "Light indicator is missing its heavy border on an edge");
            }
        notice.setLevel(false, -1);
        require(value->text() == "0%", "Light indicator does not clamp low values");
        notice.setLevel(true, 120);
        require(value->text() == "100%", "Light indicator does not clamp high values");
        notice.centerIn(QSize(1680, 1264));
        require(notice.geometry().center() == QRect(0, 0, 1680, 1264).center(),
                "Light indicator is off center in landscape");
    }
    {
        kobalt::ui::PagerBar pager(nullptr, "test");
        for (int count : {0, 1}) {
            pager.setPage(0, count, {}, true);
            require(!pager.previous->isEnabled() && !pager.next->isEnabled(),
                    "Empty or single-page list enabled a turn");
            require(pager.turnTarget(-1) == 0 && pager.turnTarget(1) == 0, "Empty or single-page list changed page");
        }
        pager.setPage(0, 3, {}, true);
        require(pager.previous->isEnabled() && pager.turnTarget(-1) == 2, "List previous did not wrap");
        pager.setPage(2, 3, {}, true);
        require(pager.next->isEnabled() && pager.turnTarget(1) == 0, "List next did not wrap");
        pager.setPage(2, 3);
        require(!pager.next->isEnabled() && pager.turnTarget(1) == 2, "Bounded content pager wrapped");
    }
    {
        using namespace kobalt::ui;
        const int savedDensity = density();
        for (int ppi : {212, 300})
            for (bool dark : {false, true}) {
                setDensity(ppi);
                QWidget first, second;
                const auto setTheme = [](QWidget &host, bool dark) {
                    host.setProperty("reader_dark", dark);
                    auto colors = host.palette();
                    colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
                    colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
                    host.setPalette(colors);
                };
                setTheme(first, dark);
                setTheme(second, !dark);
                PaintCountingListRow row(&first);
                row.setCheckable(true);
                row.resize(px(480), metrics::list::twoLines);
                row.setTitle("A long title with words to elide across the row");
                row.setDetail("A detail");
                row.setAccessory(ListRow::Caption{"123456789", px(60)});
                first.resize(row.size());
                first.show();
                row.show();
                app.processEvents();
                row.clearFocus();
                row.setAttribute(Qt::WA_UnderMouse, false);
                const auto normalRow = row.grab().toImage();
                row.setAttribute(Qt::WA_UnderMouse, true);
                require(row.grab().toImage() == normalRow, "List rows change appearance on hover");
                row.setDown(true);
                require(row.grab().toImage() != normalRow, "List rows lack press feedback");
                row.setDown(false);
                require(row.grab().toImage() == normalRow, "List rows stay highlighted after release");
                int clicks = 0;
                QObject::connect(&row, &QPushButton::clicked, &first, [&] {
                    ++clicks;
                });
                StyleCountingObserver observer;
                for (auto *label :
                     {row.titleLabel(), row.detailLabel(), row.captionLabel(), row.leadingLabel(), row.trailingLabel()})
                    label->installEventFilter(&observer);
                const auto labelInk = [&](QLabel *label, QColor color) {
                    require(label->property("reader_style").toString().endsWith("color:" + color.name() + ';'),
                            "List label styles lag behind presentation state");
                };
                labelInk(row.titleLabel(), first.palette().color(QPalette::WindowText));
                row.blockSignals(true);
                row.setChecked(true);
                labelInk(row.titleLabel(), first.palette().color(QPalette::Window));
                row.setChecked(false);
                row.blockSignals(false);
                labelInk(row.titleLabel(), first.palette().color(QPalette::WindowText));
                row.blockSignals(true);
                const QPoint point = row.rect().center();
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), row.mapToGlobal(point), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), row.mapToGlobal(point), Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(&row, &press);
                QApplication::sendEvent(&row, &release);
                require(row.isChecked(), "Signal-blocked row input lost its native checked state");
                labelInk(row.titleLabel(), first.palette().color(QPalette::Window));
                row.setChecked(false);
                row.blockSignals(false);
                row.setChecked(true);
                labelInk(row.titleLabel(), first.palette().color(QPalette::Window));
                row.setEnabled(false);
                labelInk(row.titleLabel(), QColor("#999999"));
                row.setEnabled(true);
                row.setChecked(false);
                labelInk(row.detailLabel(), noteColor(&row));
                row.setTitlePixels(px(34));
                require(row.titleLabel()->font().pixelSize() == px(34),
                        "Owned title pixels do not reach the row label");
                app.processEvents();
                observer.changes = 0;
                row.paints = 0;
                const auto geometry = row.titleLabel()->geometry();
                row.setAccessory(ListRow::Caption{"123456789", px(60)});
                row.setTitlePixels(px(34));
                row.setTitle(row.text());
                row.setDetail("A detail");
                row.setCaption("");
                row.setLeading("");
                app.processEvents();
                require(row.paints == 0 && observer.changes == 0 && row.titleLabel()->geometry() == geometry,
                        "Unchanged row accessory or title pixels repeats presentation work");
                const int previousWidth = row.titleLabel()->width();
                auto trailingFont = row.trailingLabel()->font();
                trailingFont.setLetterSpacing(QFont::AbsoluteSpacing, px(10));
                row.trailingLabel()->setFont(trailingFont);
                require(row.titleLabel()->width() < previousWidth,
                        "Child font change leaves stale trailing reservation");
                row.setAccessory(ListRow::Chevron{});
                require(row.accessoryValue().kind() == ListRow::Accessory::Kind::Chevron &&
                            !row.accessoryValue().caption() && row.trailingLabel()->isHidden() &&
                            row.trailingLabel()->text().isEmpty(),
                        "Chevron retains a stale trailing caption");
                row.setAccessory(ListRow::Caption{"", -1});
                require(row.accessoryValue().caption()->reservedWidth == 0,
                        "Caption reservation does not clamp negative widths");
                row.setAccessory(ListRow::None{});
                require(row.accessoryValue().kind() == ListRow::Accessory::Kind::None &&
                            !row.accessoryValue().caption() && row.titleLabel()->width() > previousWidth,
                        "Empty accessory keeps trailing space reserved");
                // Rows draw with their parent's colors, even if their own palette is explicit.
                row.setPalette(first.palette());
                auto changed = first.palette();
                changed.setColor(QPalette::WindowText, QColor("#123456"));
                first.setPalette(changed);
                labelInk(row.titleLabel(), QColor("#123456"));
                row.setParent(&second);
                labelInk(row.titleLabel(), second.palette().color(QPalette::WindowText));
                labelInk(row.detailLabel(), noteColor(&row));
                changed.setColor(QPalette::WindowText, QColor("#654321"));
                first.setPalette(changed);
                labelInk(row.titleLabel(), second.palette().color(QPalette::WindowText));
                second.resize(row.size());
                second.show();
                row.show();
                app.processEvents();
                observer.changes = 0;
                const QString style = row.titleLabel()->styleSheet();
                row.grab();
                row.grab();
                require(observer.changes == 0 && row.titleLabel()->styleSheet() == style,
                        "List painting mutates label styles");
                require(clicks == 0, "Row presentation updates emit user intent");
                auto *removed = new ListRow(&first);
                removed->setCheckable(true);
                removed->resize(px(240), px(100));
                removed->show();
                QPointer<ListRow> alive(removed);
                QObject::connect(removed, &QPushButton::clicked, &first, [removed] {
                    delete removed;
                });
                const QPoint target = removed->rect().center();
                QMouseEvent down(QEvent::MouseButtonPress, QPointF(target), removed->mapToGlobal(target),
                                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent up(QEvent::MouseButtonRelease, QPointF(target), removed->mapToGlobal(target),
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(removed, &down);
                QApplication::sendEvent(removed, &up);
                require(!alive, "List row cannot be deleted during its click callback");
            }
        setDensity(savedDensity);
    }
    for (bool dark : {false, true}) {
        namespace m = kobalt::ui::metrics;
        QWidget host;
        host.resize(640, 600);
        host.setProperty("reader_dark", dark);
        auto palette = host.palette();
        palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        host.setPalette(palette);
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        host.show();
        kobalt::ui::ModalOverlay loading(&host);
        loading.card()->message("Applying appearance changes", "rrUpdatingText");
        loading.fitCard();
        for (qint64 elapsed : {qint64(0), qint64(999), qint64(1000)}) {
            loading.setDelayedProgress(true, elapsed, 37, "Applying appearance changes...");
            auto *bar = dynamic_cast<kobalt::ui::ProgressBar *>(loading.findChild<QWidget *>("rrApplyingProgress"));
            require(bar && bar->value() == 37, "Loading bar does not use reported progress");
            require(loading.isVisible() == (elapsed >= 1000),
                    "Appearance notice appears before one second or fails to appear afterwards");
            require(!bar->isHidden() == (elapsed >= 1000), "Appearance bar does not follow the one-second threshold");
            require(loading.card()->findChildren<QLabel *>().size() == 1 &&
                        loading.findChild<QLabel *>("rrUpdatingText")->text() == "Applying appearance changes...",
                    "Relayout notice must contain only its reason label");
            if (elapsed >= 1000)
                require(loading.card()->rect().contains(bar->geometry()), "Loading bar exceeds the modal card");
        }
        for (const auto &message :
             {"Enabling Bold Reading...", "Disabling Bold Reading...", "Updating page layout..."}) {
            loading.setDelayedProgress(true, 1100, 0, message);
            require(loading.findChild<QLabel *>("rrUpdatingText")->text() == message &&
                        loading.card()->findChildren<QLabel *>().size() == 1 &&
                        dynamic_cast<kobalt::ui::ProgressBar *>(loading.findChild<QWidget *>("rrApplyingProgress"))
                                ->value() == 0,
                    "Replacing a relayout reason adds a second label or retains old progress");
        }
        loading.setDelayedProgress(false, 2500, 100);
        require(
            !loading.isVisible() &&
                dynamic_cast<kobalt::ui::ProgressBar *>(loading.findChild<QWidget *>("rrApplyingProgress"))->isHidden(),
            "Completed loading notice remains visible");
        loading.setDelayedProgress(true, 0, 0);
        require(
            !loading.isVisible() &&
                dynamic_cast<kobalt::ui::ProgressBar *>(loading.findChild<QWidget *>("rrApplyingProgress"))->isHidden(),
            "A new render inherits the previous loading notice");
        kobalt::ui::ListRow row(&host);
        row.setTitle("A very long chapter name that must fit in a narrow row");
        require(row.sizeHint().height() == m::list::oneLine, "One-line row has a different height");
        row.setDetail("A passage from the book");
        require(row.sizeHint().height() == m::list::twoLines, "Two-line row has a different height");
        row.setCaption("12 MAY 2026");
        require(row.sizeHint().height() == m::list::threeLines, "Three-line row has a different height");
        auto *remove = row.addAction("remove", "Remove bookmark");
        row.setLeading("01");
        row.setAccessory(kobalt::ui::ListRow::Caption{"123", 60});
        row.resize(480, row.sizeHint().height());
        row.show();
        QApplication::processEvents();
        require(row.titleLabel()->text() != row.text() && row.titleLabel()->text().contains(QChar(0x2026)),
                "Row title is not elided");
        require(remove->width() == m::chrome::cellWidth && remove->height() == row.height() &&
                    remove->geometry().right() == row.width() - 1,
                "Trailing action does not fill its cell");
        require(row.titleLabel()->geometry().right() < row.trailingLabel()->x() &&
                    row.trailingLabel()->geometry().right() < remove->x(),
                "Row text overlaps its trailing cell");
        row.setCheckable(true);
        row.setChecked(true);
        QApplication::processEvents();
        require(qGray(row.grab().toImage().pixel(3, 3)) == (dark ? 255 : 0), "Current row is not inverted");
        row.setDown(true);
        require(qGray(row.grab().toImage().pixel(3, 3)) == (dark ? 255 : 0), "Pressed current row loses its inversion");
        row.setDown(false);
        row.setChecked(false);
        row.setDown(true);
        require(qGray(row.grab().toImage().pixel(3, 3)) == (dark ? 51 : 221), "Pressed row has a different highlight");
        row.setDown(false);
        row.setEnabled(false);
        require(qGray(row.grab().toImage().pixel(3, 3)) == (dark ? 0 : 255), "Disabled row highlights");
        kobalt::ui::ListHeader header(&host);
        header.resize(480, m::list::headerHeight);
        header.setCaptions("Contents · 16 chapters", "Page");
        require(header.leftLabel()->text() == QStringLiteral("Contents · 16 chapters") &&
                    header.rightLabel()->text() == "Page" &&
                    header.leftLabel()->font().capitalization() == QFont::AllUppercase &&
                    header.leftLabel()->geometry().right() < header.rightLabel()->x(),
                "List header loses captions or overlaps columns");
    }
    {
        QWidget reader;
        reader.show();
        kobalt::BrowserHandoff handoff;
        bool suspended = false;
        handoff.begin(&reader, [&] {
            return !suspended;
        });
        QWidget wireless;
        wireless.setObjectName("N3Dialog");
        wireless.show();
        QApplication::processEvents();
        require(reader.isVisible(), "Unrelated native dialog hid the reader");
        wireless.hide();
        QApplication::processEvents();
        QWidget dialog;
        dialog.setObjectName("N3Dialog");
        QWidget browser(&dialog);
        browser.setObjectName("N3BrowserView");
        dialog.show();
        browser.show();
        QApplication::processEvents();
        require(reader.isHidden(), "Native browser did not hide the modal reader");
        require(kobalt::BrowserHandoff::activeFor(&reader),
                "Browser handoff did not block the visibility check from waking the reader");
        dialog.hide();
        QApplication::processEvents();
        require(reader.isVisible(), "Closing the native browser did not restore the reader");
        require(!kobalt::BrowserHandoff::activeFor(&reader), "Closed browser kept the reader wake blocked");
        handoff.begin(&reader, [&] {
            return !suspended;
        });
        dialog.show();
        browser.show();
        QApplication::processEvents();
        suspended = true;
        dialog.hide();
        QApplication::processEvents();
        require(reader.isHidden() && kobalt::BrowserHandoff::activeFor(&reader),
                "Sleeping the browser released its reader handoff");
        suspended = false;
        dialog.show();
        QApplication::processEvents();
        require(reader.isHidden(), "Waking the browser restored the reader above it");
        dialog.hide();
        QApplication::processEvents();
        require(reader.isVisible() && !kobalt::BrowserHandoff::activeFor(&reader),
                "Closing the resumed browser did not restore the reader");
        auto *removed = new QWidget;
        removed->setObjectName("N3Dialog");
        auto *removedBrowser = new QWidget(removed);
        removedBrowser->setObjectName("N3BrowserView");
        handoff.begin(&reader, [&] {
            return !suspended;
        });
        removed->show();
        QApplication::processEvents();
        suspended = true;
        delete removed;
        QApplication::processEvents();
        require(reader.isHidden() && !kobalt::BrowserHandoff::activeFor(&reader),
                "Destroyed browser left a suspended reader held indefinitely");
    }
    const int openingDensity = kobalt::ui::density();
    struct OpeningScreen {
        int width, height, ppi;
    };
    // Reader widgets use physical pixels, including the desktop canvas.
    for (const auto &screen : {OpeningScreen{420, 500, 150}, OpeningScreen{840, 1000, 300},
                               OpeningScreen{800, 600, 212}, OpeningScreen{1072, 1448, 300}})
        for (bool dark : {false, true}) {
            kobalt::ui::setDensity(screen.ppi);
            QJsonObject preferences;
            preferences["dark_mode"] = dark;
            QWidget background;
            background.resize(screen.width, screen.height);
            background.show();
            auto view = kobalt::createOpening(&background, preferences, kobalt::test::makeHostPresentation);
            auto *window = view->window();
            window->resize(screen.width, screen.height);
            window->show();
            app.processEvents();
            auto *progress = window->findChild<QProgressBar *>("rrOpeningProgress");
            auto *label = window->findChild<QLabel *>("rrOpeningLabel");
            auto *detail = window->findChild<QLabel *>("rrOpeningDetail");
            auto *retry = window->findChild<QPushButton *>("rrOpeningNative");
            auto *back = window->findChild<QPushButton *>("rrOpeningBack");
            require(progress && progress->isVisible() && label, "normal opening has progress");
            view->showFallback(kobalt::OpeningFallback::Protected);
            app.processEvents();
            require(!progress->isVisible() && !retry->isVisible() && !back->isVisible(),
                    "handoff has no progress or confirmation");
            require(label->text() == kobalt::strings::get("opening.native"), "handoff caption");
            view->showFallback(kobalt::OpeningFallback::Remembered);
            app.processEvents();
            require(detail->text() == kobalt::strings::get("opening.remembered") && !retry->isVisible(),
                    "remembered failure uses neutral automatic handoff");
            view->showFallback(kobalt::OpeningFallback::Failure);
            app.processEvents();
            auto *failure = window->findChild<QWidget *>("rrOpeningFailure");
            auto *failureTitle = failure->findChild<QLabel *>("rrModalTitle");
            auto *failureBody = failure->findChild<QLabel *>("rrModalDescription");
            require(retry->isVisible() && back->isVisible() && failureBody->isVisible(),
                    "failure has two actions and explanation");
            require(failureTitle->text() == kobalt::strings::get("opening.failed"), "failure caption");
            require(failureTitle->font().pixelSize() == int(kobalt::ui::metrics::chrome::labelText),
                    "failure uses the shared title size");
            for (auto *widget : {static_cast<QWidget *>(failureTitle), static_cast<QWidget *>(failureBody),
                                 static_cast<QWidget *>(retry), static_cast<QWidget *>(back)})
                require(QFontInfo(widget->font()).family() == kobalt::ui::interfaceFontFamily(),
                        "failure uses the shared interface font");
            require(failureTitle->geometry().bottom() < failureBody->geometry().top() &&
                        failureBody->geometry().bottom() < retry->geometry().top(),
                    "failure content does not overlap");
            require(failure->rect().contains(failure->findChild<QWidget *>("rrModalCard")->geometry()),
                    "failure card fits");
            int accepted = 0, rejected = 0;
            QObject::connect(window, &QDialog::accepted, [&] {
                ++accepted;
                require(!window->isVisible(), "native continuation must not have a modal splash");
            });
            QObject::connect(window, &QDialog::rejected, [&] {
                ++rejected;
            });
            retry->click();
            require(accepted == 1 && rejected == 0, "native action accepts");
            window->show();
            back->click();
            require(rejected == 1, "back action rejects");
        }
    kobalt::ui::setDensity(openingDensity);

    {
        using namespace kobalt::ui;
        QWidget host;
        host.setFont(interfaceFont());
        readerStyle(&host, styles::reader());
        BackButton back("Exit Kobalt", &host);
        back.setObjectName("rrReturn");
        back.setContentsMargins(0, 0, metrics::panel::exitLabelExtraRight, 0);
        // A late font change must not retain the width measured at construction.
        for (int pixels : {40, 52}) {
            readerStyle(&back, "QPushButton#rrReturn{" + styles::brand(pixels) + "}");
            back.ensurePolished();
            back.resize(back.sizeHint().width(), 112);
            host.resize(600, 200);
            host.show();
            app.processEvents();
            const int available = back.width() - metrics::navigation::labelLeft - metrics::navigation::labelRight -
                                  back.contentsMargins().right();
            require(back.fontMetrics().elidedText(back.text(), Qt::ElideRight, available) == back.text(),
                    "Back button size hint truncates the Exit Kobalt caption");
        }
        if (app.arguments().contains("--exit-button-only"))
            return 0;
    }
    {
        using namespace kobalt;
        strings::showKeys(true);
        QWidget host;
        ChapterGraph graph(&host);
        require(graph.findChild<QLabel *>("rrGraphChapterCaption")->text() == "stats.chapter",
                "Chapter caption does not expose its catalog key");
        graph.setProgress("Title", 4, 10, 12, 25, 100);
        require(graph.findChild<QLabel *>("rrGraphChapterPercent")->text() == "common.percent [30]",
                "Key mode lost the formatting argument");
        ui::PageNavigation navigation(&host, "keyTest");
        require(navigation.back->text() == "common.back", "Navigation bypasses the string catalog");
        strings::showKeys(false);
    }
    {
        using namespace kobalt::ui;
        BatteryIndicator battery(nullptr);
        readerStyle(&battery, "background:white;color:black;");
        battery.show();
        int previous = -1;
        for (int percent : {0, 10, 25, 50, 75, 90, 100}) {
            battery.setStatus(percent, false);
            app.processEvents();
            const auto image = battery.grab().toImage();
            int ink = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = image.width() - metrics::panel::batteryIconSize; x < image.width(); ++x)
                    ink += qGray(image.pixel(x, y)) < 128;
            require(ink > previous, "Battery fill does not track small percentage changes");
            previous = ink;
        }
    }
    {
        using namespace kobalt::ui;
        // Interface text is unhinted on every path, including style sheet
        // fonts that resolve against an interface font.
        for (const auto &font : {interfaceFont(24), chromeFont(24), captionFont(), brandFont(24)})
            require(font.hintingPreference() == QFont::PreferNoHinting, "Interface font asks for hinting");
        // Nickel's style sheet makes check boxes italic, and a text link is
        // underlined. Painting resolves a font against its widget's, so the
        // interface fonts set these themselves and stay upright and plain.
        QFont decorated;
        decorated.setItalic(true);
        decorated.setUnderline(true);
        for (const auto &font : {interfaceFont(24), chromeFont(24), captionFont()}) {
            const auto painted = font.resolve(decorated);
            require(!painted.italic() && !painted.underline(), "Interface font took a widget's italic or underline");
        }
        QWidget parent;
        parent.setFont(interfaceFont());
        QLabel styled(&parent);
        styled.setStyleSheet("font-size:20px;font-weight:600;");
        styled.ensurePolished();
        require(styled.font().hintingPreference() == QFont::PreferNoHinting,
                "Style sheet font lost the interface hinting preference");
        // Qt 5.2 polishes style sheet fonts with the default hinting. The
        // window's keeper turns such a font back to unhinted.
        QWidget window;
        keepInterfaceUnhinted(&window);
        QLabel reset(&window);
        QFont polished = interfaceFont(20);
        polished.setHintingPreference(QFont::PreferDefaultHinting);
        reset.setFont(polished);
        require(reset.font().hintingPreference() == QFont::PreferNoHinting,
                "Window kept a font with the default hinting");
        QWidget elsewhere;
        QLabel outside(&elsewhere);
        outside.setFont(polished);
        require(outside.font().hintingPreference() == QFont::PreferDefaultHinting,
                "Hinting keeper changed a widget in another window");
    }
    {
        using namespace kobalt::ui;
        for (int id = int(ControlIcon::Search); id <= int(ControlIcon::More); ++id) {
            const auto black =
                controlIcon(ControlIcon(id)).pixmap(64, 64).toImage().convertToFormat(QImage::Format_ARGB32);
            const auto white =
                controlIcon(ControlIcon(id), Qt::white).pixmap(64, 64).toImage().convertToFormat(QImage::Format_ARGB32);
            require(!black.isNull() && black.size() == white.size(), "Bundled icon resource is missing");
            int ink = 0;
            for (int y = 0; y < black.height(); ++y)
                for (int x = 0; x < black.width(); ++x) {
                    const auto b = QColor::fromRgba(black.pixel(x, y)), w = QColor::fromRgba(white.pixel(x, y));
                    require(b.alpha() == w.alpha(), "Icon tint changed its shape");
                    if (b.alpha() > 128) {
                        ++ink;
                        require(b.lightness() == 0 && w.lightness() == 255, "Icon tint lost theme contrast");
                    }
                }
            require(ink > 20, "Bundled icon is empty");
        }
    }
    {
        using namespace kobalt::ui;
        const int savedDensity = density();
        for (int ppi : {167, 212, 300}) {
            setDensity(ppi);
            for (int id = int(ControlIcon::Search); id <= int(ControlIcon::Download); ++id) {
                const auto glyph = ControlIcon(id);
                const auto icon = controlIcon(glyph);
                for (int size : {19, 37, 83, 256}) {
                    const auto raster = icon.pixmap(size, size).toImage();
                    require(raster.size() == QSize(size, size),
                            "Vector icon did not render at its requested pixel size");
                    int visible = 0;
                    for (int y = 0; y < size; ++y)
                        for (int x = 0; x < size; ++x)
                            visible += qAlpha(raster.pixel(x, y)) > 32;
                    require(visible > 3, "Vector glyph has no visible ink");
                    QImage direct(size, size, QImage::Format_ARGB32_Premultiplied);
                    direct.fill(Qt::transparent);
                    {
                        QPainter painter(&direct);
                        icon.paint(&painter, direct.rect());
                    }
                    require(direct.convertToFormat(QImage::Format_ARGB32) ==
                                raster.convertToFormat(QImage::Format_ARGB32),
                            "Icon pixmap and direct drawing use different geometry");
                }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                for (qreal dpr : {1., 1.5, 2.}) {
                    const int pixels = qRound(38 * dpr);
                    const auto raster = icon.pixmap(QSize(38, 38), dpr);
                    require(raster.size() == QSize(pixels, pixels) && qFuzzyCompare(raster.devicePixelRatio(), dpr),
                            "Vector icon applied device pixel ratio twice or lost its physical resolution");
                    QImage direct(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
                    direct.fill(Qt::transparent);
                    direct.setDevicePixelRatio(dpr);
                    {
                        QPainter painter(&direct);
                        icon.paint(&painter, QRect(0, 0, 38, 38));
                    }
                    require(direct.convertToFormat(QImage::Format_ARGB32) ==
                                raster.toImage().convertToFormat(QImage::Format_ARGB32),
                            "High-DPI icon bitmap differs from direct vector drawing");
                }
#endif
            }
            QWidget host;
            host.setProperty("reader_dark", false);
            Button action(&host);
            for (auto glyph : {ControlIcon::Close, ControlIcon::Search, ControlIcon::Portrait, ControlIcon::Download}) {
                action.setIconConfiguration({glyph, IconRendition::Action});
                for (auto state : {QIcon::Off, QIcon::On}) {
                    const auto image = action.icon()
                                           .pixmap(QSize(128, 128), QIcon::Normal, state)
                                           .toImage()
                                           .convertToFormat(QImage::Format_ARGB32);
                    int ink = 0;
                    QRect bounds;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const auto color = QColor::fromRgba(image.pixel(x, y));
                            if (color.alpha() > 128) {
                                require(color.lightness() == (state == QIcon::Off ? 0 : 255),
                                        "Action icon lost its active color");
                                bounds = bounds.united(QRect(x, y, 1, 1));
                                ++ink;
                            }
                        }
                    require(ink > 20, "Action rendition has no visible ink");
                    const int extent = qRound(128. *
                                              int(glyph == ControlIcon::Close ? metrics::control::closeInkSize
                                                                              : metrics::control::actionInkSize) /
                                              int(metrics::control::iconCanvas));
                    require(qAbs(qMax(bounds.width(), bounds.height()) - extent) <= 2,
                            "Action icon does not fit its configured vector ink extent");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    const auto highDpi = action.icon().pixmap(QSize(38, 38), 2., QIcon::Normal, state);
                    QImage direct(76, 76, QImage::Format_ARGB32_Premultiplied);
                    direct.fill(Qt::transparent);
                    direct.setDevicePixelRatio(2.);
                    {
                        QPainter painter(&direct);
                        action.icon().paint(&painter, QRect(0, 0, 38, 38), Qt::AlignCenter, QIcon::Normal, state);
                    }
                    require(highDpi.size() == QSize(76, 76) && highDpi.devicePixelRatio() == 2. &&
                                direct.convertToFormat(QImage::Format_ARGB32) ==
                                    highDpi.toImage().convertToFormat(QImage::Format_ARGB32),
                            "Action or active icon loses final device resolution");
#endif
                }
            }
        }
        setDensity(savedDensity);
        const auto cachedIcon = controlIcon(ControlIcon::Search, QColor("#153759"));
        const auto first = cachedIcon.pixmap(64, 64);
        const auto repeated = cachedIcon.pixmap(64, 64);
        require(first.cacheKey() == repeated.cacheKey(),
                "Repeated icon requests did not reuse their final-size raster");
        const auto recreated = controlIcon(ControlIcon::Search, QColor("#153759")).pixmap(64, 64);
        require(first.cacheKey() == recreated.cacheKey(), "Short-lived icons did not share their final-size raster");
        for (int color = 0; color < 80; ++color)
            controlIcon(ControlIcon::Search, QColor(color, 100, 200)).pixmap(64, 64);
        const auto evicted = cachedIcon.pixmap(64, 64);
        require(first.cacheKey() != evicted.cacheKey() && first.toImage() == evicted.toImage(),
                "Bounded icon cache failed to evict or changed regenerated pixels");
        const auto oversized = cachedIcon.pixmap(400, 400);
        const auto oversizedAgain = cachedIcon.pixmap(400, 400);
        require(oversized.cacheKey() != oversizedAgain.cacheKey() && oversized.toImage() == oversizedAgain.toImage(),
                "Oversized icon entered the bounded cache or changed uncached output");
        const auto disabled = cachedIcon.pixmap(QSize(64, 64), QIcon::Disabled);
        require(disabled.toImage() != evicted.toImage(), "Icon cache mixed disabled and normal pixels");
        for (const QColor &ink : {QColor(Qt::black), QColor(Qt::white), QColor("#666666")}) {
            for (int transform = 0; transform < 5; ++transform) {
                QImage expected(120, 120, QImage::Format_ARGB32_Premultiplied);
                expected.fill(QColor("#4675a3"));
                QImage actual = expected;
                const auto prepare = [transform](QPainter &painter) {
                    if (transform == 1)
                        painter.translate(.25, .5);
                    if (transform == 2)
                        painter.scale(1.5, .75);
                    if (transform == 3) {
                        painter.translate(110, 0);
                        painter.scale(-1, 1);
                    }
                    if (transform == 4)
                        painter.setOpacity(.8);
                };
                {
                    QPainter painter(&expected);
                    prepare(painter);
                    paintOpeningIcon(painter, ControlIcon::Lock, QRect(10, 10, 64, 64), ink);
                }
                {
                    QPainter painter(&actual);
                    prepare(painter);
                    controlIcon(ControlIcon::Lock, ink).paint(&painter, QRect(10, 10, 64, 64));
                }
                require(actual == expected, "Cached or transformed icon changed direct vector edge composition");
            }
        }
        QImage paper(96, 96, QImage::Format_ARGB32_Premultiplied);
        paper.fill(QColor("#4675a3"));
        QPainter painter(&paper);
        painter.setOpacity(.8);
        painter.setPen(Qt::red);
        painter.setBrush(Qt::green);
        painter.translate(2, 3);
        painter.setClipRect(QRect(0, 0, 90, 90));
        const auto transform = painter.transform();
        const auto pen = painter.pen();
        const auto brush = painter.brush();
        const auto clip = painter.clipRegion();
        for (int turn = 0; turn < 4; ++turn)
            controlIcon(ControlIcon(int(ControlIcon::Portrait) + turn), Qt::black, true)
                .paint(&painter, QRect(5, 5, 76, 76));
        require(painter.transform() == transform && painter.pen() == pen && painter.brush() == brush &&
                    painter.clipRegion() == clip && qFuzzyCompare(painter.opacity(), .8) &&
                    painter.compositionMode() == QPainter::CompositionMode_SourceOver,
                "Drawing an icon changed its caller's painter state");
        painter.end();
        for (int y = 0; y < paper.height(); ++y)
            for (int x = 0; x < paper.width(); ++x)
                require(qAlpha(paper.pixel(x, y)) == 255, "Orientation icon erased its caller's background");
    }
    {
        using namespace kobalt::ui;
        // The thick bezel must match the physical edge after Qt's rotation.
        for (bool sideButtons : {false, true}) {
            for (int turn = 0; turn < 4; ++turn) {
                const auto image = controlIcon(ControlIcon(int(ControlIcon::Portrait) + turn), Qt::black, sideButtons)
                                       .pixmap(128, 128)
                                       .toImage();
                int ink[4]{}; // top, right, bottom, left
                for (int along = 45; along < 83; ++along)
                    for (int edge = 0; edge < 40; ++edge) {
                        ink[0] += qAlpha(image.pixel(along, edge));
                        ink[1] += qAlpha(image.pixel(127 - edge, along));
                        ink[2] += qAlpha(image.pixel(along, 127 - edge));
                        ink[3] += qAlpha(image.pixel(edge, along));
                    }
                const int expected = ((sideButtons ? 1 : 2) - turn + 4) % 4;
                for (int edge = 0; edge < 4; ++edge)
                    require(edge == expected || ink[expected] > ink[edge],
                            "Rotation icon shows the wrong physical bezel edge");
            }
        }
    }
    {
        QWidget host;
        host.resize(1072, 1448);
        host.show();
        kobalt::LinkPanel note(&host);
        kobalt::LinkDetails details;
        details.kind = kobalt::LinkKind::Footnote;
        details.target = "passage";
        details.chapter = "Notes:";
        details.html = "<p>Short note.</p>";
        const QRect readingArea(0, 80, 1072, 1288);
        QString destination;
        note.navigate = [&](const QString &target) {
            destination = target;
        };
        note.open(details, QRect(80, 100, 150, 40), readingArea);
        QApplication::processEvents();
        auto *text = note.findChild<kobalt::ui::PagedText *>("rrLinkText");
        auto *open = note.findChild<QPushButton *>("rrLinkOpen");
        auto *next = note.findChild<QPushButton *>("rrLinkNext");
        require(readingArea.contains(note.geometry()), "Lower note overlaps the reading footer");
        require(note.y() > 500 && text->pages() == 1 && !next->isEnabled(),
                "Short note geometry or paging is incorrect");
        require(destination.isEmpty(), "Opening a note navigated without choosing Show in book");
        auto *row = static_cast<kobalt::ui::CaptionButton *>(open);
        require(row->text() == kobalt::strings::get("link_panel.show_in_book") &&
                    row->caption() == kobalt::strings::get("link_panel.in_chapter").arg("Notes"),
                "Show in book does not name the note's chapter");
        require(open->height() == kobalt::ui::metrics::links::returnHeight, "Show in book is not a return-row height");
        {
            // The row closes a lower sheet with a divider where it meets the footer strip.
            QImage image(note.size(), QImage::Format_ARGB32);
            note.render(&image);
            const QColor edge = QColor::fromRgba(image.pixel(note.width() / 2, open->geometry().bottom()));
            require(edge == kobalt::ui::dividerColor(&note), "Show in book lacks its bottom divider");
        }
        open->click();
        require(destination == details.target, "Show in book lost its destination");
        details.html =
            QString("<p>A long paragraph with enough text to wrap into several lines at the reader UI size.</p>")
                .repeated(80);
        note.open(details, QRect(80, 1100, 150, 40), readingArea);
        QApplication::processEvents();
        require(readingArea.contains(note.geometry()), "Upper note overlaps the reading header");
        require(note.y() < 120 && text->pages() > 1 && next->isEnabled(),
                "Long note did not paginate above its reference");
        next->click();
        require(text->page() == 1, "Note Next did not advance");
        note.findChild<QPushButton *>("rrLinkPrevious")->click();
        require(text->page() == 0, "Note Previous did not return");
        details.target.clear();
        details.chapter.clear();
        details.html = "<p>A hidden note.</p>";
        note.open(details, {});
        QApplication::processEvents();
        require(!open->isVisible() && text->page() == 0 && text->pages() == 1,
                "Hidden note retained a target or old pagination");
        QWidget browserHost;
        browserHost.resize(1264, 1680);
        browserHost.show();
        kobalt::WebsiteDialog website(&browserHost);
        const QString address = "https://example.org/";
        website.open(address);
        QApplication::processEvents();
        auto *confirm = website.findChild<QPushButton *>("rrLinkOpen");
        require(confirm && !confirm->isEnabled(), "Unavailable browser did not disable confirmation");
        auto *decline = website.findChild<QPushButton *>("rrLinkNo");
        require(decline && decline->isVisible(), "External website has no No action");
        int browserCalls = 0, dismissals = 0;
        website.browserAvailable = [] {
            return true;
        };
        website.openBrowser = [&](const QUrl &url) {
            ++browserCalls;
            return url == QUrl(address);
        };
        website.dismissed = [&] {
            ++dismissals;
        };
        website.open(address);
        QApplication::processEvents();
        require(confirm->isEnabled() && browserCalls == 0, "Browser opened before confirmation");
        decline->click();
        require(browserCalls == 0 && dismissals == 1, "No opened the browser");
        confirm->click();
        require(browserCalls == 1 && dismissals == 2, "Yes did not open and dismiss the link");
    }
    for (bool dark : {false, true}) {
        QWidget host;
        host.resize(1072, 1448);
        host.setProperty("reader_dark", dark);
        QPalette palette = host.palette();
        palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        host.setPalette(palette);
        host.show();
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        kobalt::ChapterContents choices(&host);
        choices.resize(host.size());
        QVector<kobalt::Choice> chapters;
        for (int i = 0; i < 35; ++i)
            chapters.append({QString("Chapter %1").arg(i + 1),
                             QString::number(i),
                             QString("chapter%1").arg(i),
                             {},
                             i == 1 ? 2 : 1,
                             i * 10});
        chapters[2].value.clear();
        choices.showChapters("Book <title>", chapters, "chapter34");
        QApplication::processEvents();
        auto *contents = &choices;
        auto *previous = choices.findChild<QPushButton *>("rrContentsPrevious");
        require(choices.currentPage() == 2 && choices.findChild<QPushButton *>("rrContentsNext")->isEnabled(),
                "Contents did not open on the current chapter's page");
        auto *current = contents->findChild<QLabel *>("chapter34Title");
        require(current && !current->font().bold(), "Current chapter uses a bold label");
        require(contents->findChild<QLabel *>("chapter34Number")->text() == "35" &&
                    contents->findChild<QLabel *>("chapter34Page")->text() == "341",
                "Current chapter lost its number or start page");
        auto *currentButton = contents->findChild<QPushButton *>("chapter34");
        const auto selectedPaper = currentButton->grab().toImage().pixel(4, 4);
        require(qGray(selectedPaper) == (dark ? 255 : 0), "Current chapter is not filled with ink");
        currentButton->setDown(true);
        QApplication::processEvents();
        require(currentButton->grab().toImage().pixel(4, 4) == selectedPaper,
                "Pressing the current chapter washes out its selected background");
        currentButton->setDown(false);
        previous->click();
        previous->click();
        QApplication::processEvents();
        require(choices.currentPage() == 0 && previous->isEnabled(), "Contents pager did not change the visible page");
        auto *parent = contents->findChild<QLabel *>("chapter0Title");
        auto *nested = contents->findChild<QLabel *>("chapter1Title");
        require(parent && nested &&
                    parent->x() == contents->findChild<QLabel *>("chapter0Number")->geometry().right() + 1 &&
                    nested->x() > parent->x(),
                "Contents lost chapter depth or indented the root level");
        require(contents->findChild<QLabel *>("chapter0Number")->text() == "01", "Chapter numbers are not zero-padded");
        require(!contents->findChild<QPushButton *>("chapter2")->isEnabled(), "Unresolved chapter remained navigable");
        auto *row = contents->findChild<QPushButton *>("chapter1");
        row->setFocus(Qt::MouseFocusReason);
        row->setDown(true);
        QApplication::processEvents();
        const int highlight = dark ? 51 : 221;
        const auto pressed = row->grab().toImage();
        require(qGray(pressed.pixel(4, 4)) == highlight && qGray(pressed.pixel(nested->x() + 2, 3)) == highlight,
                "Pressed chapter row does not show a full gray highlight");
        for (int x = 1; x < pressed.width() - 1; ++x)
            require(qGray(pressed.pixel(x, 1)) == highlight, "Chapter highlight has an inset focus outline");
        row->setDown(false);
        require(qGray(row->grab().toImage().pixel(4, 4)) == (dark ? 0 : 255),
                "Released chapter row retains a focus highlight");
        row->clearFocus();
        row->setAttribute(Qt::WA_UnderMouse, true);
        QMouseEvent enter(QEvent::MouseMove, QPointF(4, 4), row->mapToGlobal(QPoint(4, 4)), Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(row, &enter);
        require(qGray(row->grab().toImage().pixel(4, 4)) == (dark ? 0 : 255), "Chapter row highlights on hover");
        row->setAttribute(Qt::WA_UnderMouse, false);
        QMouseEvent leave(QEvent::MouseMove, QPointF(-1, -1), row->mapToGlobal(QPoint(-1, -1)), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(row, &leave);
        require(qGray(row->grab().toImage().pixel(4, 4)) == (dark ? 0 : 255),
                "Chapter row retained its highlight after leaving");
        auto *disabled = contents->findChild<QPushButton *>("chapter2");
        disabled->setAttribute(Qt::WA_UnderMouse, true);
        QApplication::sendEvent(disabled, &enter);
        require(qGray(disabled->grab().toImage().pixel(4, 4)) == (dark ? 0 : 255),
                "Unavailable chapter shows a hover highlight");

        QString target;
        choices.selected = [&](const QString &value) {
            target = value;
        };
        contents->findChild<QPushButton *>("chapter1")->click();
        require(target == "1", "Contents paging selected a stale destination");
        bool closed = false;
        choices.back = [&] {
            closed = true;
        };
        auto *close = choices.findChild<QPushButton *>("rrContentsClose");
        close->click();
        require(closed, "Contents close did not return to the reader");
        require(close->geometry().topLeft() == QPoint() && close->width() == kobalt::ui::metrics::chrome::cellWidth,
                "The Chapters bar lost its close cell");
        auto *next = choices.findChild<QPushButton *>("rrContentsNext");
        previous->click();
        require(choices.currentPage() == 2, "Previous did not wrap to the final contents page");
        next->click();
        require(choices.currentPage() == 0, "Next did not wrap to the first contents page");
        previous->click();
        require(currentButton->grab().toImage().pixel(4, 4) == selectedPaper,
                "Wrapping changed the selected chapter highlight");
        choices.resize(1448, 1072);
        QApplication::processEvents();
        const auto count = choices.findChild<QLabel *>("rrContentsPage")->text().split(" of ");
        require(count.size() == 2 && count[0].toInt() == choices.currentPage() + 1 &&
                    count[0].toInt() <= count[1].toInt(),
                "Contents resize left the current page outside its range");
        choices.showChapters("Short book", chapters.mid(0, 3), "chapter0");
        require(!next->isVisibleTo(&choices), "A single page of contents shows a pager");
        choices.showChapters("Empty book", {}, {});
        require(contents->findChild<QLabel *>("rrContentsEmpty")->isVisible(),
                "Empty contents retained stale chapter rows");
        for (auto *button : contents->findChildren<QPushButton *>())
            require(!button->objectName().startsWith("chapter") || !button->isVisible(),
                    "Empty contents retained a navigable chapter");
    }
    std::puts("PASS: chapter row highlights, paging and navigation in light and dark themes");
    {
        namespace g = kobalt::ui::metrics::chapters;
        namespace c = kobalt::ui::metrics::chrome;
        using kobalt::strings::get;
        QWidget host;
        host.resize(1072, 1448);
        // Match the font inherited from the production Reader window.
        host.setFont(kobalt::ui::interfaceFont());
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        kobalt::ChapterGraph graph(&host);
        graph.resize(1072, g::graphHeight);
        host.show();
        graph.open();
        QApplication::processEvents();
        auto *message = graph.findChild<QLabel *>("rrGraphMessage");
        auto *bars = graph.findChild<QWidget *>("rrChapterBars");
        auto *bookCell = graph.findChild<QWidget *>("rrGraphBookCell");
        auto text = [&](const char *name) {
            return graph.findChild<QLabel *>(name)->text();
        };
        graph.setData({}, {}, false, {});
        require(message->isVisible() && message->text() == get("stats.counting") && !bars->isVisible(),
                "Pending counts appeared as an empty graph");
        require(bookCell->isVisible(), "Page figures waited for word counts");
        graph.setData({}, {}, false, "Native count failed");
        require(message->text() == get("chapter_graph.chapter_lengths_are_unavailable") && bookCell->isVisible(),
                "Failed counts remained in the loading state");
        QVector<kobalt::ChapterLink> chapters{{"Short", "a", 0, 0, 100}, {"Long", "b", 1, 0, 400}};
        QPalette colors = host.palette();
        colors.setColor(QPalette::Window, Qt::white);
        colors.setColor(QPalette::WindowText, Qt::black);
        colors.setColor(QPalette::Mid, QColor(128, 128, 128));
        graph.setPalette(colors);
        graph.setData(chapters, {}, true, {});
        QApplication::processEvents();
        const auto image = bars->grab().toImage();
        const int chartHeight = g::barsHeight, barWidth = g::maximumBarWidth;
        const int groupLeft = (image.width() - 2 * barWidth - g::gapWide) / 2;
        require(qGray(image.pixel(groupLeft, chartHeight / 2)) == 255 &&
                    qGray(image.pixel(image.width() - groupLeft - barWidth, chartHeight / 2)) == 0,
                "Graph heights do not distinguish native word counts from equal page lengths");
        require(qGray(image.pixel(groupLeft - 1, chartHeight - 4)) == 255 &&
                    qGray(image.pixel(groupLeft, chartHeight - 4)) == 0 &&
                    qGray(image.pixel(image.width() - groupLeft, chartHeight - 4)) == 255,
                "Short-book bars exceed their width cap or are not centered");
        require(!graph.findChild<QPushButton *>("rrGraphNext")->isVisible(), "A short graph exposed paging controls");
        // One scale for the whole book, capped so that one outlier does not flatten the rest.
        QVector<kobalt::ChapterLink> even;
        for (int i = 0; i < 21; ++i)
            even.append({QString::number(i), QString::number(i), i, 0, 1000});
        even.last().words = 10000000;
        graph.setData(even, {}, true, {});
        QApplication::processEvents();
        require(graph.graphPages() == 1, "Twenty-one chapters did not fit on one dense chart");
        const auto capped = bars->grab().toImage();
        const int evenWidth = (capped.width() - 20 * g::gapWide) / 21,
                  evenLeft = (capped.width() - 21 * evenWidth - 20 * g::gapWide) / 2;
        const int outlierLeft = evenLeft + 20 * (evenWidth + g::gapWide);
        require(qGray(capped.pixel(evenLeft, chartHeight - chartHeight / g::scaleCap + 2)) == 0,
                "An outlier flattened the other chapters");
        // A capped chapter reaches the top of the chart with an unbroken outline.
        for (int y = 0; y < chartHeight; ++y)
            require(qGray(capped.pixel(outlierLeft, y)) == 0, "A capped chapter lacks its full height or is marked");
        for (int i = 21; i < 80; ++i)
            even.append({QString::number(i), QString::number(i), i, 0, 1000});
        graph.setData(even, {}, true, {});
        require(graph.graphPages() == 1 && !graph.findChild<QPushButton *>("rrGraphNext")->isVisible(),
                "Eighty chapters did not fit on one dense chart");
        for (int i = 2; i < 180; ++i)
            chapters.append({QString::number(i), QString::number(i), i, 0, i});
        graph.setData(chapters, {120, 121, 2, 8}, true, {});
        graph.open();
        require(graph.graphPage() == 1 && graph.graphPages() == 2,
                "A long dense graph did not page with narrow bars or open at the current chapter");
        auto *next = graph.findChild<QPushButton *>("rrGraphNext");
        require(next->isVisible(), "A paged graph hides its pager");
        graph.findChild<QPushButton *>("rrGraphPrevious")->click();
        while (next->isEnabled())
            next->click();
        require(graph.graphPage() == graph.graphPages() - 1, "Graph paging skipped its final entries");
        graph.resize(1448, g::graphHeight);
        QApplication::processEvents();
        require(graph.graphPage() < graph.graphPages(), "Resizing left the graph outside its page range");
        graph.resize(1072, g::graphHeight);
        // The sparse chart pages twenty chapters at a time.
        chapters.resize(21);
        graph.setDense(false);
        graph.setData(chapters, {20, -1, 1, 1}, true, {});
        graph.open();
        require(graph.graphPage() == 1 && graph.graphPages() == 2, "The 21st chapter did not get its own sparse page");
        require(bars->accessibleName() == get("chapter_graph.chapters_to_of").arg(21).arg(21).arg(21),
                "Final graph page has the wrong accessible chapter range");
        QApplication::processEvents();
        const auto lastPage = bars->grab().toImage();
        int filled = 0;
        for (int x = 0; x < lastPage.width(); ++x)
            if (qGray(lastPage.pixel(x, chartHeight - 2)) < 255)
                ++filled;
        require(filled == barWidth, "A single final chapter expanded beyond the bar width cap");
        graph.setDense(true);
        require(graph.graphPages() == 1, "The dense chart kept the sparse pages");
        const QString longTitle =
            "A chapter with a very long name that needs more than one line of the Stats panel to display completely";
        graph.setProgress(longTitle, 4, 10, 12, 25, 100);
        QApplication::processEvents();
        auto *title = graph.findChild<QLabel *>("rrGraphChapterTitle");
        require(title->text().endsWith(QChar(0x2026)) && title->text().size() < longTitle.size() &&
                    title->height() < 2 * title->fontMetrics().lineSpacing(),
                "A long chapter title did not elide on one line");
        graph.setProgress("The Second Voyage", 4, 10, 12, 25, 100);
        QApplication::processEvents();
        PaintCountingObserver graphPaints, barPaints, titlePaints;
        graph.installEventFilter(&graphPaints);
        bars->installEventFilter(&barPaints);
        title->installEventFilter(&titlePaints);
        graph.setData(chapters, {20, -1, 1, 1}, true, {});
        graph.setProgress("The Second Voyage", 4, 10, 12, 25, 100);
        QApplication::processEvents();
        require(graphPaints.paints == 0 && barPaints.paints == 0 && titlePaints.paints == 0,
                "Repeated Stats data and progress repainted the graph");
        auto changedChapters = chapters;
        ++changedChapters[20].words;
        graph.setData(changedChapters, {20, -1, 1, 1}, true, {});
        QApplication::processEvents();
        require(barPaints.paints > 0, "Changed chapter word count did not repaint the bars");
        graph.setData(chapters, {20, -1, 1, 1}, true, {});
        require(title->text() == "The Second Voyage", "The chapter title is missing");
        require(text("rrGraphChapterPercent") == get("common.percent").arg(30) && text("rrGraphChapterFigure") == "7" &&
                    text("rrGraphChapterUnit") == get("stats.pages_left"),
                "Chapter progress credited the current unread page");
        require(text("rrGraphChapterPosition") == get("stats.page_of").arg(4).arg(10),
                "Chapter page position is missing");
        require(text("rrGraphNextChapter") == get("chapter_graph.next_chapter").arg(12).arg(get("common.pages")),
                "Next chapter length is missing");
        require(text("rrGraphBookPercent") == get("common.percent").arg(25),
                "Book progress disagrees with the current page");
        require(text("rrGraphBookFigure") == "75", "Book pages remaining omit the displayed page");
        graph.setChapterNumber(13, 24);
        graph.setReading(23, 35706);
        require(text("rrGraphChapterNumber") == get("stats.contents_position").arg(13).arg(24) &&
                    text("rrGraphSessionFigure") == "23" &&
                    text("rrGraphBookDetail") == get("stats.words").arg("35,706"),
                "Chapter number, session or word count is missing");
        for (int width : {758, 1072, 1448}) {
            graph.resize(width, g::graphHeight);
            graph.setProgress("Large book", 1, 3500, 3500, 1, 10000);
            QApplication::processEvents();
            for (const auto *name : {"rrGraphChapterCell", "rrGraphBookCell", "rrGraphSessionCell"}) {
                const auto *cell = graph.findChild<QWidget *>(name);
                const auto *figure = cell->findChildren<QLabel *>().at(2), *unit = cell->findChildren<QLabel *>().at(3),
                           *detail = cell->findChildren<QLabel *>().at(4);
                require(figure->width() >= kobalt::qt::textWidth(figure->fontMetrics(), figure->text()),
                        "Stats clips a large figure");
                require(unit->geometry().right() < cell->width() - c::inset + 1, "A figure unit runs past its cell");
                // The same padding sits above the caption and below the last line.
                require(cell->findChildren<QLabel *>().at(0)->y() == g::cellPadding &&
                            detail->geometry().bottom() + 1 + g::cellPadding <= cell->height(),
                        "A Stats cell lost its padding");
            }
        }
        // Times at the session's pace fit on one line from the Clara up, and
        // wrap evenly on narrower screens instead of shortening.
        graph.setReading(23, 35706, 45 * 60000, (12 * 60 + 45) * 60000);
        for (int width : {758, 1072, 1264}) {
            graph.resize(width, g::graphHeight);
            QApplication::processEvents();
            for (const auto *name : {"rrGraphChapterCell", "rrGraphBookCell", "rrGraphSessionCell"}) {
                const auto *detail = graph.findChild<QWidget *>(name)->findChildren<QLabel *>().at(4);
                const auto lines = detail->text().split('\n');
                require(!detail->text().contains(QChar(0x2026)) && lines.size() <= g::detailLines &&
                            (width < 1072 || lines.size() == 1),
                        "A Stats detail was cut off or wrapped on a wide screen");
                for (const auto &line : lines)
                    require(kobalt::qt::textWidth(detail->fontMetrics(), line) <= detail->width(),
                            "A Stats detail line is wider than its cell");
            }
        }
        require(text("rrGraphBookDetail") == get("stats.time_left").arg(get("stats.duration_hours").arg(12).arg(45)),
                "Book time left is missing");
        // On the Nia the hours use a clock format instead of wrapping.
        graph.setReading(23, 35706, 45 * 60000, (12 * 60 + 5) * 60000);
        graph.resize(758, g::graphHeight);
        QApplication::processEvents();
        require(text("rrGraphBookDetail") ==
                        get("stats.time_left").arg(get("stats.duration_clock").arg(12).arg("05")) &&
                    text("rrGraphNextChapter") == get("stats.time_left").arg(get("stats.duration_minutes").arg(45)),
                "A narrow cell did not use the clock format for hours only");
        graph.setReading(23, 35706);
        graph.resize(1072, g::graphHeight);
        // At its own density a Nia has the Clara's physical layout, so every
        // detail fits on one line, as it does on the Clara.
        for (const int ppi : {212, 227}) {
            kobalt::ui::setDensity(ppi);
            kobalt::ChapterGraph scaled(&host);
            scaled.resize(ppi == 212 ? 758 : 1404, g::graphHeight);
            scaled.setData(chapters, {20, -1, 1, 1}, true, {});
            scaled.setProgress("The Second Voyage", 4, 10, 12, 25, 100);
            scaled.setReading(23, 35706, 45 * 60000, (12 * 60 + 45) * 60000);
            scaled.open();
            QApplication::processEvents();
            require(scaled.height() >= int(g::graphHeight) && int(g::cellHeight) == kobalt::ui::px(253),
                    "Stats did not scale its height");
            for (const auto *name : {"rrGraphChapterCell", "rrGraphBookCell", "rrGraphSessionCell"}) {
                const auto *cell = scaled.findChild<QWidget *>(name);
                const auto labels = cell->findChildren<QLabel *>();
                require(cell->height() == int(g::cellHeight) && labels.at(0)->y() == int(g::cellPadding),
                        "A scaled Stats cell lost its size or padding");
                require(!labels.at(4)->text().contains('\n') && !labels.at(4)->text().contains(QChar(0x2026)) &&
                            kobalt::qt::textWidth(labels.at(4)->fontMetrics(), labels.at(4)->text()) <=
                                labels.at(4)->width(),
                        "A Stats detail did not fit one line at its own density");
            }
        }
        kobalt::ui::setDensity(300);
        graph.setProgress("Last chapter", 1, 1, 0, 0, 1);
        require(text("rrGraphNextChapter") == get("stats.last_chapter"), "Last chapter still names a next chapter");
        // Without shared counts, Stats charts chapters by their length in pages.
        const auto lengths = kobalt::chapterPageLengths(
            {{"One", "a", 0}, {"Two", "b", 4}, {"Part", "c", 4}, {"Lost", "d", -1}, {"End", "e", 9}}, 12);
        require(lengths.size() == 5 && lengths[0].words == 4 && lengths[1].words == 5 && lengths[2].words == 5 &&
                    lengths[3].words == 0 && lengths[4].words == 3,
                "Page lengths did not follow the chapter starts");
        require(kobalt::chapterPageLengths({{"Lost", "a", -1}}, 12).isEmpty(), "Chapters without pages gave a chart");
        graph.setData({}, {}, true, {});
        require(message->isVisible() &&
                    message->text() == kobalt::strings::get("chapter_graph.no_chapter_lengths_available") &&
                    !bars->isVisible(),
                "Empty completed counts appeared as loading or stale bars");
    }
    {
        using namespace kobalt;
        using namespace kobalt::ui;
        QWidget host;
        host.resize(1072, 1448);
        host.show();
        ChoiceScreen picker(&host);
        QVector<Choice> choices;
        for (int i = 0; i < 25; ++i)
            choices.append({QString("Font %1").arg(i), QString("Family %1").arg(i), QString("font%1").arg(i)});
        QVector<QStringList> requestedPages;
        picker.fontPageRequested = [&](const QStringList &families) {
            requestedPages.append(families);
        };
        picker.setDropdownGeometry(QRect(0, 200, 1072, picker.fontPanelHeight()));
        picker.showChoices("Font", choices, {}, ChoiceScreen::Mode::Fonts);
        require(requestedPages.size() == 1, "Opening the font picker did not request its visible page once");
        auto *next = picker.findChild<QPushButton *>("rrFontPagerNext");
        auto *previous = picker.findChild<QPushButton *>("rrFontPagerPrevious");
        require(next && previous, "Font picker lacks paging controls");
        for (int page = 0; page < 3; ++page) {
            app.processEvents();
            QStringList expectedFamilies;
            for (int i = page * 12; i < qMin((page + 1) * 12, int(choices.size())); ++i)
                expectedFamilies.append(choices[i].value);
            require(requestedPages.size() == page + 1 && requestedPages.last() == expectedFamilies,
                    "Font picker requested families outside the visible page");
            int visible = 0;
            QVector<QRect> rectangles;
            for (int i = 0; i < choices.size(); ++i) {
                auto *button = picker.findChild<QPushButton *>(choices[i].name);
                if (!button || !button->isVisible())
                    continue;
                require((i / 12) == page, "Grid skipped or repeated a font");
                require(picker.rect().contains(button->geometry()), "Grid cell escaped the panel");
                for (const auto &rect : rectangles)
                    require(!rect.intersects(button->geometry()), "Grid cells overlap");
                rectangles.append(button->geometry());
                ++visible;
            }
            require(visible == (page < 2 ? 12 : 1), "Grid page capacity is wrong");
            require(next->isEnabled(), "Grid last-page boundary is wrong");
            if (page < 2)
                next->click();
        }
        for (int turn = 0; turn < 2; ++turn)
            previous->click();
        require(requestedPages.size() == 5 && requestedPages[3] == requestedPages[1] &&
                    requestedPages[4] == requestedPages[0],
                "Returning to a font page requested different families");
        previous->click();
        require(requestedPages.last() == QStringList{choices.last().value},
                "Font previous did not request the last page");
        next->click();
        require(requestedPages.last() == requestedPages.first(), "Font next did not request the first page");
        for (const QSize size : {QSize(1072, picker.fontPanelHeight()), QSize(600, 450)}) {
            picker.setFontGeometry(QRect(QPoint(), size));
            app.processEvents();
            for (int i = 0; i < 12; ++i) {
                auto *cell = picker.findChild<QPushButton *>(choices[i].name);
                require(cell && cell->isVisible() && picker.rect().contains(cell->geometry()),
                        "Grid clips a cell after resize");
            }
        }
        require(requestedPages.size() == 7, "Resizing the font picker requested a hidden font page");
        picker.fontPageRequested = {};
        require(picker.findChild<QPushButton *>("font0")->width() == picker.width() / 3 &&
                    picker.findChild<QPushButton *>("font1")->width() == picker.width() / 3,
                "Default slots must each occupy a third of a row");
        bool finished = false;
        picker.back = [&] {
            finished = true;
        };
        auto *done = picker.findChild<QPushButton *>("rrFontClose");
        require(done && done->isVisible(), "Missing close button");
        require(done->text().isEmpty() && done->width() <= metrics::choices::pickerHeader,
                "Close cell is not a compact icon button");
        for (const bool pressed : {false, true}) {
            done->setDown(pressed);
            const auto image = done->grab().toImage();
            const auto background = done->palette().color(pressed ? QPalette::WindowText : QPalette::Window);
            for (int x = image.width() / 4; x < 3 * image.width() / 4; ++x)
                require(QColor::fromRgba(image.pixel(x, image.height() - 1)) == background,
                        "Close button draws a stray bottom rule");
        }
        done->setDown(false);
        done->click();
        require(finished, "Close button did not close the picker");
        const QVector<Choice> sections{{"Libron", "Libron", "recommended", "Recommended Fonts"},
                                       {"Ancizar Serif", "NV Ancizar Serif", "ancizar", "Recommended Fonts"},
                                       {"System", "System", "system", "Built-in Fonts"},
                                       {"Personal", "Personal", "personal", "Sideloaded Fonts"},
                                       {"Arabic", "Noto Naskh Arabic", "arabic", "Supplemental Fonts"}};
        picker.setFontGeometry(QRect(0, 0, 1072, picker.fontPanelHeight()));
        picker.showChoices("Font", sections, "NV Ancizar Serif", ChoiceScreen::Mode::Fonts);
        auto *section = picker.findChild<QLabel *>("rrFontSection");
        require(section && section->text() == "RECOMMENDED FONTS", "Font section heading is missing");
        require(picker.findChild<QPushButton *>("ancizar")->isChecked(), "Display name changed the stored selection");
        const auto selectedCell = picker.findChild<QPushButton *>("ancizar")->grab().toImage();
        int markerRows = 0;
        for (int y = selectedCell.height() - ui::px(44); y < selectedCell.height() - ui::px(12); ++y) {
            bool visibleInk = false;
            for (int x = selectedCell.width() - ui::px(44); x < selectedCell.width() - ui::px(12); ++x)
                visibleInk |= QColor::fromRgba(selectedCell.pixel(x, y)) == picker.palette().color(QPalette::Window);
            if (visibleInk)
                ++markerRows;
        }
        require(markerRows >= ui::px(10), "Font label covers the active-font checkmark");
        QString chosen;
        picker.selected = [&](const QString &value) {
            chosen = value;
        };
        picker.findChild<QPushButton *>("ancizar")->click();
        require(chosen == "NV Ancizar Serif" && fontDisplayName(chosen) == "Ancizar Serif",
                "Picker changed the font identifier");
        for (int i = 2; i < sections.size(); ++i) {
            next->click();
            app.processEvents();
            require(section->text() == sections[i].section.toUpper(), "Pager skipped a font section");
            auto *cell = picker.findChild<QPushButton *>(sections[i].name);
            require(cell && cell->isVisible(), "Section page lost its first font");
            cell->click();
            require(chosen == sections[i].value, "Section page chose a font from another page");
        }
        require(next->isEnabled(), "Supplemental page cannot wrap to the first section");
        const auto sparse = picker.grab().toImage();
        const auto paper = picker.palette().color(QPalette::Window);
        const auto ink = picker.palette().color(QPalette::WindowText);
        for (int y = 0; y < metrics::chrome::heavyRule; ++y)
            for (int x = 0; x < sparse.width(); ++x)
                require(QColor::fromRgba(sparse.pixel(x, y)) == ink, "Font panel top rule has a gap");
        const int emptyY = picker.height() / 2;
        for (const int x : {0, sparse.width() - 1})
            require(QColor::fromRgba(sparse.pixel(x, emptyY)) == paper, "Sparse font page has an outer side border");
        require(QColor::fromRgba(sparse.pixel(sparse.width() / 2, metrics::choices::pickerHeader - 1)) == paper,
                "Font header has an unwanted bottom rule");
        choices[3].label = "Atkinson Hyperlegible Next";
        choices[3].value = "Libron";
        picker.showChoices("Font", choices, {}, ChoiceScreen::Mode::Fonts);
        auto *longName = picker.findChild<SampleLabel *>("font3Title");
        require(longName && longName->isVisible(), "Long font name is missing from the grid");
        const auto namePixels = longName->parentWidget()->grab().toImage().copy(longName->geometry());
        int nameBands = 0;
        bool previousInk = false;
        for (int y = 0; y < namePixels.height(); ++y) {
            bool ink = false;
            for (int x = 0; x < namePixels.width(); ++x)
                ink |= qGray(namePixels.pixel(x, y)) < 128;
            if (ink && !previousInk)
                ++nameBands;
            previousInk = ink;
        }
        require(nameBands == 2, "The font grid leaves insufficient height for a two-line name");
        choices[2].placeholder = true;
        picker.showChoices("Font", choices, {}, ChoiceScreen::Mode::Fonts);
        require(!picker.findChild<QLabel *>("font2Title")->isVisible(), "Placeholder label covers its circle");
        picker.showChoices(
            "Header size",
            {{"Off", "off", "sizeOff"}, {"Small", "small", "sizeSmall"}, {"Large", "large", "sizeLarge"}}, "small",
            ChoiceScreen::Mode::Sizes);
        require(picker.findChild<QLabel *>("sizeLargeTitle")->isVisible(), "Reused placeholder cell hid a size label");
    }
    {
        using namespace kobalt;
        using namespace kobalt::ui;
        QWidget host;
        host.resize(1072, 1448);
        host.show();
        ChoiceScreen picker(&host);
        picker.setGeometry(20, 20, 600, 4 * metrics::choices::fontRowHeight);
        const QVector<Choice> choices{{"Off", "off", "headerOff"},
                                      {"Small", "small", "headerSmall"},
                                      {"Medium", "medium", "headerMedium"},
                                      {"Large", "large", "headerLarge"}};
        picker.showChoices("Header size", choices, "large", ChoiceScreen::Mode::Sizes);
        app.processEvents();
        // The size dropdown has a two-pixel outline above its first choice.
        int bottom = 2;
        for (const auto &choice : choices) {
            auto *button = picker.findChild<QPushButton *>(choice.name);
            require(button && button->isVisible(), "Size selector hid an available choice");
            require(button->geometry().top() == bottom, "Size selector left a gap between choices");
            bottom = button->geometry().bottom() + 1;
        }
        require(picker.findChild<QPushButton *>("headerLarge")->isChecked(), "Size selector lost its selected value");
        auto *current = picker.findChild<ListRow *>("headerLarge");
        require(current && current->titleLabel()->font().pixelSize() == int(metrics::text::body),
                "Size selector text is smaller than its control");
        const auto selectedImage = current->grab().toImage();
        current->setAccessory(ListRow::None{});
        require(current->grab().toImage() != selectedImage, "Size selector has no checkmark on its current choice");
        require(!picker.findChild<QPushButton *>("rrChoicesNext")->isVisible() &&
                    !picker.findChild<QPushButton *>("rrChoicesPrevious")->isVisible(),
                "Size selector exposed paging controls");
    }
    // Compare visible mixed-case ink across the bundled families, independent
    // of their claimed font metrics. Use the same sample sizing as the picker.
    QStringList readingFamilies;
    for (const auto &file : QDir(kobalt::test::readerAssets().file("fonts/ebook-core")).entryList({"*.ttf"})) {
        const auto id = QFontDatabase::addApplicationFont(kobalt::test::readerFontCopies().file(
            "ebook-core/" + file, kobalt::test::readerAssets().fontData("ebook-core/" + file)));
        require(id >= 0, "Core font could not be registered for optical check");
        readingFamilies.append(QFontDatabase::applicationFontFamilies(id));
    }
    readingFamilies.removeDuplicates();
    for (const auto &face : readingFamilies) {
        const auto font = kobalt::ui::fontSample(face);
        require(QFontInfo(font).family() == face, "Optical font check used a fallback");
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.setFont(font);
            painter.setPen(Qt::black);
            painter.drawText(QPoint(10, 70), "Hox");
        }
        int top = 100, bottom = -1;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qGray(image.pixel(x, y)) < 128) {
                    top = qMin(top, y);
                    bottom = y;
                    break;
                }
        require(qAbs(bottom - top + 1 - kobalt::ui::metrics::choices::sampleInkHeight) <= 2,
                "Font preview has inconsistent visible size");
    }
    const auto nativeFont = QApplication::font();
    // Regular and Bold share their legacy family names on Qt 5 and Qt 6.
    // The interface registers renamed copies, apart from reading fonts.
    const QString nativeFamily = "Kobalt UI Sans";
    const QString chromeFamily = "Kobalt UI Mono";
    require(kobalt::ui::interfaceFontFamily() == nativeFamily, "The interface did not register Plex Sans");
    require(QFontInfo(kobalt::ui::interfaceFont()).family() == nativeFamily, "The interface resolved another font");
    require(QFontInfo(kobalt::ui::chromeFont()).family() == chromeFamily, "Chrome did not register Plex Mono");
    require(QFontInfo(kobalt::ui::interfaceFont()).weight() == QFont::Normal, "Controls did not resolve Plex Regular");
    require(QFontInfo(kobalt::ui::chromeFont()).weight() == QFont::Normal, "Chrome did not resolve Plex Regular");
    auto boldUi = kobalt::ui::interfaceFont();
    boldUi.setBold(true);
    require(QFontInfo(boldUi).family() == nativeFamily, "Bold interface text resolved another family");
    require(QFontInfo(boldUi).weight() == QFont::Bold, "Bold interface text did not resolve Plex Bold");
    require(QApplication::font() == nativeFont, "The interface changed the native application font");

    // Heavier interface text replaces the faces while the widgets are shown.
    // Layout stays the same, and switching back draws the same pixels.
    {
        using namespace kobalt::ui;
        QWidget host;
        host.setFont(interfaceFont());
        // As the reader window does, so style sheet fonts stay unhinted on Qt 5.2.
        keepInterfaceUnhinted(&host);
        readerStyle(&host, styles::reader());
        auto *plain = new QLabel("Swipe along edges to adjust frontlight", &host);
        plain->setGeometry(10, 10, 880, 60);
        auto *rich = new QLabel("<b>Chapter</b> 12 of 40", &host);
        rich->setTextFormat(Qt::RichText);
        rich->setGeometry(10, 80, 880, 60);
        auto *caption = new QLabel("12 pages left", &host);
        caption->setFont(captionFont());
        readerStyle(caption, styles::caption());
        caption->setGeometry(10, 150, 880, 60);
        auto *brand = new QLabel("Kobalt", &host);
        readerStyle(brand, styles::brand(48));
        brand->setGeometry(10, 220, 880, 90);
        // A label's own style sheet keeps the font of its rules.
        auto *styled = new QLabel("12 pages left", &host);
        readerStyle(styled, styles::caption());
        styled->setGeometry(10, 150, 880, 60);
        auto *explicitFont = new QLabel("12 pages left", &host);
        explicitFont->setFont(chromeFont(22));
        explicitFont->setGeometry(10, 150, 880, 60);
        // Dictionary and link panels keep a laid-out document.
        auto *paged = new PagedText(&host);
        paged->setTextFont(interfaceFont(28));
        paged->setHtml("<p>Swipe along edges to adjust frontlight</p>");
        paged->setGeometry(10, 150, 880, 120);
        const QVector<QWidget *> labels{plain, rich, caption, brand, styled, explicitFont, paged};
        host.resize(900, 320);
        host.show();
        app.processEvents();
        // Render on white: captions have a transparent background.
        const auto ink = [](QWidget *widget) {
            QImage image(widget->size(), QImage::Format_RGB32);
            image.fill(Qt::white);
            widget->render(&image);
            double sum = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    sum += 255 - qGray(image.pixel(x, y));
            return sum / 255;
        };
        const auto measure = [&] {
            QVector<double> values;
            for (auto *widget : labels)
                values.append(ink(widget));
            return values;
        };
        const auto regular = measure();
        const auto hint = plain->sizeHint();
        const auto richHint = rich->sizeHint();
        QVector<QRect> rectangles;
        QVector<QString> families;
        QVector<int> fontSizes;
        for (auto *widget : labels) {
            rectangles.append(widget->geometry());
            families.append(QFontInfo(widget->font()).family());
            fontSizes.append(widget->font().pixelSize());
        }
        const auto fits = [](QWidget *widget) {
            const auto hint = widget->sizeHint();
            return hint.width() <= widget->width() && hint.height() <= widget->height();
        };
        require(fits(plain) && fits(rich), "Regular text does not fit its widget");
        const auto regularPixels = plain->grab().toImage();
        require(interfaceWeight() == 0, "Regular font fixture changed its weight");
        kobalt::ChapterGraph graph(&host);
        graph.resize(900, kobalt::ui::metrics::chapters::graphHeight);
        graph.setProgress("A chapter with a title that needs layout", 4, 10, 12, 25, 100);
        auto *graphTitle = graph.findChild<QLabel *>("rrGraphChapterTitle");
        for (int round = 0; round < 2; ++round) {
            require(setInterfaceWeight(heavierTextSteps), "Heavier text did not replace the faces");
            require(!setInterfaceWeight(heavierTextSteps), "The same weight replaced the faces again");
            refreshInterfaceText(&host);
            app.processEvents();
            graphTitle->setGeometry(0, 0, 1, 1);
            graph.setProgress("A chapter with a title that needs layout", 4, 10, 12, 25, 100);
            require(graphTitle->width() == graph.width() - 2 * int(metrics::chrome::inset),
                    "Heavier interface text did not lay out unchanged Stats progress");
            require(interfaceWeight() == heavierTextSteps && QFontInfo(interfaceFont()).family() == nativeFamily &&
                        QFontInfo(chromeFont()).family() == chromeFamily,
                    "Heavier text changed the family");
            const auto heavier = measure();
            require(paged->textFont().pixelSize() == 28, "Paged text lost its size when the faces changed");
            for (int index = 0; index < labels.size(); ++index) {
                std::printf("interface weight: widget %d ink %+.1f%%\n", index,
                            100 * (heavier[index] / regular[index] - 1));
                require(heavier[index] > regular[index] * 1.02 && heavier[index] < regular[index] * 1.3,
                        "A widget did not draw moderately heavier text");
            }
            for (int index = 0; index < labels.size(); ++index) {
                require(labels[index]->geometry() == rectangles[index], "Heavier text moved a widget");
                require(QFontInfo(labels[index]->font()).family() == families[index] &&
                            labels[index]->font().pixelSize() == fontSizes[index],
                        "Heavier text changed a widget font family or size");
            }
            require(fits(plain) && fits(rich), "Heavier text does not fit its widget");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            // Qt5 grid fitting can change advances without changing the stored
            // font metrics. font-data-check tests those design advances exactly.
            require(plain->sizeHint().height() == hint.height() && rich->sizeHint().height() == richHint.height(),
                    "Heavier text changed a line height");
#else
            require(plain->sizeHint() == hint && rich->sizeHint() == richHint, "Heavier text changed the layout");
#endif
            require(setInterfaceWeight(0), "Regular text did not replace the faces");
            refreshInterfaceText(&host);
            app.processEvents();
            graphTitle->setGeometry(0, 0, 1, 1);
            graph.setProgress("A chapter with a title that needs layout", 4, 10, 12, 25, 100);
            require(graphTitle->width() == graph.width() - 2 * int(metrics::chrome::inset),
                    "Regular interface text did not lay out unchanged Stats progress");
            require(plain->grab().toImage() == regularPixels && measure() == regular,
                    "Regular text did not return to its pixels");
        }
    }

    require(kobalt::ui::metrics::reading::textPixels({}, false) == 24 &&
                kobalt::ui::metrics::reading::textPixels({}, true) == 26,
            "Default chrome sizes changed");
    require(kobalt::ui::metrics::reading::textPixels("small", false) == 24 &&
                kobalt::ui::metrics::reading::textPixels("medium", false) == 26 &&
                kobalt::ui::metrics::reading::textPixels("large", false) == 28 &&
                kobalt::ui::metrics::reading::textPixels("small", true) == 26 &&
                kobalt::ui::metrics::reading::textPixels("medium", true) == 28 &&
                kobalt::ui::metrics::reading::textPixels("large", true) == 30,
            "Chrome text sizes have the wrong increments");
    require(kobalt::ui::metrics::reading::stripHeight("small") == 48 &&
                kobalt::ui::metrics::reading::stripHeight("medium") == 64 &&
                kobalt::ui::metrics::reading::stripHeight("large") == 80,
            "Chrome strips have the wrong heights");
    require(kobalt::ui::metrics::reading::textSize("invalid") == "small" &&
                kobalt::ui::metrics::reading::textPixels("invalid", false) == 24,
            "Unknown chrome size did not use the default");
    // Exercise the final styled layout, not only the constructor's size hints.
    {
        using namespace kobalt::ui;
        const auto originalStyle = app.styleSheet();
        const int originalDensity = density();
        app.setStyleSheet("QSlider { min-height: 80px; max-height: 80px; }");
        for (const int ppi : {167, 212, 300}) {
            setDensity(ppi);
            QWidget host;
            host.resize(600, metrics::control::stepSize);
            auto *rows = new QVBoxLayout(&host);
            rows->setContentsMargins(0, 0, 0, 0);
            auto *slider =
                sliderRow(&host, rows, "density-slider", "density-value", "Size", "less", "more", 0, 100, 1, {});
            host.show();
            app.processEvents();
            require(slider->height() == int(metrics::control::sliderHeight),
                    "Setting slider inherits Nickel's unscaled height");
            require(slider->parentWidget()->rect().contains(slider->geometry()), "Setting slider leaves its row");
            require(slider->visibleRegion() == QRegion(slider->rect()), "Setting slider handle is clipped by its row");
            host.hide();
        }
        app.setStyleSheet(originalStyle);
        setDensity(originalDensity);
    }
    // A stylesheet repolish previously flattened label-only stepper buttons.
    for (const bool dark : {false, true})
        for (const auto size : {QSize(1072, 1448), QSize(1448, 1072)}) {
            using namespace kobalt::ui;
            QWidget host;
            host.setProperty("reader_dark", dark);
            host.resize(size);
            readerStyle(&host, styles::reader());
            auto *rows = new QVBoxLayout(&host);
            rows->setContentsMargins(metrics::panel::contentMargins);
            rows->setSpacing(metrics::control::rowGap);
            auto *level = sliderRow(&host, rows, "level", "value", "Brightness", "less", "more", 0, 100, 1, {});
            auto *actions = horizontalRow();
            rows->addLayout(actions);
            auto *keep = new Button("Keep settings", &host);
            auto *restore = new Button("Restore defaults", &host);
            readerStyle(keep, styles::outlined());
            readerStyle(restore, styles::primary());
            actions->addWidget(keep);
            actions->addWidget(restore);
            rows->addStretch(1);
            host.show();
            app.processEvents();
            for (int pass = 0; pass < 2; ++pass) {
                readerStyle(&host, styles::reader());
                for (auto *widget : host.findChildren<QWidget *>())
                    if (widget->property("reader_style").isValid())
                        readerStyle(widget, widget->property("reader_style").toString());
                level->hide();
                app.processEvents();
                level->show();
                app.processEvents();
                for (auto *button : {static_cast<StepButton *>(host.findChild<QPushButton *>("less")),
                                     static_cast<StepButton *>(host.findChild<QPushButton *>("more"))}) {
                    require(button->size() == QSize(72, 72), "Styled step button is not square");
                    int clicks = 0;
                    const auto connection = QObject::connect(button, &QPushButton::clicked, [&] {
                        ++clicks;
                    });
                    for (const auto point : {QPoint(2, 2), QPoint(69, 2), QPoint(2, 69), QPoint(69, 69)}) {
                        QMouseEvent press(QEvent::MouseButtonPress, point, button->mapToGlobal(point), Qt::LeftButton,
                                          Qt::LeftButton, Qt::NoModifier);
                        QMouseEvent release(QEvent::MouseButtonRelease, point, button->mapToGlobal(point),
                                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(button, &press);
                        QApplication::sendEvent(button, &release);
                    }
                    QObject::disconnect(connection);
                    require(clicks == 4, "Stepper corners do not accept taps");
                    require(host.rect().contains(QRect(button->mapTo(&host, QPoint()), button->size())),
                            "Step button leaves its panel");
                }
                require(keep->height() == restore->height() && keep->y() == restore->y(),
                        "Paired actions have different heights or alignment");
            }
            host.hide();
        }
    {
        using namespace kobalt::ui;
        BackButton back("Back", nullptr);
        back.setFont(kobalt::ui::interfaceFont());
        back.resize(192, 68);
        // Native styles may supply ButtonText independently of reader text.
        // The custom arrow and caption must both use the reader's ink role.
        QPalette colors = back.palette();
        colors.setColor(QPalette::Window, Qt::white);
        colors.setColor(QPalette::WindowText, Qt::black);
        colors.setColor(QPalette::ButtonText, Qt::white);
        back.setPalette(colors);
        readerStyle(&back, "QPushButton{background:white;border:none;padding:0;}");
        back.show();
        app.processEvents();
        const auto image = back.grab().toImage();
        int captionInk = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = metrics::navigation::labelLeft; x < image.width(); ++x)
                captionInk += qGray(image.pixel(x, y)) < 128;
        require(captionInk > 50, "Back caption follows the native button palette instead of reader ink");
    }
    QWidget optionsHost;
    kobalt::ui::readerStyle(&optionsHost, kobalt::ui::styles::reader());
    kobalt::ui::DescribedOption option("Enable page animations",
                                       "Sweep between text pages. Images and colour keep their usual refresh.",
                                       &optionsHost);
    optionsHost.show();
    option.show();
    app.processEvents();
    for (int width : {900, 1250}) {
        option.resize(width, option.heightForWidth(width));
        const auto reference = option.grab().toImage();
        int firstInk = -1, lastInk = -1;
        for (int y = 0; y < reference.height(); ++y)
            for (int x = 60; x < reference.width(); ++x)
                if (qGray(reference.pixel(x, y)) < 128) {
                    if (firstInk < 0)
                        firstInk = y;
                    lastInk = y;
                }
        require(firstInk >= 4 && lastInk >= 0 && lastInk < reference.height() - 4,
                "Setting description does not fit its requested height");
    }
    kobalt::ui::DescribedOption compactOption("Automatic warmth", {}, &optionsHost);
    compactOption.resize(700, compactOption.heightForWidth(700));
    compactOption.show();
    app.processEvents();
    require(compactOption.heightForWidth(700) == kobalt::ui::metrics::control::stepSize,
            "Caption-only checkbox retained description spacing");
    const QPointF checkboxPoint(compactOption.width() - 8, compactOption.height() / 2);
    QMouseEvent checkboxDown(QEvent::MouseButtonPress, checkboxPoint, checkboxPoint, Qt::LeftButton, Qt::LeftButton,
                             Qt::NoModifier);
    QMouseEvent checkboxUp(QEvent::MouseButtonRelease, checkboxPoint, checkboxPoint, Qt::LeftButton, Qt::NoButton,
                           Qt::NoModifier);
    QApplication::sendEvent(&compactOption, &checkboxDown);
    QApplication::sendEvent(&compactOption, &checkboxUp);
    require(compactOption.isChecked(), "Checkbox rejected a tap in the empty part of its row");
    optionsHost.hide();
    {
        using namespace kobalt::ui;
        const int savedDensity = density();
        for (int ppi : {167, 212, 300}) {
            setDensity(ppi);
            for (bool dark : {false, true}) {
                QWidget host;
                host.setProperty("reader_dark", dark);
                QPalette palette = host.palette();
                palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
                palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
                host.setPalette(palette);
                host.resize(px(900), px(700));
                host.show();
                PaintCountingOption option("A setting", "A description that wraps at narrow widths.", &host);
                int clicks = 0, toggles = 0;
                QObject::connect(&option, &QCheckBox::clicked, &host, [&] {
                    ++clicks;
                });
                QObject::connect(&option, &QCheckBox::toggled, &host, [&] {
                    ++toggles;
                });
                require(option.presentation().indicator == DescribedOption::Indicator::Toggle &&
                            option.presentation().minimumRowHeight == 116 &&
                            option.presentation().labelPixels == int(metrics::text::body) &&
                            option.presentation().indicatorSize ==
                                QSize(metrics::settings::toggleWidth, metrics::settings::toggleHeight),
                        "Option defaults changed with density");
                QImage previous;
                for (auto indicator : {DescribedOption::Indicator::Toggle, DescribedOption::Indicator::Square,
                                       DescribedOption::Indicator::Radio}) {
                    auto presentation = option.presentation();
                    presentation.indicator = indicator;
                    presentation.dividers = Edges(Qt::TopEdge) | Qt::BottomEdge;
                    presentation.horizontalInset = metrics::chrome::inset;
                    presentation.minimumRowHeight = metrics::chrome::optionHeight;
                    presentation.labelPixels = metrics::chrome::labelText;
                    presentation.descriptionPixels = metrics::chrome::descriptionText;
                    presentation.indicatorSize =
                        indicator == DescribedOption::Indicator::Toggle
                            ? QSize(metrics::settings::toggleWidth, metrics::settings::toggleHeight)
                            : QSize(metrics::chrome::checkSize, metrics::chrome::checkSize);
                    const int beforeClicks = clicks, beforeToggles = toggles;
                    const bool beforeChecked = option.isChecked();
                    option.setPresentation(presentation);
                    require(clicks == beforeClicks && toggles == beforeToggles && option.isChecked() == beforeChecked,
                            "Option presentation emitted intent or changed selection");
                    option.resize(px(700), option.heightForWidth(px(700)));
                    option.show();
                    app.processEvents();
                    const auto unchecked = option.grab().toImage();
                    require(QColor::fromRgba(unchecked.pixel(0, 0)) == dividerColor(&option) &&
                                QColor::fromRgba(unchecked.pixel(0, unchecked.height() - 1)) == dividerColor(&option),
                            "Option lost configured divider edges");
                    if (!previous.isNull())
                        require(previous != unchecked, "Option indicators paint the same shape");
                    previous = unchecked;
                    // Drain queued exposure paints before measuring setter work.
                    bool settled = false;
                    for (int attempt = 0; attempt < 8 && !settled; ++attempt) {
                        const int pendingPaints = option.paints;
                        app.processEvents();
                        settled = option.paints == pendingPaints;
                    }
                    require(settled, "Option exposure paints did not settle");
                    const int paints = option.paints;
                    option.setPresentation(presentation);
                    app.processEvents();
                    require(option.paints == paints, "Unchanged option presentation repainted");
                    const QPointF point(px(20), option.height() / 2);
                    QMouseEvent down(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton,
                                     Qt::NoModifier);
                    QMouseEvent up(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton,
                                   Qt::NoModifier);
                    QApplication::sendEvent(&option, &down);
                    QApplication::sendEvent(&option, &up);
                    require(clicks == beforeClicks + 1 && toggles == beforeToggles + 1 && option.isChecked(),
                            "Option indicator rejected row input or changed native signal timing");
                    require(option.grab().toImage() != unchecked, "Checked option indicator did not change");
                    option.setEnabled(false);
                    QApplication::sendEvent(&option, &down);
                    QApplication::sendEvent(&option, &up);
                    require(clicks == beforeClicks + 1 && option.isChecked(), "Disabled option accepted row input");
                    option.setEnabled(true);
                    option.setChecked(false);
                    app.processEvents();
                }
                DescribedOption compact("Compact", {}, &host);
                auto compactPresentation = compact.presentation();
                compactPresentation.indicator = DescribedOption::Indicator::Radio;
                compactPresentation.minimumRowHeight = px(150);
                compact.setPresentation(compactPresentation);
                require(compact.minimumHeight() == px(150), "Option configuration lost caller minimum height");
                require(compact.heightForWidth(px(700)) == int(metrics::control::stepSize),
                        "Compact option height changed with presentation");
            }
        }
        setDensity(savedDensity);
    }

    kobalt::ui::WholeLineLabel excerpt;
    excerpt.setFont(kobalt::ui::interfaceFont());
    kobalt::ui::readerStyle(&excerpt, "color:black;background:white;font-size:30px;");
    excerpt.resize(400, 106);
    excerpt.setText("<b>MMMM</b><br>MMMM<br>MMMM<br>MMMM");
    excerpt.show();
    app.processEvents();
    const auto excerptImage = excerpt.grab().toImage();
    int excerptInkRows = 0, excerptLines = 0;
    for (int y = 0; y < excerptImage.height(); ++y) {
        bool ink = false;
        for (int x = 0; x < excerptImage.width(); ++x)
            ink |= qGray(excerptImage.pixel(x, y)) < 128;
        if (ink)
            ++excerptInkRows;
        else if (excerptInkRows) {
            require(excerptInkRows >= 20, "Search excerpt clips a text line");
            ++excerptLines;
            excerptInkRows = 0;
        }
    }
    require(excerptLines > 0 && excerptLines < 4 && excerptInkRows == 0,
            "Search excerpt does not end between complete lines");
    excerpt.hide();
    for (int ppi : {167, 212, 300}) {
        kobalt::ui::setDensity(ppi);
        QWidget host;
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        kobalt::ui::PopupCard card(&host);
        auto *title = card.title("Use this font for new books?", "popupTitle");
        auto *message = card.message("Your current book keeps its own appearance settings.", "popupMessage");
        auto *actions = card.actions();
        auto *cancel = kobalt::ui::makeButton(&card, actions, "popupCancel", "Cancel", [] {});
        auto *confirm = kobalt::ui::makeButton(&card, actions, "popupConfirm", "Make default", [] {});
        card.fitToWidth(kobalt::ui::metrics::notice::cardWidth);
        card.show();
        host.show();
        app.processEvents();
        const int textPixels = kobalt::ui::metrics::chrome::labelText;
        require(title->font().pixelSize() == textPixels && title->font().bold(),
                "Popup title lost its shared typography");
        require(message->font().pixelSize() == textPixels && !message->font().bold(),
                "Popup body lost its shared typography");
        require(cancel->font().pixelSize() == textPixels && confirm->font().pixelSize() == textPixels,
                "Popup actions inherited a different text size");
        require(cancel->height() == confirm->height(), "Popup actions have different heights");
        require(confirm->x() - cancel->geometry().right() - 1 == int(kobalt::ui::metrics::notice::cardGap),
                "Popup actions lost their shared gap");
        require(card.rect().contains(title->geometry()) && card.rect().contains(message->geometry()) &&
                    card.rect().contains(cancel->geometry()) && card.rect().contains(confirm->geometry()),
                "Popup content exceeds the card at this density");
        require(message->height() >= message->heightForWidth(message->width()), "Popup body clips wrapped text");
        host.hide();
    }
    kobalt::ui::setDensity(kobalt::ui::designPpi);
    const auto originalPalette = app.palette();
    for (const bool dark : {false, true}) {
        QWidget host;
        host.resize(600, 800);
        host.setProperty("reader_dark", dark);
        QPalette colors = originalPalette;
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        host.setPalette(colors);
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        QLabel passage("Reading page", &host);
        passage.setGeometry(20, 20, 400, 80);
        kobalt::ui::Button outside("Underlying control", &host);
        outside.setGeometry(20, 680, 400, 80);
        QLabel colorPatch(&host);
        colorPatch.setGeometry(450, 20, 100, 80);
        colorPatch.setStyleSheet("background:#347abd;");
        // Qt5 draws a dotted focus outline on the first child button. Keep
        // focus on the host so the backdrop check compares the same state.
        host.setFocusPolicy(Qt::StrongFocus);
        host.show();
        host.setFocus();
        app.processEvents();
        const auto page = host.grab().toImage();
        kobalt::ui::ModalOverlay overlay(&host);
        overlay.setGeometry(host.rect());
        auto *card = overlay.card();
        auto *rows = card->rows();
        auto *label = new QLabel("Synced progress", card);
        rows->addWidget(label);
        auto *keep = new kobalt::ui::Button("Keep reading here", card);
        rows->addWidget(keep);
        auto *go = new kobalt::ui::Button("Go to synced position", card);
        rows->addWidget(go);
        card->setFixedSize(450, 320);
        overlay.centerCard();
        overlay.show();
        overlay.setFocus();
        app.processEvents();
        require(!overlay.isWindow() && overlay.window() == &host,
                "Progress overlay has a separate backing store/theme");
        const auto shown = host.grab().toImage();
        const auto paper = dark ? QColor(Qt::black) : QColor(Qt::white);
        const QPoint gutter = card->pos() + QPoint(4, 4);
        require(QColor::fromRgba(shown.pixel(gutter)) == paper, "Progress card gutter has the wrong theme");
        for (int y = 0; y < page.height(); ++y)
            for (int x = 0; x < page.width(); ++x)
                if (!card->geometry().contains(x, y))
                    require(shown.pixel(x, y) == page.pixel(x, y), "Modal changed pixels outside its card");
        for (int n = 0; n < 6; ++n) {
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
            QApplication::sendEvent(host.focusWidget(), &tab);
            require(overlay.isAncestorOf(host.focusWidget()), "Modal keyboard focus escaped to underlying controls");
        }
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &escape);
        require(overlay.isVisible(), "Escape silently dismissed a progress choice");
        host.resize(800, 600);
        app.processEvents();
        require(card->geometry().center() == overlay.rect().center(), "Modal card did not follow reader geometry");
        host.resize(600, 800);
        overlay.hide();
        host.setFocus();
        app.processEvents();
        require(host.grab().toImage() == page, "Dismissed backdrop did not restore the page exactly");
        kobalt::ui::PopupWindow opening(&host, dark);
        opening.setGeometry(host.geometry());
        auto *openingCard = opening.card();
        openingCard->message("Loading book…");
        auto *bar = new kobalt::ui::ProgressBar(openingCard);
        bar->setValue(50);
        openingCard->rows()->addWidget(bar);
        openingCard->fitToWidth(450);
        opening.show();
        app.processEvents();
        auto image = opening.grab().toImage();
        require(openingCard->geometry().center() == opening.rect().center(), "Opening card is not centered");
        require(QColor::fromRgba(image.pixel(openingCard->pos() + QPoint(4, 4))) == paper,
                "Opening popup did not use the shared theme");
        for (int y = 0; y < page.height(); ++y)
            for (int x = 0; x < page.width(); ++x)
                if (!openingCard->geometry().contains(x, y))
                    require(image.pixel(x, y) == page.pixel(x, y), "Opening popup changed its captured background");
        opening.hide();
        app.processEvents();
        require(host.grab().toImage() == page, "Closing the opening popup damaged its parent");
        host.hide();
    }
    kobalt::ui::PagedText definition;
    definition.resize(400, 95);
    definition.show();
    app.processEvents();
    QString paragraphs;
    for (int n = 0; n < 17; ++n)
        paragraphs += "<p style='margin:0;font-size:24px'>MMMM</p>";
    definition.setHtml(paragraphs);
    int completeLines = 0;
    for (int page = 0; page < definition.pages(); ++page) {
        definition.setPage(page);
        const QImage pixels = definition.grab().toImage();
        int inkRows = 0;
        for (int y = 0; y < pixels.height(); ++y) {
            bool ink = false;
            for (int x = 0; x < pixels.width(); ++x)
                ink |= qGray(pixels.pixel(x, y)) < 128;
            if (ink)
                ++inkRows;
            else if (inkRows) {
                require(inkRows >= 15, "Definition page clipped part of a text line");
                ++completeLines;
                inkRows = 0;
            }
        }
        require(inkRows == 0, "Definition text reaches the bottom edge");
    }
    require(completeLines == 17, "Definition paging omitted or repeated a line");
    definition.resize(980, 426);
    QString table = "<table><tr><td>";
    for (int n = 0; n < 40; ++n)
        table += "<p style='margin:0;font-size:36px'>MMMM MMMM</p>";
    table += "</td><td>";
    for (int n = 0; n < 40; ++n)
        table += "<p style='margin:0;font-size:37px;color:#ff0000'>MMMM MMMM</p>";
    table += "</td></tr></table>";
    definition.setHtml(table);
    require(definition.pages() >= 5, "Multi-column definition was compressed into too few pages");
    for (bool secondColumn : {false, true}) {
        int tableLines = 0;
        for (int page = 0; page < definition.pages(); ++page) {
            definition.setPage(page);
            const auto pixels = definition.grab().toImage();
            int inkRows = 0;
            for (int y = 0; y < pixels.height(); ++y) {
                bool ink = false;
                // Separate the columns by ink colour so either can wrap freely.
                for (int x = 0; x < pixels.width(); ++x) {
                    const auto pixel = pixels.pixel(x, y);
                    ink |= secondColumn ? qRed(pixel) > 160 && qGreen(pixel) < 80 && qBlue(pixel) < 80
                                        : qRed(pixel) < 128 && qGreen(pixel) < 128 && qBlue(pixel) < 128;
                }
                if (ink)
                    ++inkRows;
                else if (inkRows) {
                    require(inkRows >= 23, "Definition page shrank or clipped its text");
                    ++tableLines;
                    inkRows = 0;
                }
            }
            require(inkRows == 0, "Definition column reaches the bottom edge");
        }
        require(tableLines == 40, "Table pagination omitted or repeated a line");
    }
    {
        QString uneven = "<table><tr><td>";
        for (int line = 0; line < 14; ++line)
            uneven += "<p style='margin:0;font-size:36px;color:#ff0000'>MMMM</p>";
        uneven += "</td><td><p style='margin:0;font-size:36px'>MMMM</p></td></tr>"
                  "<tr><td><p style='margin:0;font-size:36px;color:#00aa00'>MMMM</p></td>"
                  "<td><p style='margin:0;font-size:36px;color:#0000ff'>MMMM</p></td></tr></table>";
        definition.setHtml(uneven);
        int counts[4] = {}, firstRows[4] = {-1, -1, -1, -1};
        for (int page = 0; page < definition.pages(); ++page) {
            definition.setPage(page);
            const auto pixels = definition.grab().toImage();
            int inkRows[4] = {};
            for (int y = 0; y < pixels.height(); ++y) {
                bool ink[4] = {};
                for (int x = 0; x < pixels.width(); ++x) {
                    const auto pixel = pixels.pixel(x, y);
                    ink[0] |= qRed(pixel) > 160 && qGreen(pixel) < 80 && qBlue(pixel) < 80;
                    ink[1] |= qRed(pixel) < 80 && qGreen(pixel) < 80 && qBlue(pixel) < 80;
                    ink[2] |= qRed(pixel) < 80 && qGreen(pixel) > 120 && qBlue(pixel) < 80;
                    ink[3] |= qRed(pixel) < 80 && qGreen(pixel) < 80 && qBlue(pixel) > 160;
                }
                for (int color = 0; color < 4; ++color) {
                    if (ink[color]) {
                        if (firstRows[color] < 0)
                            firstRows[color] = page * pixels.height() + y;
                        ++inkRows[color];
                    } else if (inkRows[color]) {
                        require(inkRows[color] >= 23, "Uneven table clipped a line");
                        ++counts[color];
                        inkRows[color] = 0;
                    }
                }
            }
            for (int color = 0; color < 4; ++color)
                require(inkRows[color] == 0, "Uneven table reaches the page edge");
        }
        require(counts[0] == 14 && counts[1] == 1 && counts[2] == 1 && counts[3] == 1,
                "Uneven table lost or repeated a line");
        require(firstRows[2] == firstRows[3], "A table cell acquired a page break from its neighbor");
        definition.setHtml(table);
    }
    const int portraitPages = definition.pages();
    definition.resize(1100, 600);
    app.processEvents();
    definition.resize(980, 426);
    app.processEvents();
    require(definition.pages() == portraitPages, "Resizing retained obsolete definition page breaks");
    {
        const QString list = "<ol style='margin:0;font-size:36px'>"
                             "<li style='margin:0'>MMMM</li><li style='margin:0'>MMMM</li>"
                             "<li style='margin:0 0 12px 0'>MMMM</li><li style='margin:0'>" +
                             QString("MMMM ").repeated(32) + "</li><li style='margin:0'>MMMM</li></ol>";
        kobalt::ui::PagedDocument continuous;
        continuous.setDefaultFont(kobalt::ui::interfaceFont(36));
        continuous.setDocumentMargin(0);
        auto option = continuous.defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        continuous.setDefaultTextOption(option);
        continuous.setHtml(list);
        const int margin = kobalt::ui::metrics::acknowledgments::textMargin;
        const int width = 400;
        continuous.setTextWidth(width - 2 * margin);
        continuous.size();
        QVector<QTextBlock> items;
        for (auto block = continuous.begin(); block.isValid(); block = block.next())
            if (block.text().startsWith("MMMM"))
                items.append(block);
        require(items.size() == 5 && items[3].layout()->lineCount() > 1, "List fixture did not wrap its fourth item");
        const auto first = items[2].layout()->lineAt(0);
        const qreal firstBottom = items[2].layout()->position().y() + first.y() + first.height();
        const qreal secondTop = items[3].layout()->position().y() + items[3].layout()->lineAt(0).y();
        // Put the page boundary inside the margin, so neither adjacent line
        // crosses it. A later wrapped line must not jump back to that boundary.
        const int pageHeight = qRound((firstBottom + secondTop) / 2);
        require(firstBottom < pageHeight && pageHeight < secondTop, "List fixture lacks a margin-only page crossing");
        auto lines = [](const QImage &pixels) {
            int count = 0, inkRows = 0;
            for (int y = 0; y < pixels.height(); ++y) {
                bool ink = false;
                for (int x = 0; x < pixels.width(); ++x)
                    ink |= qGray(pixels.pixel(x, y)) < 128;
                if (ink)
                    ++inkRows;
                else if (inkRows) {
                    require(inkRows >= 23 && inkRows <= 32, "Wrapped list line was clipped or overlapped");
                    ++count;
                    inkRows = 0;
                }
            }
            require(inkRows == 0, "Wrapped list reaches the page edge");
            return count;
        };
        QImage reference(width, qCeil(continuous.size().height()) + 2 * margin, QImage::Format_RGB32);
        reference.fill(Qt::white);
        {
            QPainter painter(&reference);
            painter.translate(margin, margin);
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette = definition.palette();
            continuous.documentLayout()->draw(&painter, context);
        }
        const int expected = lines(reference);
        auto checkList = [&] {
            int count = 0;
            for (int page = 0; page < definition.pages(); ++page) {
                definition.setPage(page);
                count += lines(definition.grab().toImage());
            }
            require(count == expected, "Margin-only page crossing lost or repeated wrapped list lines");
        };
        definition.resize(width, pageHeight + 2 * margin);
        app.processEvents();
        definition.setHtml(list);
        checkList();
        definition.resize(width, pageHeight + 2 * margin + 17);
        app.processEvents();
        checkList();
        definition.resize(width, pageHeight + 2 * margin);
        app.processEvents();
        checkList();
    }
    definition.hide();
    {
        QWidget owner;
        owner.resize(1100, 650);
        owner.show();
        kobalt::reader::DictionariesPage page(&owner);
        page.resize(owner.size());
        page.show();
        QVector<kobalt::DownloadDictionary> entries;
        for (const auto &id : {"en-fr", "ar-en", "en-en", "ar-ar", "de-de", "de-en", "el-el", "cs-cs"}) {
            kobalt::DownloadDictionary entry;
            entry.id = id;
            entry.language = entry.id.section('-', 0, 0);
            entry.target = entry.id.section('-', 1, 1);
            entry.name = entry.id;
            entry.size = 3500000;
            entries.append(entry);
        }
        page.setLanguage("en-US");
        page.setCatalog(entries);
        page.setState({"en-en", "ar-ar"}, false, {});
        page.setStorage({{"en-en", 92000000}, {"ar-ar", 3500000}}, 1000000000);
        app.processEvents();
        auto *first = page.findChild<QWidget *>("rrDictionaryRow0");
        require(first && first->property("dictionary_id").toString() == "en-en",
                "Book-language monolingual dictionary is not first");
        QSet<QString> seen;
        int actionWidth = -1, pageCount = 0;
        do {
            app.processEvents();
            ++pageCount;
            QVector<QWidget *> blocks;
            for (auto *child : page.findChildren<QWidget *>()) {
                if (!child->isVisible())
                    continue;
                if (child->objectName().startsWith("rrDictionaryRow") ||
                    child->objectName().startsWith("rrDictionarySection") ||
                    child->objectName().startsWith("rrDictionaryLanguage"))
                    blocks.append(child);
                if (child->objectName().startsWith("rrDictionaryAction")) {
                    if (actionWidth < 0)
                        actionWidth = child->width();
                    require(child->width() == actionWidth, "Dictionary actions have different widths");
                }
            }
            std::sort(blocks.begin(), blocks.end(), [](QWidget *left, QWidget *right) {
                return left->y() < right->y();
            });
            require(!blocks.isEmpty() && blocks.first()->objectName().startsWith("rrDictionarySection"),
                    "Continued dictionary page lacks a section caption");
            require(blocks.last()->objectName().startsWith("rrDictionaryRow"),
                    "Dictionary page ends with an orphan caption");
            for (auto *block : blocks)
                if (block->objectName().startsWith("rrDictionaryRow")) {
                    const auto id = block->property("dictionary_id").toString();
                    require(!seen.contains(id), "Dictionary repeated between installed and available sections");
                    seen.insert(id);
                }
            auto *next = page.findChild<QPushButton *>("rrDictionariesNext");
            const auto fraction = page.findChild<QLabel *>("rrDictionariesPage")->text().split(" of ");
            if (fraction.first() == fraction.last())
                break;
            next->click();
        } while (pageCount < 20);
        require(seen.size() == entries.size() && pageCount > 1, "Dictionary paging lost entries");
        auto *catalogCounter = page.findChild<QLabel *>("rrDictionariesPage");
        const auto finalCatalogPage = catalogCounter->text();
        page.findChild<QPushButton *>("rrDictionariesNext")->click();
        require(catalogCounter->text().startsWith("1 of "), "Dictionary list did not wrap forwards");
        page.findChild<QPushButton *>("rrDictionariesPrevious")->click();
        require(catalogCounter->text() == finalCatalogPage, "Dictionary list did not wrap backwards");
        page.setLanguage("zz");
        page.setState({}, false, {});
        app.processEvents();
        for (auto *label : page.findChildren<QLabel *>())
            if (label->isVisible())
                require(!label->text().contains("other languages") && !label->text().contains("for this book"),
                        "Unknown language retains book-specific captions");
        auto resetPage = [&] {
            auto *previous = page.findChild<QPushButton *>("rrDictionariesPrevious");
            for (int turn = 0; turn < 20 && !page.findChild<QLabel *>("rrDictionariesPage")->text().startsWith("1 of ");
                 ++turn)
                previous->click();
            app.processEvents();
        };
        QVector<kobalt::DownloadDictionary> english;
        for (const auto &id : {"en-fr", "en-en", "en-de", "en-ar"}) {
            kobalt::DownloadDictionary entry;
            entry.id = id;
            entry.language = "en";
            entry.target = entry.id.section('-', 1, 1);
            entry.name = id;
            entry.size = 3500000;
            english.append(entry);
        }
        page.resize(1100, 470);
        page.setLanguage("en");
        page.setCatalog(english);
        page.setState({}, false, {});
        resetPage();
        QStringList ordered;
        do {
            for (int i = 0; auto *row = page.findChild<QWidget *>(QString("rrDictionaryRow%1").arg(i)); ++i)
                ordered.append(row->property("dictionary_id").toString());
            auto *next = page.findChild<QPushButton *>("rrDictionariesNext");
            const auto fraction = page.findChild<QLabel *>("rrDictionariesPage")->text().split(" of ");
            if (fraction.first() == fraction.last())
                break;
            next->click();
        } while (ordered.size() < 10);
        require(ordered == QStringList({"en-en", "en-ar", "en-fr", "en-de"}),
                "Available dictionaries are not monolingual-first then target-language ordered");
        page.setLanguage("zz");
        resetPage();
        page.findChild<QPushButton *>("rrDictionariesNext")->click();
        app.processEvents();
        auto *continuedLanguage =
            dynamic_cast<kobalt::ui::ListHeader *>(page.findChild<QWidget *>("rrDictionaryLanguage0"));
        require(continuedLanguage && continuedLanguage->leftLabel()->text() == "English",
                "Language subcaption is not repeated on a continued page");
        page.setLanguage("en");
        resetPage();
        page.setState({}, true, {});
        page.setTransfer("en-en", 2000000, 3500000, false);
        app.processEvents();
        auto *progress = page.findChild<QProgressBar *>("rrDictionaryProgress0");
        require(progress && progress->width() == progress->height() && progress->value() > 0,
                "Circular transfer progress is missing");
        require(progress->parentWidget() == page.findChild<QPushButton *>("rrDictionaryAction0"),
                "Progress is not inside the Stop button");
        require(page.findChild<QWidget *>("rrDictionaryRow0")->height() == kobalt::ui::metrics::list::oneLine,
                "Downloading changed the dictionary row height");
        page.setTransfer({}, 0, 0, false);
        page.setState({}, false, "Download failed");
        app.processEvents();
        bool error = false;
        for (auto *label : page.findChildren<QLabel *>())
            error |= label->isVisible() && label->text() == kobalt::strings::get("dictionaries.download_failed");
        require(error, "Failed transfer lacks its row error");
        page.installRequested = [](const QString &) {};
        page.findChild<QPushButton *>("rrDictionaryAction0")->click();
        app.processEvents();
        for (auto *label : page.findChildren<QLabel *>())
            require(!label->isVisible() || label->text() != kobalt::strings::get("dictionaries.download_failed"),
                    "Next page action retained transfer failure");
        page.setState({"en-en"}, false, {});
        resetPage();
        auto *installedHeader =
            dynamic_cast<kobalt::ui::ListHeader *>(page.findChild<QWidget *>("rrDictionarySection0"));
        require(installedHeader && installedHeader->leftLabel()->text().startsWith("Installed for this book"),
                "Completed install did not move into the installed section");
        page.findChild<QPushButton *>("rrDictionariesPrevious")->click();
        page.setCatalog({english[1]});
        page.setState({"en-en"}, false, {});
        app.processEvents();
        require(!page.findChild<QPushButton *>("rrDictionariesPrevious")->isEnabled() &&
                    !page.findChild<QPushButton *>("rrDictionariesNext")->isEnabled(),
                "Dictionary page did not clamp after the list shrank");
    }
    {
        const QString entry =
            "<a name='sample' /><b>sample</b> - noun</p><var><variant name='samples' /></var><p><ol><li>First meaning.</li><li>Second meaning.</li></ol></p></w>";
        QTextDocument document;
        document.setHtml(kobalt::DictionaryPanel::formatDefinition(entry.repeated(3)));
        int headings = 0;
        for (auto block = document.begin(); block.isValid(); block = block.next())
            if (block.text().startsWith("sample - noun")) {
                ++headings;
                require(!block.textList(), "Later dictionary entry heading inherited a definition list");
            }
        require(headings == 3, "Dictionary formatting lost an entry heading");
        const QString ordinary = "<p>A regular definition.</p><ol><li>A meaning.</li></ol>";
        require(kobalt::DictionaryPanel::formatDefinition(ordinary) == ordinary,
                "Kobo normalization changed ordinary dictionary HTML");
    }
    QWidget reader;
    reader.resize(1072, 1448);
    reader.show();
    kobalt::DictionaryPanel dictionary(&reader);
    QVector<kobalt::DictionaryChoice> dictionaries;
    for (int i = 0; i < 13; ++i)
        dictionaries.append({QString::number(i), QString("Dictionary %1").arg(i)});
    dictionary.setChoices(dictionaries);
    kobalt::DictionaryResult result;
    result.dictionary = "Dictionary 8";
    result.dictionaryId = "8";
    result.received = true;
    result.html = QString("<p>A definition long enough to require several pages.</p>").repeated(50);
    for (const auto size : {QSize(1072, 1448), QSize(1448, 1072)}) {
        reader.resize(size);
        dictionary.open("word", result, QRect(100, 100, 40, 40));
        app.processEvents();
        auto *definition = dictionary.findChild<kobalt::ui::PagedText *>("rrDefinition");
        auto *definitionPager = dictionary.findChild<QPushButton *>("rrDictionaryNext")->parentWidget();
        require(definitionPager && definitionPager->isVisible(), "Long definition lacks its pager");
        const QRect definitionGeometry = definition->geometry();
        const int definitionPages = definition->pages();
        dictionary.findChild<QPushButton *>("rrDictionarySelect")->click();
        app.processEvents();
        require(definition->geometry() == definitionGeometry && definition->pages() == definitionPages,
                "Dictionary picker changed definition geometry or pagination");
        auto *choices = dictionary.findChild<QWidget *>("rrDictionaryChoices");
        auto *count = dictionary.findChild<QLabel *>("rrDictionaryChoicesPages");
        require(dictionary.findChild<QPushButton *>("rrDictionaryClose")->size() ==
                    QSize(kobalt::ui::metrics::chrome::cellWidth, kobalt::ui::metrics::chrome::actionHeight),
                "Dictionary close is not a full header cell");
        require(dictionary.x() == 0 && dictionary.width() == reader.width(),
                "Dictionary sheet does not span the screen");
        require(!definitionPager->isVisible(), "Definition pager remains visible under dictionary choices");
        require(count->isVisible() && count->text().contains(" of "), "Dictionary choices lack a page count");
        require(dictionary.rect().contains(choices->geometry()), "Dictionary choices exceed their panel");
        bool selected = false;
        for (auto *button : choices->findChildren<QPushButton *>()) {
            if (!button->isVisible())
                continue;
            require(choices->rect().contains(QRect(button->mapTo(choices, QPoint()), button->size())),
                    "Dictionary choice or pager is clipped");
            if (button->objectName().startsWith("rrDictionaryChoice") && button->isCheckable())
                require(button->height() == kobalt::ui::metrics::list::oneLine,
                        "Dictionary choice row lost its fixed height");
            selected |= button->isChecked() && button->text() == result.dictionary;
        }
        require(selected, "Dictionary picker did not open at the selected dictionary");
        dictionary.findChild<QPushButton *>("rrDictionarySelect")->click();
        app.processEvents();
        require(definitionPager->isVisible() && definition->geometry() == definitionGeometry &&
                    definition->pages() == definitionPages,
                "Closing dictionary choices did not restore the definition pager without reflow");
    }
    // The header names the word before the dictionary, in the book face. A
    // word the face cannot draw in full is drawn in the interface sans. The
    // Latin-only title serif stands in for the book face here.
    {
        auto *word = dictionary.findChild<QLabel *>("rrDictionaryWord");
        auto *select = dictionary.findChild<QPushButton *>("rrDictionarySelect");
        const auto serif = kobalt::ui::brandFont(kobalt::ui::metrics::chrome::titleText).family();
        dictionary.setBookFont(serif);
        const auto familyFor = [&](const QString &text) {
            dictionary.open(text, result, QRect(100, 100, 40, 40));
            app.processEvents();
            word->ensurePolished();
            return word->font().family();
        };
        require(familyFor("sister") == serif && word->text() == "sister",
                "The dictionary header does not show the word in the book face");
        require(word->geometry().right() < select->x(), "The word overlaps the dictionary selector");
        require(familyFor(QString::fromUtf8("αδελφή")) != serif, "A Greek word used a Latin-only book face");
        require(familyFor(QString::fromUtf8("tiếng")) != serif,
                "A partly covered word mixed the book face with fallback glyphs");
        familyFor(QString(200, 'm'));
        require(word->text().endsWith(QString::fromUtf8("…")) && word->geometry().right() < select->x(),
                "A long word was not cut before the selector");
    }
    reader.hide();
    kobalt::ui::Panel paper;
    paper.resize(200, 160);
    for (const auto color : {Qt::white, Qt::black}) {
        QPalette palette = paper.palette();
        palette.setColor(QPalette::Window, color);
        paper.setPalette(palette);
        QImage pixels(paper.size(), QImage::Format_RGB32);
        pixels.fill(Qt::red);
        // Opaque panels must fill their own pixels even when Qt does not
        // prepare a background. Include a partial repaint over old content.
        const QRect damage(20, 30, 100, 90);
        paper.render(&pixels, damage.topLeft(), QRegion(damage), QWidget::DrawChildren);
        require(QColor::fromRgb(pixels.pixel(damage.center())) == QColor(color),
                "Opaque panel left stale book pixels in its background");
        require(QColor::fromRgb(pixels.pixel(0, 0)) == QColor(Qt::red), "Panel painted outside its dirty region");
    }
    QWidget window;
    kobalt::ui::PageNavigation navigation(&window, "pages");
    for (const auto size : {QSize(1072, 1448), QSize(1448, 1072), QSize(920, 600)}) {
        window.resize(size);
        navigation.layout(size, size.height() == 600);
        const QRect previous(navigation.previous->mapTo(&window, QPoint()), navigation.previous->size());
        const QRect next(navigation.next->mapTo(&window, QPoint()), navigation.next->size());
        require(!previous.intersects(next) && window.rect().contains(next),
                "Pager buttons overlap or leave the screen");
        require(navigation.title->geometry().center().x() == window.rect().center().x(),
                "Heading is not centered on screen");
        require(navigation.back->isHidden() == (size.height() == 600), "Font dropdown shows a Back button");
    }
    QWidget panel;
    new QVBoxLayout(&panel);
    panel.resize(1000, 120);
    panel.show();
    app.processEvents();
    kobalt::ui::SampleLabel sample(&panel);
    sample.optical = true;
    sample.setGeometry(0, 0, 400, 68);
    sample.show();
    kobalt::ui::readerStyle(&sample, "background:white;color:black;font-size:32px;");
    {
        sample.ensurePolished();
        const int wordWidth = sample.fontMetrics().boundingRect("Ancizar").width();
        sample.setGeometry(0, 0, wordWidth + 8, 64);
        sample.setAlignment(Qt::AlignCenter);
        sample.setText("Ancizar Serif");
        sample.setWordWrap(false);
        const auto singleLine = sample.grab().toImage();
        sample.setWordWrap(true);
        const auto wrapped = sample.grab().toImage();
        require(wrapped != singleLine, "A multiword font name still used a single elided line");
        int bands = 0;
        bool previous = false;
        for (int y = 0; y < wrapped.height(); ++y) {
            bool ink = false;
            for (int x = 0; x < wrapped.width(); ++x)
                ink |= qGray(wrapped.pixel(x, y)) < 128;
            if (ink && !previous)
                ++bands;
            previous = ink;
        }
        require(bands == 2, "A multiword font name did not paint two complete lines");
        sample.setText(QString(60, 'M'));
        const auto longWord = sample.grab().toImage();
        sample.setWordWrap(false);
        require(sample.grab().toImage() == longWord, "An oversized single word wrapped instead of using an ellipsis");
        sample.setText("Libron");
        const auto shortName = sample.grab().toImage();
        sample.setWordWrap(true);
        require(sample.grab().toImage() == shortName, "Wrapping changed a short font name's appearance");
        sample.setWordWrap(false);
        sample.setGeometry(0, 0, 400, 68);
    }
    for (const auto *name : {"Libron", "Georgia", "gyp", "ÅÉê"}) {
        sample.setText(QString::fromUtf8(name));
        app.processEvents();
        const auto image = sample.grab().toImage();
        QRect ink;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qGray(image.pixel(x, y)) < 128)
                    ink = ink.united(QRect(x, y, 1, 1));
        require(!ink.isEmpty() && qAbs(ink.center().y() - sample.rect().center().y()) <= 2,
                "Font sample ink is not centered");
    }
    // The sample changes actual glyph ink, and Bionic modes suppress stored
    // overall weight. Leaving the mode restores the original sample pixels.
    sample.setText("Reading office");
    sample.setFont(kobalt::ui::fontSample("Libron"));
    const auto normalSample = sample.grab().toImage();
    auto inkAmount = [](const QImage &image) {
        double total = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                total += (255 - qGray(image.pixel(x, y))) / 255.0;
        return total;
    };
    sample.setAppearance({8, "off"});
    const auto heavierSample = sample.grab().toImage();
    require(inkAmount(heavierSample) > inkAmount(normalSample) * 1.08, "Weight did not thicken the font preview");
    for (const auto *mode : {"bold"}) {
        sample.setAppearance({0, mode});
        const auto emphasized = sample.grab().toImage();
        require(emphasized != normalSample && inkAmount(emphasized) > inkAmount(normalSample),
                "Bionic mode did not emphasize the font preview");
        sample.setAppearance({8, mode});
        require(sample.grab().toImage() == emphasized, "Bionic font preview stacked stored weight on its prefixes");
    }
    sample.setAppearance({0, "off"});
    require(sample.grab().toImage() == normalSample, "Font preview did not restore normal ink");
    const auto prefixes = kobalt::ui::samplePrefixes(QString::fromUtf8("Reading αλφα שלום a\xcc\x81"));
    require((prefixes == QVector<QPair<int, int>>{{0, 4}, {8, 2}, {13, 2}, {18, 2}}),
            "Sample prefixes lost Unicode letters or combining marks");
    for (const auto *word : {u8"abⱠcd", u8"abᴫcd", u8"abꞳcd", u8"abᲀcd", u8"ＡＢＣＤＥ", u8"ab\U0001df00cd"}) {
        const auto text = QString::fromUtf8(word);
        const auto letter = text.toUcs4().at(2);
        // Qt5's Unicode tables predate some letters supported by the engine.
        if (!QChar::isLetter(letter))
            continue;
        const auto prefix = kobalt::ui::samplePrefixes(text);
        const int length = letter > 0xffff ? 4 : 3;
        require((prefix == QVector<QPair<int, int>>{{0, length}}),
                "An extended Latin or Cyrillic letter split a font-preview prefix");
    }
    require(kobalt::ui::samplePrefixes(QString::fromUtf8("العربية 中文 한국어")).isEmpty(),
            "Sample emphasis changed contextual or unsegmented scripts");
    require((kobalt::ui::samplePrefixes(QString::fromUtf8("rea\xc2\xad"
                                                          "d")) == QVector<QPair<int, int>>{{0, 2}}),
            "Soft hyphen changed the sample prefix");
    sample.hide();
    kobalt::ui::Button action(&panel);
    action.setGeometry(0, 0, 92, 68);
    kobalt::ui::iconAction(&action, kobalt::ui::ControlIcon::Layout, "Page layout");
    action.show();
    app.processEvents();
    const auto idle = action.grab().toImage();
    action.setDown(true);
    const auto pressed = action.grab().toImage();
    require(idle != pressed, "Icon action has no pressed feedback");
    require(qGray(pressed.pixel(8, 8)) < 128, "Pressed icon action does not fill its background");
    action.setDown(false);
    action.hide();
    {
        using namespace kobalt::ui;
        const int savedDensity = density();
        for (const int ppi : {167, 212, 300})
            for (const bool dark : {false, true}) {
                setDensity(ppi);
                QWidget host;
                host.resize(px(200), px(140));
                host.setProperty("reader_dark", dark);
                auto colors = originalPalette;
                const QColor ink = dark ? Qt::white : Qt::black, paper = dark ? Qt::black : Qt::white;
                colors.setColor(QPalette::Window, paper);
                colors.setColor(QPalette::WindowText, ink);
                colors.setColor(QPalette::Mid, QColor("#999999"));
                colors.setColor(QPalette::Midlight, dark ? QColor("#333333") : QColor("#dddddd"));
                host.setPalette(colors);
                readerStyle(&host, styles::reader());
                host.show();
                PaintCountingButton button(&host);
                button.setGeometry(px(10), px(10), px(132), px(80));
                button.setCheckable(true);
                button.setIconSize(QSize(metrics::control::iconSize, metrics::control::iconSize));
                button.setIconConfiguration({ControlIcon::Close, IconRendition::Action});
                int clicks = 0, toggles = 0;
                QObject::connect(&button, &QPushButton::clicked, &host, [&] {
                    ++clicks;
                });
                QObject::connect(&button, &QPushButton::toggled, &host, [&] {
                    ++toggles;
                });
                button.show();
                app.processEvents();
                const auto iconKey = button.icon().cacheKey();
                const auto geometry = button.geometry();
                for (const auto mode : {ButtonPaintMode::Native, ButtonPaintMode::OutlinedAction,
                                        ButtonPaintMode::ToolbarSquare, ButtonPaintMode::ToolbarCell}) {
                    button.setPaintMode(mode);
                    app.processEvents();
                    require(button.paintMode() == mode && button.icon().cacheKey() == iconKey &&
                                button.geometry() == geometry,
                            "Button paint mode changes its icon asset or geometry");
                    const int paints = button.paints;
                    button.setPaintMode(mode);
                    button.setIconConfiguration({ControlIcon::Close, IconRendition::Action});
                    refreshButtonIcon(&button);
                    app.processEvents();
                    require(button.paints == paints && button.icon().cacheKey() == iconKey,
                            "Unchanged button configuration regenerates or repaints its icon");
                    require(clicks == 0 && toggles == 0, "Programmatic button configuration emits user intent");
                }
                for (const bool enabled : {true, false})
                    for (const bool checked : {false, true})
                        for (const bool down : {false, true}) {
                            button.setEnabled(enabled);
                            button.setChecked(checked);
                            button.setDown(down);
                            const bool active = enabled && (checked || down);
                            button.setPaintMode(ButtonPaintMode::ToolbarSquare);
                            const auto square = button.grab().toImage();
                            button.setPaintMode(ButtonPaintMode::ToolbarCell);
                            const auto cell = button.grab().toImage();
                            const QPoint outer(px(2), button.height() / 2);
                            require(QColor::fromRgba(square.pixel(outer)) == paper &&
                                        QColor::fromRgba(cell.pixel(outer)) == (active ? ink : paper),
                                    "Toolbar square and full-cell feedback have incorrect bounds");
                            require(
                                active ? square != cell : square == cell,
                                "Toolbar placement changes idle or disabled pixels, or loses active full-cell feedback");
                            button.setPaintMode(ButtonPaintMode::OutlinedAction);
                            const auto outlined = button.grab().toImage();
                            const QPoint interior(px(8), px(8));
                            require(QColor::fromRgba(outlined.pixel(interior)) == (active ? ink : paper),
                                    "Outlined action background does not follow enabled, checked and pressed state");
                        }
                button.setEnabled(true);
                button.setChecked(false);
                button.setDown(false);
                button.setPaintMode(ButtonPaintMode::ToolbarCell);
                buttonIcon(&button, ControlIcon::Close);
                require(button.paintMode() == ButtonPaintMode::ToolbarCell &&
                            button.iconConfiguration()->rendition == IconRendition::OriginalAsset,
                        "Original icon configuration changes the button painter");
                const auto originalClose =
                    button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage();
                button.setIconConfiguration({ControlIcon::Close, IconRendition::Action});
                require(
                    button.paintMode() == ButtonPaintMode::ToolbarCell &&
                        button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage() !=
                            originalClose,
                    "Action rendition does not change the asset independently of the painter");
                button.setPaintMode(ButtonPaintMode::Native);
                button.setIconConfiguration({ControlIcon::Portrait, IconRendition::OriginalAsset, false});
                const auto plainRotation =
                    button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage();
                button.setIconConfiguration({ControlIcon::Portrait, IconRendition::OriginalAsset, true});
                const auto sideRotation =
                    button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage();
                require(plainRotation != sideRotation, "Original orientation icon ignores physical side buttons");
                button.setIconConfiguration({ControlIcon::Portrait, IconRendition::Action, false});
                const auto actionRotation =
                    button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage();
                button.setIconConfiguration({ControlIcon::Portrait, IconRendition::Action, true});
                require(button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage() ==
                            actionRotation,
                        "Action cropping changes its accepted standard orientation artwork");
                button.setIconConfiguration({ControlIcon::Close});
                const auto beforeReplay =
                    button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage();
                host.setProperty("reader_dark", !dark);
                refreshButtonIcon(&button);
                require(button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage() !=
                            beforeReplay,
                        "Theme replay retains the previous icon ink");
                host.setProperty("reader_dark", dark);
                refreshButtonIcon(&button);
                require(button.icon().pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas).toImage() ==
                            beforeReplay,
                        "Returning to a theme changes original icon geometry");
            }
        setDensity(savedDensity);
    }
    panel.setProperty("reader_dark", true);
    kobalt::ui::readerStyle(&action, "color:palette(window-text);background:palette(window);");
    require(action.styleSheet().contains("color:white;background:black"), "Dark component colors are wrong");
    {
        QWidget nativeWindow;
        nativeWindow.setProperty("reader_dark", false);
        QWidget reader(&nativeWindow);
        reader.setProperty("reader_dark", true);
        kobalt::ui::Button nested("Read", &reader);
        nested.resize(180, 80);
        kobalt::ui::readerStyle(&nested, "color:palette(window-text);background:palette(window);");
        require(nested.styleSheet().contains("color:white;background:black"),
                "Embedded reader uses the native window's light theme");
        require(kobalt::ui::noteColor(&nested) == QColor(kobalt::ui::metrics::text::explanationDark),
                "Embedded reader caption uses light-theme ink");
        const auto image = nested.grab().toImage();
        require(qGray(image.pixel(4, 4)) == 0, "Embedded reader button has white paper in dark mode");
        require(!nativeWindow.property("reader_dark").toBool(), "Reader changed its host window's theme");
    }
    require(app.palette() == originalPalette, "Component changed the native application's palette");
    PaintCountingButton button("Open panel", &panel);
    button.setGeometry(0, 0, 150, 60);
    button.show();
    app.processEvents();
    int paintsAtClick = -1;
    QObject::connect(&button, &QPushButton::clicked, &panel, [&] {
        paintsAtClick = button.paints;
        button.setText("Done");
    });
    auto sendMouse = [&](QEvent::Type type, QWidget *target) {
        const QPointF local(30, 30), global = target->mapToGlobal(local.toPoint());
        QMouseEvent event(type, local, global, Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };
    sendMouse(QEvent::MouseButtonPress, &button);
    app.processEvents();
    button.paints = 0;
    sendMouse(QEvent::MouseButtonRelease, &button);
    require(paintsAtClick == 0, "Button repainted released state before its click handler");
    app.processEvents();
    require(button.paints > 0, "Button lost its final repaint");
    for (auto indicator :
         {kobalt::ui::DescribedOption::Indicator::Toggle, kobalt::ui::DescribedOption::Indicator::Square,
          kobalt::ui::DescribedOption::Indicator::Radio}) {
        PaintCountingOption option("Refresh", "", &panel);
        auto presentation = option.presentation();
        presentation.indicator = indicator;
        option.setPresentation(presentation);
        option.setGeometry(0, 0, 260, 120);
        option.show();
        app.processEvents();
        int clicks = 0, paintsAtOptionClick = -1;
        QObject::connect(&option, &QCheckBox::clicked, &panel, [&] {
            ++clicks;
            paintsAtOptionClick = option.paints;
        });
        sendMouse(QEvent::MouseButtonPress, &option);
        app.processEvents();
        option.paints = 0;
        sendMouse(QEvent::MouseButtonRelease, &option);
        require(clicks == 1 && option.isChecked() && paintsAtOptionClick == 0,
                "Option paints synchronously before its click handler");
        app.processEvents();
        require(option.paints > 0, "Option loses its selected-state repaint");
        option.hide();
        PaintCountingPanel optionHost;
        optionHost.resize(300, 150);
        optionHost.show();
        auto *closingOption = new PaintCountingOption("Refresh", "", &optionHost);
        closingOption->setGeometry(10, 10, 260, 120);
        closingOption->show();
        QPointer<PaintCountingOption> optionGuard(closingOption);
        QObject::connect(closingOption, &QCheckBox::clicked, &optionHost, [closingOption] {
            delete closingOption;
        });
        app.processEvents();
        sendMouse(QEvent::MouseButtonPress, closingOption);
        app.processEvents();
        optionHost.painted = QRegion();
        sendMouse(QEvent::MouseButtonRelease, closingOption);
        app.processEvents();
        require(!optionGuard && optionHost.painted.contains(QRect(10, 10, 260, 120)),
                "Closing option leaves its pressed background behind");
    }
    PaintCountingPanel destination;
    destination.resize(300, 150);
    destination.show();
    kobalt::ui::Button row("Read license", &destination);
    row.setGeometry(10, 10, 260, 80);
    row.show();
    QObject::connect(&row, &QPushButton::clicked, &destination, [&] {
        row.hide();
    });
    app.processEvents();
    sendMouse(QEvent::MouseButtonPress, &row);
    app.processEvents();
    destination.painted = QRegion();
    sendMouse(QEvent::MouseButtonRelease, &row);
    app.processEvents();
    require(destination.painted.contains(row.geometry()), "Hidden pressed row leaves its background behind");
    destination.hide();
    auto *closing = new PaintCountingButton("Close", &panel);
    closing->setGeometry(0, 0, 150, 60);
    closing->show();
    QPointer<PaintCountingButton> guard(closing);
    QObject::connect(closing, &QPushButton::clicked, &panel, [closing] {
        delete closing;
    });
    sendMouse(QEvent::MouseButtonPress, closing);
    sendMouse(QEvent::MouseButtonRelease, closing);
    require(!guard, "Button click did not delete its target");
    for (auto presentation :
         {kobalt::ui::CaptionButton::Presentation::Caption, kobalt::ui::CaptionButton::Presentation::ListAction}) {
        using namespace kobalt::ui;
        PaintCountingCaptionButton action(presentation, &panel);
        action.setText("Action");
        action.setGeometry(0, 0, 260, 120);
        action.show();
        app.processEvents();
        int clicks = 0, paintsAtAction = -1;
        QObject::connect(&action, &QPushButton::clicked, &panel, [&] {
            ++clicks;
            paintsAtAction = action.paints;
        });
        action.setCaption("Page 4");
        action.setRule(Qt::BottomEdge);
        action.setChevron(false);
        action.setIconVisible(false);
        app.processEvents();
        require(clicks == 0 && action.caption() == "Page 4" && action.presentation() == presentation,
                "Action presentation setters emit user intent or lose their value");
        action.paints = 0;
        action.setCaption("Page 4");
        action.setRule(Qt::BottomEdge);
        action.setChevron(false);
        action.setIconVisible(false);
        app.processEvents();
        require(action.paints == 0, "Unchanged action presentation schedules another paint");
        sendMouse(QEvent::MouseButtonPress, &action);
        app.processEvents();
        action.paints = 0;
        sendMouse(QEvent::MouseButtonRelease, &action);
        require(clicks == 1 && paintsAtAction == 0, "Action bypasses release paint batching");
        app.processEvents();
        require(action.paints > 0, "Action loses its released repaint");
        sendMouse(QEvent::MouseButtonPress, &action);
        QMouseEvent move(QEvent::MouseMove, QPointF(-10, -10), action.mapToGlobal(QPoint(-10, -10)), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&action, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(-10, -10), action.mapToGlobal(QPoint(-10, -10)),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&action, &release);
        require(clicks == 1 && !action.isDown(), "Dragging outside an action still clicks or stays pressed");
        sendMouse(QEvent::MouseButtonPress, &action);
        action.setEnabled(false);
        sendMouse(QEvent::MouseButtonRelease, &action);
        require(clicks == 1 && !action.isDown(), "Disabled action still clicks or stays pressed");
        action.hide();
        PaintCountingPanel actionHost;
        actionHost.resize(300, 150);
        actionHost.show();
        auto *hidden = new CaptionButton(presentation, &actionHost);
        hidden->setGeometry(10, 10, 260, 120);
        hidden->show();
        QObject::connect(hidden, &QPushButton::clicked, &actionHost, [hidden] {
            hidden->hide();
        });
        app.processEvents();
        sendMouse(QEvent::MouseButtonPress, hidden);
        app.processEvents();
        actionHost.painted = QRegion();
        sendMouse(QEvent::MouseButtonRelease, hidden);
        app.processEvents();
        require(actionHost.painted.contains(hidden->geometry()), "Hidden action leaves its pressed background behind");
        auto *removed = new CaptionButton(presentation, &actionHost);
        removed->setGeometry(10, 10, 260, 120);
        removed->show();
        QPointer<CaptionButton> retained(removed);
        QObject::connect(removed, &QPushButton::clicked, &actionHost, [removed] {
            delete removed;
        });
        sendMouse(QEvent::MouseButtonPress, removed);
        sendMouse(QEvent::MouseButtonRelease, removed);
        require(!retained, "Action cannot be deleted during its click callback");
    }
    {
        using namespace kobalt::ui;
        QWidget actionHost;
        auto *actions = new QVBoxLayout(&actionHost);
        int clicks = 0;
        auto *action = makeCaptionButton(&actionHost, actions, "typedAction", "Continue", [&] {
            ++clicks;
        });
        require(action->parentWidget() == &actionHost && actions->indexOf(action) == 0 &&
                    action->focusPolicy() == Qt::NoFocus && action->objectName() == "typedAction" &&
                    action->presentation() == CaptionButton::Presentation::ListAction,
                "Action factory loses its layout, ownership, identity or focus policy");
        action->click();
        require(clicks == 1, "Action factory does not connect its callback");
    }
    {
        // A list action with a note centres both lines on their letters: from
        // the label's capitals to the note's baseline, as Restore defaults shows.
        using namespace kobalt::ui;
        QWidget actionHost;
        actionHost.setStyleSheet("background:white;");
        auto *actions = new QVBoxLayout(&actionHost);
        auto *action = makeCaptionButton(&actionHost, actions, "notedAction", "Restore appearance defaults", [] {});
        action->setCaption("ASKS FIRST");
        action->setChevron(true);
        action->setIconVisible(false);
        action->setFont(interfaceFont(metrics::text::secondary));
        action->setFixedSize(900, metrics::about::actionHeight);
        actionHost.show();
        app.processEvents();
        const auto image = action->grab().toImage();
        const int textRight = image.width() - int(metrics::chrome::actionRight);
        QVector<int> inkRows;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < textRight; ++x)
                if (qGray(image.pixel(x, y)) < 128) {
                    inkRows.append(y);
                    break;
                }
        require(!inkRows.isEmpty(), "List action drew no text");
        // The note is the last group of rows; its baseline is the commonest column bottom.
        int noteTop = inkRows.last();
        for (int i = inkRows.size() - 1; i > 0 && inkRows[i] - inkRows[i - 1] <= 2; --i)
            noteTop = inkRows[i - 1];
        QHash<int, int> bottoms;
        for (int x = 0; x < textRight; ++x)
            for (int y = image.height() - 1; y >= noteTop; --y)
                if (qGray(image.pixel(x, y)) < 128) {
                    ++bottoms[y];
                    break;
                }
        int baseline = noteTop, most = 0;
        for (auto it = bottoms.cbegin(); it != bottoms.cend(); ++it)
            if (it.value() > most) {
                most = it.value();
                baseline = it.key();
            }
        const qreal offset = (inkRows.first() + baseline) / 2.0 - (image.height() - 1) / 2.0;
        require(qAbs(offset) <= 1.5, "List action text is not centred on its letters");
    }
    kobalt::ui::SettingSlider slider(&panel, 0, 100);
    slider.resize(300, 72);
    slider.show();
    slider.setValue(20);
    slider.setMarkers({-1, 0, 50, 50, 100, 101});
    for (const bool dark : {false, true}) {
        QPalette sliderPalette = originalPalette;
        sliderPalette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        sliderPalette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        slider.setPalette(sliderPalette);
        app.processEvents();
        const auto sliderImage = slider.grab().toImage();
        for (const int x : {int(kobalt::ui::metrics::control::sliderInset), 150,
                            slider.width() - kobalt::ui::metrics::control::sliderInset})
            require(qGray(sliderImage.pixel(x, kobalt::ui::metrics::control::sliderMarkerTop +
                                                   kobalt::ui::metrics::control::sliderMarkerHeight / 2)) ==
                        (dark ? 255 : 0),
                    "Slider did not draw a bounded marker above its track");
        require(qGray(sliderImage.pixel(100, kobalt::ui::metrics::control::sliderMarkerTop +
                                                 kobalt::ui::metrics::control::sliderMarkerHeight / 2)) ==
                    (dark ? 0 : 255),
                "Slider drew an out-of-range marker");
    }
    int commits = 0, lastValue = -1;
    slider.committed = [&](int value) {
        ++commits;
        lastValue = value;
    };
    auto slide = [&](QEvent::Type type, int x) {
        const QPointF local(x, 36), global = slider.mapToGlobal(local.toPoint());
        QMouseEvent event(type, local, global, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&slider, &event);
    };
    slide(QEvent::MouseButtonPress, 25);
    slide(QEvent::MouseMove, 200);
    require(commits == 0 && slider.value() == 70, "Slider applied its draft before release");
    slide(QEvent::MouseButtonRelease, 275);
    require(commits == 1 && lastValue == 100, "Slider did not apply its released target once");
    slide(QEvent::MouseButtonPress, 25);
    slider.hide();
    slider.show();
    slide(QEvent::MouseButtonRelease, 25);
    require(commits == 1 && slider.value() == 100, "Hidden slider applied an abandoned gesture");
    slide(QEvent::MouseButtonPress, 25);
    QEvent cancel(QEvent::TouchCancel);
    QApplication::sendEvent(&slider, &cancel);
    slide(QEvent::MouseButtonRelease, 25);
    require(commits == 1 && slider.value() == 100, "Cancelled slider applied an abandoned gesture");
    slider.setRange(0, 0);
    slide(QEvent::MouseButtonPress, 25);
    slide(QEvent::MouseButtonRelease, 275);
    require(commits == 1 && slider.value() == 0, "Single-page slider emitted a jump");
    slider.setRange(100, 300);
    const QVector<int> spacingStops{100, 110, 120, 130, 140, 150, 175, 200, 250, 300};
    slider.setMarkers(spacingStops, kobalt::ui::SettingSlider::MarkerMode::Stops);
    const int inset = kobalt::ui::metrics::control::sliderInset;
    slider.resize(360 + 2 * inset, 72);
    for (int index = 0; index < spacingStops.size(); ++index) {
        const int x = inset + 40 * index;
        slide(QEvent::MouseButtonPress, x + (index == spacingStops.size() - 1 ? -12 : 12));
        require(slider.value() == spacingStops[index], "Pointer did not snap to equally spaced line-height stops");
        slide(QEvent::MouseButtonRelease, x);
        slider.nudge(1);
        require(slider.value() == qMin(300, spacingStops[index] + 1), "Plus skipped a granular line-height value");
        slider.nudge(-1);
        require(slider.value() == (index == spacingStops.size() - 1 ? 299 : spacingStops[index]),
                "Minus snapped instead of changing line height by one percent");
    }
    slider.setRange(14, 96);
    slider.setSingleStep(2);
    const QVector<int> sizeStops{14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 48, 56, 64, 80, 96};
    slider.setMarkers(sizeStops, kobalt::ui::SettingSlider::MarkerMode::Stops);
    slider.resize(560 + 2 * inset, 72);
    for (int index = 0; index < sizeStops.size(); ++index) {
        const int x = inset + 40 * index;
        slide(QEvent::MouseButtonPress, x);
        slide(QEvent::MouseButtonRelease, x);
        require(slider.value() == sizeStops[index], "Font size did not snap to its nonlinear stop");
    }
    slider.nudge(-1);
    require(slider.value() == 94, "Minus did not change 48 pt to 47 pt");
    slider.nudge(-1);
    require(slider.value() == 92, "Minus snapped instead of changing to 46 pt");
    slider.setRange(100, 300);
    slider.setSingleStep(1);
    slider.setMarkers(spacingStops, kobalt::ui::SettingSlider::MarkerMode::Stops);
    slider.resize(360 + 2 * inset, 72);
    for (const bool dark : {false, true}) {
        QPalette colors = originalPalette;
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        slider.setPalette(colors);
        slider.setValue(151);
        const auto image = slider.grab().toImage();
        const int markerY =
            kobalt::ui::metrics::control::sliderMarkerTop + kobalt::ui::metrics::control::sliderMarkerHeight / 2;
        for (int index = 0; index < spacingStops.size(); ++index) {
            require(qGray(image.pixel(inset + 40 * index, markerY)) == (dark ? 255 : 0),
                    "Nonlinear slider marks were not equally spaced");
        }
        require(qGray(image.pixel(inset + 20, markerY)) == (dark ? 0 : 255),
                "Nonlinear slider drew a marker between its stops");
    }
    slider.setValue(151);
    const int beforeSnap = commits;
    slide(QEvent::MouseButtonPress, inset + 240);
    QApplication::sendEvent(&slider, &cancel);
    slide(QEvent::MouseButtonRelease, inset + 240);
    require(slider.value() == 151 && commits == beforeSnap, "Cancelling a snapped gesture lost the granular value");
    kobalt::ui::FontButton font(&panel);
    font.ensurePolished();
    font.resize(500, kobalt::ui::metrics::choices::fontButtonHeight);
    font.setFace("Sans");
    kobalt::ui::Panel dropdown(&panel, kobalt::ui::Edges(Qt::LeftEdge) | Qt::RightEdge | Qt::BottomEdge);
    dropdown.resize(500, 200);
    kobalt::ui::PageNavigation dropdownNavigation(&dropdown, "dropdown");
    dropdownNavigation.layout(dropdown.size(), true);
    dropdownNavigation.setPage(0, 2);
    for (const bool dark : {false, true}) {
        QPalette colors = originalPalette;
        const QColor ink = dark ? Qt::white : Qt::black;
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, ink);
        font.setPalette(colors);
        dropdown.setPalette(colors);
        QImage selector(font.size(), QImage::Format_RGB32), picker(dropdown.size(), QImage::Format_RGB32);
        selector.fill(Qt::red);
        picker.fill(Qt::red);
        font.render(&selector);
        dropdown.render(&picker);
        for (int y = 0; y < selector.height(); ++y)
            require(QColor::fromRgb(selector.pixel(0, y)) == ink && QColor::fromRgb(selector.pixel(499, y)) == ink,
                    "Font selector has a broken border");
        for (int y = 0; y < picker.height(); ++y)
            require(QColor::fromRgb(picker.pixel(0, y)) == ink && QColor::fromRgb(picker.pixel(499, y)) == ink,
                    "Font dropdown has a broken border");
        require(QColor::fromRgb(selector.pixel(250, selector.height() - 1)) == ink &&
                    QColor::fromRgb(picker.pixel(250, 0)) == colors.color(QPalette::Window),
                "Font selector and dropdown doubled their shared border");
    }
    for (const int ppi : {212, 227, 300}) {
        kobalt::ui::setDensity(ppi);
        QTextDocument excerpt;
        excerpt.setHtml(QString("<div style=\"line-height:%1%\">First<br>Second</div>")
                            .arg(kobalt::ui::metrics::search::excerptLineHeight));
        const auto format = excerpt.begin().blockFormat();
        require(format.lineHeightType() == QTextBlockFormat::ProportionalHeight && format.lineHeight() == 130,
                "Search excerpt line spacing changed with screen density");
    }
    for (const bool dark : {false, true}) {
        using namespace kobalt::ui;
        setDensity(designPpi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        host.resize(1000, 1400);
        QPalette colors = originalPalette;
        const QColor paper = dark ? Qt::black : Qt::white, ink = dark ? Qt::white : Qt::black;
        colors.setColor(QPalette::Window, paper);
        colors.setColor(QPalette::WindowText, ink);
        host.setPalette(colors);
        readerStyle(&host, styles::reader());
        host.show();
        ChoiceModal choice(&host, {"A required description", "A title", {}}, {"Cancel", "cancel"},
                           {"Continue", "continue"});
        choice.card()->fitToWidth(760);
        choice.show();
        app.processEvents();
        require(choice.primaryButton()->y() == choice.secondaryButton()->y(),
                "Short modal actions were not side by side");
        require(choice.secondaryButton()->x() < choice.primaryButton()->x(),
                "Secondary modal action was not on the left");
        require(choice.primaryButton()->property("reader_primary_action").toBool() &&
                    !choice.secondaryButton()->property("reader_primary_action").toBool(),
                "Modal action appearances were reversed");
        require(!styles::primary().contains("#bb3333") && !styles::outlined().contains("#bb3333") &&
                    !styles::textAction().contains("#bb3333"),
                "Dialog styles contain a destructive colour");
        for (auto *button : {choice.primaryButton(), choice.secondaryButton()}) {
            const auto fill = button == choice.primaryButton() ? ink : paper;
            const auto hasLabel = [](const QImage &image, const QColor &background) {
                int inkPixels = 0;
                for (int y = 10; y < image.height() - 10; ++y)
                    for (int x = 10; x < image.width() - 10; ++x)
                        if (QColor::fromRgba(image.pixel(x, y)) != background)
                            ++inkPixels;
                return inkPixels > 20;
            };
            const auto normal = button->grab().toImage();
            require(QColor::fromRgba(normal.pixel(5, 5)) == fill, "Modal action has the wrong background");
            require(hasLabel(normal, fill), "Modal action label disappeared into its background");
            button->setDown(true);
            app.processEvents();
            require(QColor::fromRgba(button->grab().toImage().pixel(5, 5)) == QColor(dark ? "#333333" : "#dddddd"),
                    "Pressed modal action has the wrong background");
            require(hasLabel(button->grab().toImage(), QColor(dark ? "#333333" : "#dddddd")),
                    "Pressed modal action label disappeared");
            button->setDown(false);
            button->setEnabled(false);
            app.processEvents();
            require(hasLabel(button->grab().toImage(), paper), "Disabled modal action label disappeared");
            button->setEnabled(true);
        }
        choice.primaryButton()->setText("Use this font as my default for all books");
        choice.card()->fitToWidth(400);
        app.processEvents();
        require(choice.primaryButton()->y() < choice.secondaryButton()->y(), "Long modal actions were not stacked");
        require(choice.primaryButton()->height() > choice.secondaryButton()->height(),
                "Long modal action did not wrap with padding");
        require(choice.card()->rect().contains(choice.primaryButton()->geometry()), "Modal action escaped its card");
        choice.primaryButton()->setText("Yes");
        choice.card()->fitToWidth(760);
        app.processEvents();
        require(choice.primaryButton()->y() == choice.secondaryButton()->y(),
                "Modal actions did not return to one row");
        {
            PopupCard resizing(&host);
            auto *body =
                resizing.message("This description wraps at a narrow width and must shrink when the card grows wider.",
                                 "resizingDescription");
            resizing.fitToWidth(400);
            const int narrowHeight = body->height();
            resizing.fitToWidth(760);
            require(body->height() < narrowHeight, "Modal description kept its narrow wrapping after widening");
            readerStyle(body, styles::font(48));
            resizing.fitToWidth(760);
            const int largeHeight = body->height();
            readerStyle(body, styles::font(24));
            resizing.fitToWidth(760);
            require(body->height() < largeHeight, "Modal description kept its previous font height");
        }
        QImage image(320, 120, QImage::Format_RGB32);
        image.fill(Qt::green);
        NoticeModal notice(&host, {{}, "Title without a body", image}, {"OK", "ok"});
        notice.card()->fitToWidth(400);
        notice.show();
        app.processEvents();
        require(notice.card()->findChildren<QLabel *>().size() == 2, "Optional body left an empty label");
        require(!notice.descriptionLabel(), "Notice without a body created a description");
        require(choice.descriptionLabel()->textFormat() == Qt::PlainText, "Modal description interpreted markup");
        require(notice.card()->findChildren<QPushButton *>().size() == 1, "Notice did not have exactly one action");
        require(!notice.card()->grab().isNull(), "Modal with an image could not be painted");
        host.resize(380, 1000);
        app.processEvents();
        require(choice.card()->width() <= host.width() - 2 * int(metrics::notice::sideClearance),
                "Modal ignored the screen side clearance");
        require(choice.rect().contains(choice.card()->geometry()), "Resized modal card escaped its screen");
    }
    kobalt::ui::setDensity(kobalt::ui::designPpi);
    std::puts("PASS: shared navigation, release-only sliders, continuous picker borders, actions and dark styling");
}
