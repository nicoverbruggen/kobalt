#include "library_view.h"
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/loading.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/dialogs/action_menu.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/styles.h"
#include "library/arrangement.h"
#include "reading/reading_density.h"
#include "support/strings.h"
#include "ui/hold_timer.h"
#include <QPointer>
#include <utility>
#include <QApplication>
#include <QCache>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScroller>
#include <QTimer>
#include <QVBoxLayout>

namespace kobalt::library {
namespace c = ui::metrics::chrome;
using ui::Px;
using ui::TextPx;
namespace metrics {
// Library sizes in pixels of a 300 ppi screen, like the reader's.
inline constexpr Px toolbarHeight{88}, modeWidth{104};
// The grid's cells hold a 2:3 cover and, for a book in progress, a bar under
// it. The cover itself names the book. Covers aim for these widths.
inline constexpr Px smallCover{200}, mediumCover{260}, largeCover{340};
inline constexpr Px gridGap{28}, gridTop{28}, gridBarGap{10};
// List rows use the reader's three-line height.
inline constexpr Px thumbWidth{120}, thumbHeight{180}, thumbPadding{20}, progressWidth{150}, barWidth{88},
    barHeight{14};
// Keep onboarding readable in a wide library and scrollable on short screens.
// Its text grows with large text; the buttons keep their height.
inline constexpr Px emptyMargin{54}, emptyWidth{800}, buttonHeight{108};
inline constexpr TextPx message{56}, body{30};
// A held finger may drift this far, about 4 mm, before the hold becomes a drag.
inline constexpr Px holdSlop{48};
inline constexpr int longPressMilliseconds = 550, coverCacheEntries = 64, throttleMilliseconds = 1000;
}
namespace lm = metrics;
// The view behind each toolbar cell, in toolbar order.
constexpr View modeViews[] = {View::Home, View::List, View::Grid};

namespace {
ui::Button *actionButton(QWidget *parent, const QString &text, const char *name, bool primary) {
    auto *button = new ui::Button(text, parent);
    button->setObjectName(name);
    button->setProperty("reader_primary_action", primary);
    ui::readerStyle(button, (primary ? ui::styles::primary() : ui::styles::outlined()) +
                               "QPushButton{" + ui::styles::font(c::labelText) + "}");
    button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
}
int minimumListRowHeight() {
    return qMax<int>(ui::metrics::list::threeLines, lm::thumbHeight + 2 * lm::thumbPadding);
}
QString countText(int count) {
    return count == 1 ? strings::get("library.count_one") : strings::get("library.count").arg(count);
}
QString statusText(const Book &book) {
    switch (book.status()) {
    case Status::Finished:
        return strings::get("library.status_done");
    case Status::Reading:
        return strings::get("common.percent").arg(book.percent);
    case Status::New:
        break;
    }
    return strings::get("library.status_new");
}
QString seriesText(const Book &book) {
    if (book.series.isEmpty())
        return {};
    return book.seriesIndex > 0
               ? strings::get("library.series_number").arg(book.series, QLocale().toString(book.seriesIndex))
               : book.series;
}
// Plain caption text as the reader's list headers draw it.
void drawCaption(QPainter &painter, const QRect &rect, const QString &text, Qt::Alignment alignment,
                 const QColor &color, int pixels = c::captionText) {
    auto font = ui::captionFont(pixels);
    painter.setFont(font);
    painter.setPen(color);
    ui::drawInterfaceText(painter, rect, ui::elideInterfaceText(text.toUpper(), painter.font(), rect.width()), alignment | Qt::AlignVCenter);
}
void drawChevron(QPainter &painter, QPoint center, int size, bool down, const QColor &color) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, qMax(1, ui::px(3)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const int h = size / 2;
    if (down)
        painter.drawPolyline(
            QPolygon({center + QPoint(-h, -h / 2), center + QPoint(0, h / 2), center + QPoint(h, -h / 2)}));
    else
        painter.drawPolyline(
            QPolygon({center + QPoint(-h / 2, -h), center + QPoint(h / 2, 0), center + QPoint(-h / 2, h)}));
    painter.restore();
}
}

// The covers on screen, drawn as one widget so a page change repaints once.
class BookPages final : public QWidget {
    friend class LibraryView;

public:
    BookPages(const Index &index, std::function<QString(const QString &)> coverPath, QWidget *parent)
        : QWidget(parent), index(index), coverPath(std::move(coverPath)) {
        setObjectName("libraryPages");
        setProperty("reader_owns_swipes", true);
        setAttribute(Qt::WA_OpaquePaintEvent);
        covers.setMaxCost(lm::coverCacheEntries);
        hold.setSingleShot(true);
        hold.setInterval(lm::longPressMilliseconds);
        connect(&hold, &QTimer::timeout, this, [this] { triggerHold(); });
    }
    struct Placement {
        QRect area, cover;
        const Book *book = nullptr;
    };
    View view = View::Grid;
    CoverSize coverSize = CoverSize::Medium;
    QVector<const Book *> books;
    int page = 0;
    std::function<void(const QString &)> onOpen, onHold;
    std::function<void(int)> onTurn;

    int pageCount() {
        layout();
        return qMax(1, pages);
    }
    QVector<Placement> placements() {
        layout();
        QVector<Placement> result;
        if (view == View::List) {
            const int first = page * perPage;
            for (int row = 0; row < perPage && first + row < books.size(); ++row) {
                const int top = rowTop(row);
                const QRect area(0, top, width(), rowTop(row + 1) - top);
                const QRect cover(c::inset, area.center().y() - lm::thumbHeight / 2, lm::thumbWidth, lm::thumbHeight);
                result.append({area, cover, books[first + row]});
            }
        } else {
            const int first = page * perPage;
            for (int i = 0; i < perPage && first + i < books.size(); ++i) {
                const int column = i % columns, row = i / columns;
                const QRect area(c::inset + column * (cellWidth + lm::gridGap), rowTop(row), cellWidth, cellHeight);
                // A cover narrower than its column stands in the middle of it.
                const QRect cover(area.left() + (cellWidth - coverWidth) / 2, area.top(), coverWidth, coverWidth * 3 / 2);
                result.append({area, cover, books[first + i]});
            }
        }
        return result;
    }
    QString emptyText;
    // Home starts its covers at the top and shows each book's percentage.
    bool home = false;

protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::TouchCancel) {
            hold.stop();
            gestureActive = held = false;
            pressed = -1;
            setHighlight(-1);
            event->accept();
            return true;
        }
        return QWidget::event(event);
    }
    void resizeEvent(QResizeEvent *) override {
        dirty = true;
    }
    void paintEvent(QPaintEvent *event) override {
        QPainter painter(this);
        const auto paper = palette().color(QPalette::Window), ink = palette().color(QPalette::WindowText);
        painter.fillRect(event->rect(), paper);
        placed = placements();
        if (placed.isEmpty() && !emptyText.isEmpty()) {
            painter.setFont(ui::interfaceFont(c::labelText));
            painter.setPen(ui::noteColor(this));
            ui::drawWrappedInterfaceText(painter, rect().adjusted(c::inset, 0, -c::inset, -height() / 3), emptyText, Qt::AlignCenter);
            return;
        }
        if (view == View::List)
            paintList(painter, ink);
        else
            paintGrid(painter, ink, paper);
    }
    void mousePressEvent(QMouseEvent *event) override {
        gestureActive = true;
        // The books can change between a paint and the next press.
        placed = placements();
        held = false;
        start = event->pos();
        pressed = hitAt(event->pos());
        if (pressed >= 0 && placed[pressed].book) {
            hold.start(event);
            setHighlight(pressed);
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        // These pixels are the reader's 300 PPI pixels, so the system drag
        // distance, in screen points, would cancel a hold on the slightest drift.
        if ((event->pos() - start).manhattanLength() > lm::holdSlop) {
            hold.stop();
            setHighlight(-1);
        }
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (!std::exchange(gestureActive, false))
            return;
        if ((event->pos() - start).manhattanLength() > lm::holdSlop)
            hold.stop();
        QPointer<BookPages> guard(this);
        if (hold.finish(event))
            triggerHold();
        if (!guard)
            return;
        setHighlight(-1);
        const auto delta = event->pos() - start;
        // A horizontal swipe turns the page, as in the reader.
        if (!held && qAbs(delta.x()) > 4 * QApplication::startDragDistance() && qAbs(delta.x()) > 2 * qAbs(delta.y())) {
            if (onTurn)
                onTurn(delta.x() < 0 ? 1 : -1);
            pressed = -1;
            return;
        }
        const int released = hitAt(event->pos());
        if (!held && released >= 0 && released == pressed) {
            const auto &hit = placed[released];
            if (hit.book && onOpen)
                onOpen(hit.book->key);
        }
        pressed = -1;
        held = false;
    }
    void contextMenuEvent(QContextMenuEvent *event) override {
        placed = placements();
        const int hit = hitAt(event->pos());
        if (hit >= 0 && placed[hit].book && onHold) {
            held = true;
            hold.stop();
            setHighlight(-1);
            onHold(placed[hit].book->key);
        }
    }

private:
    void triggerHold() {
        if (held || pressed < 0 || !placed.value(pressed).book)
            return;
        held = true;
        setHighlight(-1);
        if (onHold)
            onHold(placed[pressed].book->key);
    }
    // The book under a press, drawn pressed until the press ends or moves away.
    int highlighted = -1;
    void setHighlight(int value) {
        if (highlighted == value)
            return;
        highlighted = value;
        update();
    }
    bool isHighlighted(const Placement &hit) const {
        return highlighted >= 0 && highlighted < placed.size() && &hit == &placed[highlighted];
    }
    const Index &index;
    std::function<QString(const QString &)> coverPath;
    QCache<QString, QImage> covers;
    QVector<Placement> placed;
    ui::HoldTimer hold;
    bool gestureActive = false;
    QPoint start;
    QSize laidOut;
    int perPage = 1, pages = 1, columns = 1, cellWidth = 0, cellHeight = 0, coverWidth = 0, pressed = -1;
    // Rows on a page: the first row's top and the distance between rows.
    // Space a page cannot use is shared above, between and below the rows,
    // so a page looks the same on every screen size.
    int firstTop = 0, rowStep = 0;
    bool held = false;

public:
    bool dirty = true;

private:
    int rowTop(int row) const {
        // Round boundaries, not individual heights, so every list pixel belongs
        // to a row and the final row ends exactly at the pager.
        return view == View::List ? int(qint64(row) * height() / perPage) : firstTop + row * rowStep;
    }
    // Shares the space left under `rows` rows of `height`, with `gap` between
    // them, equally above, between and below the rows.
    void spread(int rows, int height, int gap) {
        const int spare = qMax(0, this->height() - rows * height - (rows - 1) * gap) / (rows + 1);
        firstTop = spare;
        rowStep = height + gap + spare;
    }
    const QImage *cover(const Book &book) {
        if (book.cover.isEmpty())
            return nullptr;
        // A book keeps its cover file name when it changes, so the cache key
        // also holds the file's date: a changed book reads its new cover.
        const auto key = book.cover + QLatin1Char(':') + QString::number(book.modified);
        if (auto *image = covers.object(key))
            return image->isNull() ? nullptr : image;
        // Missing or damaged files are cached as null images, so they are
        // not read again for every paint.
        QImageReader reader(coverPath(book.cover));
        const auto size = reader.size();
        auto *image =
            new QImage(size.isValid() && size.width() <= 1024 && size.height() <= 1024 ? reader.read() : QImage());
        covers.insert(key, image);
        return image->isNull() ? nullptr : image;
    }
    void layout() {
        if (!dirty && laidOut == size())
            return;
        dirty = false;
        laidOut = size();
        const int available = height();
        if (view == View::List) {
            // Rows are tall enough for the cover with some room around it.
            perPage = qMax(1, available / minimumListRowHeight());
            pages = (books.size() + perPage - 1) / perPage;
        } else {
            const int room = width() - 2 * c::inset, height = available - lm::gridTop;
            const int target = coverSize == CoverSize::Small    ? int(lm::smallCover)
                               : coverSize == CoverSize::Medium ? int(lm::mediumCover)
                                                                : int(lm::largeCover);
            int below = lm::gridBarGap + lm::barHeight;
            if (home)
                below += lm::gridBarGap + percentHeight();
            // The nearest whole number of columns and rows for the target
            // size; covers then shrink to fit both ways, so a page is full.
            columns = qMax(2, qRound(qreal(room + lm::gridGap) / (target + lm::gridGap)));
            const int gridRows =
                qMax(1, qRound(qreal(height + lm::gridGap) / (target * 3 / 2 + below + lm::gridGap)));
            cellWidth = (room - (columns - 1) * lm::gridGap) / columns;
            const int tallest = (height - (gridRows - 1) * lm::gridGap) / gridRows - below;
            coverWidth = qMax(1, qMin(cellWidth, tallest * 2 / 3));
            cellHeight = coverWidth * 3 / 2 + below;
            perPage = columns * gridRows;
            if (home) {
                firstTop = lm::gridTop;
                rowStep = cellHeight + lm::gridGap;
            } else {
                spread(gridRows, cellHeight, lm::gridGap);
            }
            pages = (books.size() + perPage - 1) / perPage;
        }
        page = qBound(0, page, qMax(0, pages - 1));
    }
    int hitAt(QPoint point) const {
        for (int i = 0; i < placed.size(); ++i)
            if (placed[i].area.contains(point))
                return i;
        return -1;
    }
    // A missing cover: the reopen card's drawing, a rule above and below the title.
    static void paintPlainCover(QPainter &painter, const QRect &rect, const Book &book, const QColor &ink,
                                const QColor &paper) {
        painter.fillRect(rect, paper);
        ui::paintEdges(painter, rect, Qt::LeftEdge | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge, ink,
                       qMax(1, ui::px(2)));
        const int inset = qMax(4, rect.width() / 12);
        const int top = rect.top() + rect.height() / 3, bottom = rect.bottom() - rect.height() / 5;
        painter.fillRect(rect.left() + inset, top, rect.width() - 2 * inset, qMax(1, ui::px(2)), ink);
        painter.fillRect(rect.left() + inset, bottom, rect.width() - 2 * inset, qMax(1, ui::px(2)), ink);
        painter.setPen(ink);
        painter.setFont(ui::brandFont(qMax(8, rect.width() / 7)));
        ui::drawWrappedInterfaceText(painter, QRect(rect.left() + inset, top + inset / 2, rect.width() - 2 * inset, bottom - top - inset), book.displayTitle(), Qt::AlignLeft | Qt::AlignTop);
    }
    void paintCover(QPainter &painter, const QRect &box, const Book &book, const QColor &ink, const QColor &paper,
                    bool fit) {
        const auto *image = cover(book);
        if (!image) {
            paintPlainCover(painter, box, book, ink, paper);
            return;
        }
        QRect target = box;
        if (fit) {
            // Fit inside the cell's 2:3 box, standing on its bottom edge.
            const auto size = image->size().scaled(box.size(), Qt::KeepAspectRatio);
            target = QRect(box.left() + (box.width() - size.width()) / 2, box.bottom() + 1 - size.height(),
                           size.width(), size.height());
        }
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, *image);
        painter.restore();
    }
    void paintList(QPainter &painter, const QColor &ink) {
        const auto note = ui::noteColor(this), divider = ui::dividerColor(this);
        const auto paper = palette().color(QPalette::Window);
        for (const auto &hit : placed) {
            const auto &book = *hit.book;
            const auto &area = hit.area;
            // A pressed row turns the reader's pressed gray, as list rows do.
            if (isHighlighted(hit))
                painter.fillRect(area, palette().color(QPalette::Midlight));
            paintCover(painter, hit.cover, book, ink, paper, true);
            const int left = hit.cover.right() + 1 + c::inset, right = area.right() - lm::progressWidth;
            const int textWidth = qMax(1, right - left);
            auto title = ui::interfaceFont(c::labelText);
            auto author = ui::interfaceFont(c::descriptionText);
            auto caption = ui::captionFont();
            const int titleHeight = ui::interfaceLineHeight(title), authorHeight = ui::interfaceLineHeight(author),
                      captionHeight = ui::interfaceLineHeight(caption);
            const int gap = ui::px(6);
            // Center the lines the row has: title, then author and caption when present.
            const auto caption_ = seriesText(book);
            const int linesHeight = titleHeight + (book.author.isEmpty() ? 0 : gap + authorHeight) +
                                    (caption_.isEmpty() ? 0 : gap + captionHeight);
            int y = area.center().y() - linesHeight / 2;
            painter.setFont(title);
            painter.setPen(ink);
            ui::drawInterfaceText(painter, QRect(left, y, textWidth, titleHeight), ui::elideInterfaceText(book.displayTitle(), painter.font(), textWidth), Qt::AlignLeft | Qt::AlignVCenter);
            y += titleHeight + gap;
            if (!book.author.isEmpty()) {
                painter.setFont(author);
                painter.setPen(note);
                ui::drawInterfaceText(painter, QRect(left, y, textWidth, authorHeight), ui::elideInterfaceText(book.author, painter.font(), textWidth), Qt::AlignLeft | Qt::AlignVCenter);
                y += authorHeight + gap;
            }
            if (!caption_.isEmpty())
                drawCaption(painter, QRect(left, y, textWidth, captionHeight), caption_, Qt::AlignLeft, ink);
            // Progress column: the percentage over a bar, or New or Done.
            const QRect column(area.right() - lm::progressWidth, area.top(), lm::progressWidth - c::inset,
                               area.height());
            const bool reading = book.status() == Status::Reading;
            drawCaption(painter, column.adjusted(0, 0, 0, reading ? -lm::barHeight - gap : 0), statusText(book),
                        Qt::AlignRight, ink, c::numberText);
            if (reading) {
                const QRect bar(column.right() + 1 - lm::barWidth, column.center().y() + captionHeight / 2,
                                lm::barWidth, lm::barHeight);
                ui::paintEdges(painter, bar, Qt::LeftEdge | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge, ink,
                               qMax(1, ui::px(2)));
                painter.fillRect(QRect(bar.left(), bar.top(), bar.width() * book.percent / 100, bar.height()), ink);
            }
            painter.fillRect(area.left(), area.bottom() + 1 - qMax(1, ui::px(2)), area.width(), qMax(1, ui::px(2)),
                             divider);
        }
    }
    void paintGrid(QPainter &painter, const QColor &ink, const QColor &paper) {
        for (const auto &hit : placed) {
            const auto &book = *hit.book;
            paintCover(painter, hit.cover, book, ink, paper, true);
            // A pressed cover gets a heavy frame along its own edge, clear of the
            // progress bar and the neighbouring covers.
            if (isHighlighted(hit))
                ui::paintEdges(painter, hit.cover, Qt::LeftEdge | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge, ink,
                               ui::px(8));
            // A book in progress shows the list's outlined bar, without a number.
            if (book.status() == Status::Reading) {
                const QRect bar(hit.cover.left(), hit.cover.bottom() + 1 + lm::gridBarGap, hit.cover.width(),
                                lm::barHeight);
                ui::paintEdges(painter, bar, Qt::LeftEdge | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge, ink,
                               qMax(1, ui::px(2)));
                painter.fillRect(QRect(bar.left(), bar.top(), bar.width() * book.percent / 100, bar.height()), ink);
                // On its own line, the percentage always has the cover's width.
                if (home)
                    drawCaption(painter,
                                QRect(bar.left(), bar.bottom() + 1 + lm::gridBarGap, bar.width(), percentHeight()),
                                strings::get("common.percent").arg(book.percent), Qt::AlignLeft, ink, c::numberText);
            }
        }
    }
    static int percentHeight() {
        return ui::interfaceLineHeight(ui::captionFont(c::numberText));
    }
};

// The toolbar's sort caption: the sort's name and a chevron, or scan progress.
class SortButton final : public ui::Button {
public:
    explicit SortButton(QWidget *parent) : ui::Button(parent) {
        setObjectName("librarySort");
        setFocusPolicy(Qt::NoFocus);
    }
    // Busy shows scan progress; plain names the view, with no choices to open.
    bool open = false, busy = false, plain = false;
    // The scan progress beside the caption, or zero when it is hidden.
    int progressWidth = 0;
    // Whether the caption fits beside scan progress this wide.
    bool fitsBeside(int progress) const {
        return ui::interfaceTextWidth(text().toUpper(), ui::captionFont()) <=
               width() - 2 * c::inset - progress - ui::px(24);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const auto paper = palette().color(QPalette::Window), ink = palette().color(QPalette::WindowText);
        painter.fillRect(rect(), isDown() ? palette().color(QPalette::Midlight) : paper);
        const int left = busy && progressWidth ? c::inset + progressWidth + ui::px(24) : int(c::inset);
        auto font = ui::captionFont();
        painter.setFont(font);
        const int chevron = ui::px(18), gap = ui::px(12);
        const bool choices = !busy && !plain;
        const int available = qMax(1, width() - left - c::inset - (choices ? chevron + gap : 0));
        const auto shown = painter.fontMetrics().elidedText(text().toUpper(), Qt::ElideRight, available);
        drawCaption(painter, QRect(left, 0, available, height()), shown, Qt::AlignLeft, ink);
        if (choices)
            drawChevron(painter,
                        QPoint(left + painter.fontMetrics().horizontalAdvance(shown) + gap + chevron / 2, height() / 2),
                        chevron, true, ink);
        Q_UNUSED(open);
    }
};

// Sort and filter choices, dropped over the top of the books. A short screen
// puts each group in its own column, side by side.
class SortSheet final : public ui::Panel {
public:
    explicit SortSheet(QWidget *parent) : ui::Panel(parent) {
        setObjectName("librarySortSheet");
        auto *rows = new QHBoxLayout(this);
        rows->setContentsMargins(0, 0, 0, c::heavyRule);
        rows->setSpacing(0);
        for (int index = 0; index < 3; ++index) {
            columns[index] = new ui::Panel(this, index ? ui::Edges(Qt::LeftEdge) : ui::Edges());
            columnRows[index] = new QVBoxLayout(columns[index]);
            columnRows[index]->setContentsMargins(index ? int(ui::metrics::control::border) : 0, 0, 0, 0);
            columnRows[index]->setSpacing(0);
            rows->addWidget(columns[index], 1);
        }
        auto *sortHeading = new ui::ListHeader(this);
        sortHeading->setShaded(true);
        sortHeading->setCaptions(strings::get("library.sort_heading"));
        sorts = new ui::RadioList(this);
        sorts->setObjectName("librarySortChoices");
        sorts->add("librarySortOpened", strings::get("library.sort_opened"));
        sorts->add("librarySortAdded", strings::get("library.sort_added"));
        sorts->add("librarySortTitle", strings::get("library.sort_title"));
        sorts->add("librarySortAuthor", strings::get("library.sort_author"));
        sorts->add("librarySortSeries", strings::get("library.sort_series"));
        auto *showHeading = new ui::ListHeader(this);
        showHeading->setShaded(true);
        showHeading->setCaptions(strings::get("library.show_heading"));
        filters = new ui::RadioList(this);
        filters->setObjectName("libraryFilterChoices");
        filters->add("libraryShowAll", strings::get("library.show_all"));
        filters->add("libraryHideFinished", strings::get("library.hide_finished"));
        coverHeading = new ui::ListHeader(this);
        coverHeading->setShaded(true);
        coverHeading->setCaptions(strings::get("library.cover_heading"));
        covers = new ui::RadioList(this);
        covers->setObjectName("libraryCoverChoices");
        covers->add("libraryCoverSmall", strings::get("library.cover_small"));
        covers->add("libraryCoverMedium", strings::get("library.cover_medium"));
        covers->add("libraryCoverLarge", strings::get("library.cover_large"));
        groups = {sortHeading, sorts, showHeading, filters, coverHeading, covers};
        arrange(false, true);
    }
    // Cover size applies only to the cover grid.
    void arrange(bool sideBySide, bool grid) {
        coverHeading->setVisible(grid);
        covers->setVisible(grid);
        for (int index = 0; index < groups.size(); ++index) {
            const int column = sideBySide ? index / 2 : 0;
            for (auto *layout : columnRows)
                layout->removeWidget(groups[index]);
            columnRows[column]->addWidget(groups[index]);
        }
        // A shorter column keeps its rows at the top.
        for (auto *layout : columnRows) {
            for (int index = layout->count() - 1; index >= 0; --index)
                if (layout->itemAt(index)->spacerItem())
                    delete layout->takeAt(index);
            layout->addStretch(1);
        }
        columns[1]->setVisible(sideBySide);
        columns[2]->setVisible(sideBySide && grid);
    }
    ui::RadioList *sorts, *filters, *covers;

protected:
    void paintEvent(QPaintEvent *event) override {
        ui::Panel::paintEvent(event);
        QPainter painter(this);
        painter.fillRect(0, height() - c::heavyRule, width(), c::heavyRule, palette().color(QPalette::WindowText));
    }

private:
    ui::Panel *columns[3];
    QVBoxLayout *columnRows[3];
    ui::ListHeader *coverHeading;
    QVector<QWidget *> groups;
};

// The folders the library scans, with the controls to add, remove and rescan.
class FolderPage final : public ui::Panel {
public:
    FolderPage(const Index &index, QWidget *parent) : ui::Panel(parent), index(index) {
        setObjectName("libraryFolders");
        back = new ui::CellButton(ui::ControlIcon::Back, this, c::backIcon);
        back->setObjectName("libraryFoldersBack");
        back->setRule(Qt::RightEdge);
        back->setAccessibleName(strings::get("library.back"));
        title = new kobalt::ui::InterfaceLabel(strings::get("library.folders"), this);
        title->setTextFormat(Qt::PlainText);
        ui::readerStyle(title, ui::styles::brand(c::titleText) + "background:transparent;");
        heading = new ui::ListHeader(this);
        heading->setTrailingInset(c::cellWidth);
        viewport = new QWidget(this);
        content = new QWidget(viewport);
        pager = new ui::PagerBar(this, "libraryFolders");
        connect(pager->previous, &QPushButton::clicked, this, [this] {
            page = pager->turnTarget(-1);
            arrange();
        });
        connect(pager->next, &QPushButton::clicked, this, [this] {
            page = pager->turnTarget(1);
            arrange();
        });
        check = new ui::DescribedOption(strings::get("library.check_at_start"),
                                        strings::get("library.check_at_start_description"), content);
        check->setObjectName("libraryCheckAtStart");
        auto look = check->presentation();
        look.indicator = ui::DescribedOption::Indicator::Square;
        look.dividers = Qt::BottomEdge;
        look.horizontalInset = c::inset;
        look.labelPixels = c::labelText;
        look.descriptionPixels = c::descriptionText;
        look.indicatorSize = {c::checkSize, c::checkSize};
        look.minimumRowHeight = c::optionHeight;
        check->setPresentation(look);
        add = actionButton(content, strings::get("library.add_folder"), "libraryFoldersAdd", true);
        rescan = actionButton(content, strings::get("library.rescan"), "libraryFoldersRescan", false);
        description = new kobalt::ui::InterfaceLabel(strings::get("library.folders_description"), content);
        description->setWordWrap(true);
        description->setTextFormat(Qt::PlainText);
        ui::readerStyle(description, ui::styles::font(c::descriptionText) +
                                         "color:palette(placeholder-text);background:transparent;");
        hide();
    }
    ui::CellButton *back;
    ui::DescribedOption *check;
    QPushButton *add, *rescan;
    std::function<void(const QString &)> removeRequested;

    void refresh() {
        heading->setCaptions(strings::get("library.folders_summary").arg(index.folders.size()),
                             strings::get("library.books_heading"));
        while (rows.size() < index.folders.size()) {
            auto *row = new ui::ListRow(content);
            auto *remove = row->addAction({}, strings::get("library.remove_folder"));
            const int position = rows.size();
            connect(remove, &QPushButton::clicked, this, [this, position] {
                if (position < index.folders.size() && removeRequested)
                    removeRequested(index.folders[position].key);
            });
            row->setEnabled(true);
            row->setObjectName(QString("libraryFolder%1").arg(position));
            remove->setObjectName(QString("libraryFolderRemove%1").arg(position));
            rows.append(row);
        }
        for (int i = 0; i < rows.size(); ++i) {
            rows[i]->setVisible(i < index.folders.size());
            if (i >= index.folders.size())
                continue;
            rows[i]->setTitle(index.folders[i].label);
            rows[i]->setDetail(index.folders[i].key.startsWith('/') ? index.folders[i].key : QString());
            rows[i]->setAccessory(ui::ListRow::Caption{QString::number(index.folderBooks(index.folders[i].key))});
        }
        check->setChecked(index.settings.checkAtStart);
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *) override {
        arrange();
    }
    void paintEvent(QPaintEvent *event) override {
        ui::Panel::paintEvent(event);
        QPainter painter(this);
        painter.fillRect(0, c::actionHeight - c::heavyRule, width(), c::heavyRule,
                         palette().color(QPalette::WindowText));
    }

private:
    const Index &index;
    QLabel *title, *description;
    ui::ListHeader *heading;
    QVector<ui::ListRow *> rows;
    QWidget *viewport, *content;
    ui::PagerBar *pager;
    int page = 0;
    void arrange() {
        const int bar = c::actionHeight - c::heavyRule;
        back->setGeometry(0, 0, c::cellWidth, bar);
        title->setGeometry(c::cellWidth + c::inset, 0, qMax(1, width() - c::cellWidth - 2 * c::inset), bar);
        heading->setGeometry(0, c::actionHeight, width(), ui::metrics::list::headerHeight);
        const int top = c::actionHeight + ui::metrics::list::headerHeight;
        int y = 0;
        QVector<QPair<int, int>> items;
        for (int i = 0; i < rows.size() && i < index.folders.size(); ++i) {
            rows[i]->setGeometry(0, y, width(), ui::metrics::list::twoLines);
            items.append({y, y + int(ui::metrics::list::twoLines)});
            y += ui::metrics::list::twoLines;
        }
        const int checkHeight = check->heightForWidth(width());
        check->setGeometry(0, y, width(), checkHeight);
        items.append({y, y + checkHeight});
        y += checkHeight + ui::metrics::spacing::gutter;
        const int buttons = width() - 2 * c::inset, buttonHeight = add->sizeHint().height();
        add->setGeometry(c::inset, y, buttons, buttonHeight);
        items.append({y, y + buttonHeight});
        y += buttonHeight + ui::metrics::spacing::row;
        rescan->setGeometry(c::inset, y, buttons, buttonHeight);
        items.append({y, y + buttonHeight});
        y += buttonHeight + ui::metrics::spacing::gutter;
        const int descriptionHeight = description->heightForWidth(buttons);
        description->setGeometry(c::inset, y, buttons, descriptionHeight);
        items.append({y, y + descriptionHeight});
        const int bodyHeight = y + descriptionHeight;
        const bool overflow = bodyHeight > height() - top;
        const int available = qMax(1, height() - top - (overflow ? int(c::actionHeight) : 0));
        QVector<int> starts{0};
        int start = 0;
        for (const auto &item : items) {
            if (item.second - start > available && item.first > start) {
                starts.append(item.first);
                start = item.first;
            }
            while (item.second - start > available) {
                start += available;
                starts.append(start);
            }
        }
        page = qBound(0, page, int(starts.size()) - 1);
        const int end = page + 1 < starts.size() ? starts[page + 1] : bodyHeight;
        const int used = qMin(available, qMax(0, end - starts[page]));
        viewport->setGeometry(0, top, width(), used);
        content->setGeometry(0, -starts[page], width(), bodyHeight);
        pager->setGeometry(0, height() - c::actionHeight, width(), c::actionHeight);
        pager->setPage(page, starts.size(), {}, true);
        pager->setBlankArea({c::inset, top + used, qMax(1, width() - 2 * int(c::inset)), available - used});
    }
};

LibraryView::LibraryView(const Index &index, std::function<QString(const QString &)> coverPath, QWidget *parent)
    : ui::Panel(parent), index(index), coverPath(coverPath) {
    setObjectName("library");
    setAttribute(Qt::WA_StyledBackground);
    ui::readerStyle(this, ui::styles::reader());
    const auto cell = [this](ui::ControlIcon icon, const char *name, const QString &label) {
        auto *button = new ui::CellButton(icon, this);
        button->setObjectName(name);
        button->setAccessibleName(label);
        button->setRule(Qt::LeftEdge);
        return button;
    };
    search = cell(ui::ControlIcon::Search, "librarySearch", strings::get("library.search"));
    more = cell(ui::ControlIcon::More, "libraryMore", strings::get("library.more"));
    closeSearch = cell(ui::ControlIcon::Close, "libraryCloseSearch", strings::get("library.close_search"));
    closeSearch->hide();
    query = new QLineEdit(this);
    kobalt::ui::prepareNativeEditor(query);
    query->setObjectName("librarySearchQuery");
    query->setMaxLength(128);
    query->setPlaceholderText(strings::get("library.search_placeholder"));
    query->setInputMethodHints(Qt::ImhNoPredictiveText);
    ui::readerStyle(query, "QLineEdit{border:none;padding:0;background:palette(window);" +
                               ui::styles::font(c::queryText) + "}");
    query->hide();
    // Home first, then the views that list every book.
    const char *modeNames[] = {"libraryViewHome", "libraryViewList", "libraryViewGrid"};
    const char *modeLabels[] = {"library.view_home", "library.view_list", "library.view_grid"};
    const ui::ControlIcon modeIcons[] = {ui::ControlIcon::Home, ui::ControlIcon::Rows, ui::ControlIcon::Grid};
    for (int i = 0; i < 3; ++i) {
        auto *mode = cell(modeIcons[i], modeNames[i], strings::get(modeLabels[i]));
        mode->setCheckable(true);
        modes.append(mode);
        connect(mode, &QPushButton::clicked, this, [this, i] {
            auto settings = this->index.settings;
            closeOverlay();
            settings.view = modeViews[i];
            applySettings(settings);
        });
    }
    sort = new SortButton(this);
    progress = new ui::LoadingIndicator(ui::LoadingIndicator::Style::Squares, this);
    progress->setObjectName("libraryScanProgress");
    progress->hide();
    pages = new BookPages(index, coverPath, this);
    pager = new ui::PagerBar(this, "library");
    empty = buildEmpty();
    throttle = new QTimer(this);
    throttle->setSingleShot(true);
    throttle->setInterval(lm::throttleMilliseconds);
    connect(throttle, &QTimer::timeout, this, [this] {
        rebuild(true);
    });

    connect(search, &QPushButton::clicked, this, [this] {
        setSearching(true);
    });
    connect(closeSearch, &QPushButton::clicked, this, [this] {
        setSearching(false);
    });
    connect(query, &QLineEdit::textChanged, this, [this] {
        pages->page = 0;
        rebuild(false);
    });
    connect(query, &QLineEdit::returnPressed, this, [this] {
        if (keyboard) keyboard->hide();
    });
    connect(more, &QPushButton::clicked, this, &LibraryView::showMoreSheet);
    connect(sort, &QPushButton::clicked, this, [this] {
        if (sortSheet && sortSheet->isVisible()) {
            closeOverlay();
            return;
        }
        if (!sortSheet) {
            sortSheet = new SortSheet(this);
            setDark(dark);
            sortSheet->sorts->chosen = [this](int choice) {
                auto settings = this->index.settings;
                settings.sort = Sort(choice);
                applySettings(settings);
                closeOverlay();
            };
            sortSheet->filters->chosen = [this](int choice) {
                auto settings = this->index.settings;
                settings.hideFinished = choice == 1;
                applySettings(settings);
                closeOverlay();
            };
            sortSheet->covers->chosen = [this](int choice) {
                auto settings = this->index.settings;
                settings.coverSize = CoverSize(choice);
                applySettings(settings);
                closeOverlay();
            };
        }
        sortSheet->sorts->setCurrent(int(this->index.settings.sort));
        sortSheet->filters->setCurrent(this->index.settings.hideFinished ? 1 : 0);
        sortSheet->covers->setCurrent(int(this->index.settings.coverSize));
        const int top = c::actionHeight + lm::toolbarHeight;
        sortSheet->arrange(isShortHeight(height(), ui::density()), this->index.settings.view == View::Grid);
        sortSheet->setGeometry(0, top, width(), sortSheet->sizeHint().height());
        sortSheet->show();
        sortSheet->raise();
        static_cast<SortButton *>(sort)->open = true;
    });
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        pages->page = pager->turnTarget(-1);
        rebuild(true);
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        pages->page = pager->turnTarget(1);
        rebuild(true);
    });
    pages->onOpen = [this](const QString &key) {
        closeOverlay();
        if (openBook)
            openBook(key);
    };
    pages->onHold = [this](const QString &key) {
        showBookMenu(key);
    };
    pages->onTurn = [this](int direction) {
        if (pages->pageCount() > 1) {
            pages->page = pager->turnTarget(direction);
            rebuild(true);
        }
    };
    setDark(false);
    rebuild(false);
}
LibraryView::~LibraryView() = default;
void LibraryView::setKeyboardFactory(KeyboardFactory factory) {
    keyboard = factory ? factory(this, query) : nullptr;
    if (!keyboard) return;
    keyboard->setSubmitText(strings::get("search_panel.search"));
    keyboard->submitted = [this] { query->clearFocus(); };
    if (searching) keyboard->show();
}

QWidget *LibraryView::buildEmpty() {
    // Short windows, such as a phone in landscape, scroll this page.
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("libraryEmpty");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
    auto *page = new QWidget;
    page->setObjectName("libraryEmptyPage");
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(lm::emptyMargin, lm::emptyMargin, lm::emptyMargin, lm::emptyMargin);
    auto *content = new QWidget(page);
    content->setMaximumWidth(lm::emptyWidth);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    outer->addStretch(1);
    auto *across = new QHBoxLayout;
    across->addStretch(1);
    across->addWidget(content, 100);
    across->addStretch(1);
    outer->addLayout(across);
    outer->addStretch(2);
    auto *message = new kobalt::ui::InterfaceLabel(strings::get("library.empty_title"), page);
    message->setObjectName("libraryEmptyTitle");
    // Italic glyphs overhang the label's text metrics on both axes.
    message->setMargin(ui::px(8));
    message->setWordWrap(true);
    message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui::readerStyle(message, ui::styles::brand(lm::message, true) + "background:transparent;");
    column->addWidget(message);
    column->addSpacing(ui::px(24));
    auto *body = new kobalt::ui::InterfaceLabel(strings::get("library.empty_body"), page);
    body->setObjectName("libraryEmptyBody");
    body->setWordWrap(true);
    body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui::readerStyle(body, ui::styles::font(lm::body) + "color:palette(placeholder-text);background:transparent;");
    column->addWidget(body);
    column->addSpacing(ui::px(40));
    const auto button = [this, page, column](const QString &text, const char *name, bool primary,
                                             std::function<void()> action) {
        auto *created = actionButton(page, text, name, primary);
        created->setMinimumHeight(lm::buttonHeight);
        column->addWidget(created);
        column->addSpacing(ui::metrics::spacing::row);
        connect(created, &QPushButton::clicked, this, std::move(action));
    };
    // A host picker can enter a nested event loop. Finish the button release
    // and restore its painting before opening one.
    button(strings::get("library.open_book"), "libraryOpenBook", true, [this] {
        if (openFile)
            QTimer::singleShot(0, this, openFile);
    });
    button(strings::get("library.choose_folder"), "libraryChooseFolder", false, [this] {
        if (addFolder)
            QTimer::singleShot(0, this, addFolder);
    });
    scroll->setWidget(page);
    scroll->hide();
    return scroll;
}

void LibraryView::refresh() {
    pages->dirty = true;
    if (folderPage && folderPage->isVisible())
        folderPage->refresh();
    // During a scan, books arrive one by one. Redraw once a second at most.
    if (scanning) {
        if (!throttle->isActive())
            throttle->start();
        return;
    }
    rebuild(true);
}
void LibraryView::setScanning(bool value, int count) {
    const bool changed = scanning != value || found != count;
    scanning = value;
    found = count;
    if (!changed)
        return;
    if (!scanning) {
        throttle->stop();
        rebuild(true);
    } else if (!throttle->isActive())
        throttle->start();
}
void LibraryView::setDark(bool value) {
    dark = value;
    const auto colors = ui::readerPalette(palette(), dark);
    setProperty("reader_dark", dark);
    ui::applyReaderPalette(this, colors);
    for (auto *widget : findChildren<QWidget *>()) {
        ui::applyReaderPalette(widget, colors);
        if (auto *button = qobject_cast<QPushButton *>(widget))
            ui::refreshButtonIcon(button);
    }
    QWidget::update();
}
void LibraryView::showNotice(const QString &title, const QString &message) {
    closeOverlay();
    auto *notice = new ui::NoticeModal(this, {message, title, {}}, {strings::get("common.ok"), "libraryNoticeOk"});
    overlay = notice;
    connect(notice->primaryButton(), &QPushButton::clicked, this, &LibraryView::closeOverlay);
    notice->dismissed = [this] {
        closeOverlay();
    };
    setDark(dark);
    notice->setGeometry(rect());
    notice->fitCard();
    notice->show();
    notice->raise();
    notice->setFocus();
}
void LibraryView::pageButton(int direction) {
    if (!isVisible() || !isEnabled() || overlay || (sortSheet && sortSheet->isVisible()) ||
        (moreSheet && moreSheet->isVisible()))
        return;
    ui::turnVisiblePageControl(folderPage && folderPage->isVisible() ? static_cast<QWidget *>(folderPage) : this, direction);
}
bool LibraryView::dismiss() {
    if (overlay || (sortSheet && sortSheet->isVisible()) || (moreSheet && moreSheet->isVisible())) {
        closeOverlay();
        return true;
    }
    if (folderPage && folderPage->isVisible()) {
        folderPage->hide();
        return true;
    }
    if (searching) {
        setSearching(false);
        return true;
    }
    return false;
}
void LibraryView::closeOverlay() {
    if (sortSheet && sortSheet->isVisible()) {
        sortSheet->hide();
        static_cast<SortButton *>(sort)->open = false;
    }
    if (moreSheet)
        moreSheet->hide();
    if (overlay) {
        overlay->hide();
        overlay->deleteLater();
        overlay = nullptr;
    }
}
void LibraryView::applySettings(const Settings &settings) {
    if (settingsChanged)
        settingsChanged(settings);
    pages->page = 0;
    rebuild(false);
}
void LibraryView::setSearching(bool value) {
    searching = value;
    query->setVisible(value);
    closeSearch->setVisible(value);
    search->setVisible(!value);
    if (value) {
        closeOverlay();
        query->setFocus();
        if (keyboard) keyboard->show();
        else QGuiApplication::inputMethod()->show();
    } else {
        query->clear();
        if (keyboard) keyboard->hide();
        else QGuiApplication::inputMethod()->hide();
    }
    arrange();
    rebuild(false);
}
void LibraryView::rebuild(bool keepPage) {
    const auto &settings = index.settings;
    const bool noLibrary = index.folders.isEmpty() && index.visibleCount() == 0 && !scanning;
    empty->setVisible(noLibrary);
    if (noLibrary)
        empty->raise();
    const auto text = searching ? query->text() : QString();
    const bool home = settings.view == View::Home;
    // Home shows the largest covers; the cover grid uses the chosen size.
    pages->coverSize = home ? CoverSize::Large : settings.coverSize;
    pages->home = home;
    if (home) {
        // The books in progress as covers, the most recently opened first.
        pages->view = View::Grid;
        pages->books = library::arrange(index, {Sort::Opened, false, text});
        pages->books.erase(std::remove_if(pages->books.begin(), pages->books.end(),
                                          [](const Book *book) {
                                              return book->status() != Status::Reading;
                                          }),
                           pages->books.end());
    } else {
        pages->view = settings.view;
        pages->books = library::arrange(index, {settings.sort, settings.hideFinished, text});
    }
    if (!keepPage)
        pages->page = 0;
    pages->dirty = true;
    pages->emptyText = !pages->books.isEmpty()                           ? QString()
                       : searching && !query->text().trimmed().isEmpty() ? strings::get("library.no_matches")
                       : home && index.visibleCount()                    ? strings::get("library.home_empty")
                       : settings.hideFinished && index.visibleCount()   ? strings::get("library.nothing_shown")
                                                                         : strings::get("library.empty_folders");
    for (int i = 0; i < modes.size(); ++i)
        modes[i]->setChecked(settings.view == modeViews[i]);
    const char *sortNames[] = {"library.sort_opened", "library.sort_added", "library.sort_title", "library.sort_author",
                               "library.sort_series"};
    auto *sortButton = static_cast<SortButton *>(sort);
    sortButton->busy = scanning;
    // Home has its own order, so its caption names the view instead of a sort.
    sortButton->plain = home;
    sortButton->setText(scanning ? strings::get("library.scanning").arg(found)
                        : home   ? strings::get("library.now_reading")
                                 : strings::get(sortNames[int(settings.sort)]));
    sortButton->setEnabled(!scanning && !home);
    if (scanning && !progress->isRunning())
        progress->start(0);
    else if (!scanning)
        progress->stop();
    if (pages->view == View::List) {
        // Decide whether a pager is needed before measuring the list. Otherwise
        // its previous visibility can leave an unnecessary page or stale count.
        const int available = qMax(1, height() - c::actionHeight - lm::toolbarHeight);
        const int capacity = qMax(1, available / minimumListRowHeight());
        pager->setVisible(pages->books.size() > capacity);
        arrange();
    }
    const int count = pages->pageCount();
    pager->setPage(pages->page, count, {}, true);
    pager->setVisible(count > 1);
    sort->setVisible(!noLibrary);
    for (auto *mode : modes)
        mode->setVisible(!noLibrary);
    arrange();
    sort->update();
    pages->QWidget::update();
    QWidget::update();
}
void LibraryView::arrange() {
    const int bar = c::actionHeight - c::heavyRule;
    int x = width();
    if (!searching) {
        x -= c::cellWidth;
        more->setGeometry(x, 0, c::cellWidth, bar);
    }
    more->setVisible(!searching);
    x -= c::cellWidth;
    (searching ? static_cast<QWidget *>(closeSearch) : static_cast<QWidget *>(search))
        ->setGeometry(x, 0, c::cellWidth, bar);
    const int left = 0;
    query->setGeometry(left, 0, qMax(1, x - left), bar);
    query->setTextMargins(c::inset, 0, c::inset, 0);
    const int toolbarTop = c::actionHeight;
    for (int i = 0; i < modes.size(); ++i)
        modes[i]->setGeometry(width() - (modes.size() - i) * lm::modeWidth, toolbarTop, lm::modeWidth,
                              lm::toolbarHeight - ui::metrics::control::border);
    sort->setGeometry(0, toolbarTop, qMax(1, width() - modes.size() * lm::modeWidth),
                      lm::toolbarHeight - ui::metrics::control::border);
    const auto hint = progress->sizeHint();
    auto *sortButton = static_cast<SortButton *>(sort);
    // As the reader's tabs drop labels that do not fit, scan progress gives
    // way to a caption that large text makes too wide to sit beside it.
    const bool squares = sortButton->busy && sortButton->fitsBeside(hint.width());
    sortButton->progressWidth = squares ? hint.width() : 0;
    progress->setVisible(squares);
    progress->setGeometry(c::inset, toolbarTop + (lm::toolbarHeight - hint.height()) / 2, hint.width(), hint.height());
    const int top = toolbarTop + lm::toolbarHeight;
    const int bottom = !pager->isHidden() ? height() - c::actionHeight : height();
    const int gap = pages->view == View::List ? 0 : ui::px(8);
    pages->setGeometry(0, top + gap, width(), qMax(1, bottom - top - gap));
    pager->setGeometry(0, height() - c::actionHeight, width(), c::actionHeight);
    empty->setGeometry(0, c::actionHeight, width(), qMax(1, height() - c::actionHeight));
    if (folderPage)
        folderPage->setGeometry(rect());
    if (overlay)
        overlay->setGeometry(rect());
}
void LibraryView::resizeEvent(QResizeEvent *) {
    pages->dirty = true;
    arrange();
    rebuild(true);
    if (keyboard) keyboard->place();
}
void LibraryView::hideEvent(QHideEvent *event) {
    if (keyboard) keyboard->hide();
    ui::Panel::hideEvent(event);
}
void LibraryView::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    // The reader style sheet on this root widget keeps its own text color, so
    // take the ink from the theme rather than the palette.
    const auto &ink = ui::themeColors(dark).ink;
    painter.fillRect(0, c::actionHeight - c::heavyRule, width(), c::heavyRule, ink);
    painter.fillRect(0, c::actionHeight + lm::toolbarHeight - ui::metrics::control::border, width(),
                     ui::metrics::control::border, ui::dividerColor(this));
    if (searching)
        return;
    const int bar = c::actionHeight - c::heavyRule;
    const int left = c::inset;
    // Search and More take the right of the header.
    const int right = width() - 2 * c::cellWidth - c::inset;
    auto titleFont = ui::brandFont(c::titleText);
    painter.setFont(titleFont);
    painter.setPen(ink);
    const auto heading = strings::get("library.title");
    const int titleWidth = qMin(ui::interfaceTextWidth(heading, painter.font()), qMax(1, right - left));
    ui::drawInterfaceText(painter, QRect(left, 0, titleWidth, bar), ui::elideInterfaceText(heading, painter.font(), qMax(1, right - left)), Qt::AlignLeft | Qt::AlignVCenter);
    const int shown = index.visibleCount();
    painter.setFont(ui::interfaceFont(c::countText));
    painter.setPen(ui::noteColor(this));
    const int countLeft = left + titleWidth + ui::px(24);
    if (right > countLeft)
        ui::drawInterfaceText(painter, QRect(countLeft, 0, right - countLeft, bar), ui::elideInterfaceText(countText(shown), painter.font(), right - countLeft), Qt::AlignRight | Qt::AlignVCenter);
}

void LibraryView::showBookMenu(const QString &key) {
    closeOverlay();
    const auto found = index.books.constFind(key);
    if (found == index.books.constEnd())
        return;
    const auto &book = found.value();
    ui::ActionMenuHeader header;
    header.image = book.cover.isEmpty() ? QImage() : QImage(coverPath(book.cover));
    if (header.image.isNull()) {
        // A book without a cover shows the same drawing as in the views.
        header.image = QImage(ui::metrics::actionMenu::thumbnailWidth, ui::metrics::actionMenu::thumbnailHeight,
                              QImage::Format_RGB32);
        QPainter painter(&header.image);
        const auto &colors = ui::themeColors(dark);
        BookPages::paintPlainCover(painter, header.image.rect(), book, colors.ink, colors.paper);
    }
    header.title = book.displayTitle();
    header.detail = book.author;
    QStringList caption{statusText(book)};
    if (!book.series.isEmpty())
        caption.append(seriesText(book));
    header.caption = caption.join(QStringLiteral(" · "));
    const auto status = book.status();
    QVector<ui::ActionMenuItem> items{
        ui::actionItem(strings::get(status == Status::New ? "library.start_reading" : "library.continue_reading"),
                       "libraryBookMenuOpen", ui::ControlIcon::Dictionary, [this, key] {
                           if (openBook)
                               openBook(key);
                       })};
    if (status != Status::Finished)
        items.append(ui::actionItem(strings::get("library.mark_finished"), "libraryBookMenuMark",
                                    ui::ControlIcon::Check, [this, key] {
                                        if (markBook)
                                            markBook(key, Book::Mark::Finished);
                                    }));
    // The host resets the book's settings and position before marking it unread.
    if (status != Status::New)
        items.append(ui::actionItem(strings::get("library.mark_unread"), "libraryBookMenuUnread", ui::ControlIcon::Undo,
                                    [this, key] {
                                        if (markBook)
                                            markBook(key, Book::Mark::Unread);
                                    }));
    items.append(ui::actionItem(strings::get("library.remove_book"), "libraryBookMenuRemove", ui::ControlIcon::Trash,
                                [this, key] {
                                    confirmRemove(key);
                                }));
    showMenu(new ui::ActionMenu(this, header, items), "libraryBookMenu");
}
void LibraryView::showMenu(ui::ActionMenu *menu, const char *name) {
    menu->setObjectName(name);
    overlay = menu;
    menu->dismissed = [this] {
        closeOverlay();
    };
    setDark(dark);
    menu->setGeometry(rect());
    // The book's menu is modal: a patterned backdrop covers the library.
    menu->setDimmed(true);
    menu->fitCard();
    menu->show();
    menu->raise();
    menu->setFocus();
}
void LibraryView::confirmRemove(const QString &key) {
    closeOverlay();
    if (!index.books.contains(key))
        return;
    const auto &book = index.books[key];
    const bool copy = book.folder.isEmpty() && singleBooksAreCopies;
    auto *modal =
        new ui::ChoiceModal(this,
                            {strings::get(copy ? "library.remove_copy_description" : "library.remove_book_description"),
                             strings::get("library.remove_book_title"),
                             {}},
                            {strings::get("common.cancel"), "libraryRemoveCancel"},
                            {strings::get("library.remove"), "libraryRemoveConfirm"});
    overlay = modal;
    connect(modal->secondaryButton(), &QPushButton::clicked, this, &LibraryView::closeOverlay);
    connect(modal->primaryButton(), &QPushButton::clicked, this, [this, key] {
        closeOverlay();
        if (removeBook)
            removeBook(key);
    });
    modal->dismissed = [this] {
        closeOverlay();
    };
    setDark(dark);
    modal->setGeometry(rect());
    modal->setDimmed(true);
    modal->fitCard();
    modal->show();
    modal->raise();
    modal->setFocus();
}
void LibraryView::showMoreSheet() {
    if (moreSheet && moreSheet->isVisible()) {
        closeOverlay();
        return;
    }
    closeOverlay();
    if (!moreSheet) {
        moreSheet = new ui::ActionMenu(this, {}, {
            ui::actionItem(strings::get("library.add_folder"), "libraryMoreAddFolder", ui::ControlIcon::FolderPlus,
                           [this] {
                               if (addFolder)
                                   QTimer::singleShot(0, this, addFolder);
                           }),
            ui::actionItem(strings::get("library.open_book"), "libraryMoreOpenBook", ui::ControlIcon::Dictionary,
                           [this] {
                               if (openFile)
                                   QTimer::singleShot(0, this, openFile);
                           }),
            ui::actionItem(strings::get("library.folders"), "libraryMoreFolders", ui::ControlIcon::Folders,
                           [this] { showFolders(); }, true),
        });
        moreSheet->setObjectName("libraryMoreSheet");
        moreSheet->dismissed = [this] { closeOverlay(); };
        moreSheet->setDimmed(true);
        setDark(dark);
    }
    moreSheet->setGeometry(rect());
    moreSheet->fitCard();
    moreSheet->show();
    moreSheet->raise();
    moreSheet->setFocus();
}
void LibraryView::showFolders() {
    if (!folderPage) {
        folderPage = new FolderPage(index, this);
        connect(folderPage->back, &QPushButton::clicked, folderPage, &QWidget::hide);
        connect(folderPage->add, &QPushButton::clicked, this, [this] {
            if (addFolder)
                QTimer::singleShot(0, this, addFolder);
        });
        connect(folderPage->rescan, &QPushButton::clicked, this, [this] {
            if (rescan)
                rescan();
        });
        connect(folderPage->check, &QCheckBox::toggled, this, [this](bool checked) {
            if (checked == index.settings.checkAtStart)
                return;
            auto settings = index.settings;
            settings.checkAtStart = checked;
            if (settingsChanged)
                settingsChanged(settings);
        });
        folderPage->removeRequested = [this](const QString &key) {
            if (removeFolder)
                removeFolder(key);
        };
        setDark(dark);
    }
    folderPage->setGeometry(rect());
    folderPage->refresh();
    // Rows added by refresh take the theme too.
    setDark(dark);
    folderPage->show();
    folderPage->raise();
}
}
