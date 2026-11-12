#pragma once
#include "sliders.h"
#include <QImage>
#include <QPair>
#include <QTimer>

class QLabel;
class QPushButton;

namespace kobalt::ui {
class Panel;
class CaptionButton;
class PageThumbnail;

// Receives page images and chapter positions from its owner. Preview rendering
// never moves the book; navigation callbacks report the requested move.
class PageScrubber : public QWidget
{
public:
    explicit PageScrubber(QWidget *parent);

    SettingSlider *slider() const { return position; }

    void setBook(int page, int pages, const QVector<QPair<int, QString>> &chapters, QSize pageSize);
    void setChapterJumps(bool enabled);
    void setChapterBar(bool enabled);
    void setChapterPosition(int index, int count, QString title, int percent, int bookPage,
                            int bookPages, const QVector<int> &chapterPages);
    void setBookmarkedPages(const QVector<int> &pages);
    void setCurrentPageBookmarked(bool marked);
    void setPreview(int page, QImage image, bool bookmarked = false);
    void cancel();
    void beginNavigation();
    void showCurrentPage();
    void turnPreview(int direction);

    void setPreviewDismissible(bool dismissible) { previewDismissible = dismissible; }

    void setLinksAction(const QString &label);
    void setReturnAction(const QString &label, const QString &note);
    // Shows beside Return when both actions fit without shortening their labels.
    void setImageAction(const QString &label);
    void setBarVisible(bool visible);

    bool hasActions() const { return actionsHeight() > 0; }

    bool selecting() const { return candidate >= 0; }

    int selectedPage() const { return candidate; }

    bool previewReady() const { return ready; }

    QRect previewBounds() const;
    QRect barBounds() const;
    QRect panelBounds() const;
    std::function<void(int, QSize)> previewRequested;
    std::function<void(int)> accepted;
    std::function<void(int)> chapterRequested, pageRequested;
    std::function<void()> cancelled;
    std::function<void()> linksRequested, returnRequested, stayRequested;
    std::function<void()> imageRequested, imageDismissed;

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
    bool event(QEvent *) override;

private:
    SettingSlider *position;
    SettingSlider *indicator;
    int bookPercent = 0;
    int chapterIndex = -1, chapterCount = 0;
    QString chapterTitle;
    QPushButton *previous, *next, *zoomOut, *zoomIn, *overviewToggle;
    RangeOverview *overview;
    QLabel *rangeLabel, *overviewLabel, *rangeStart, *rangeEnd, *overviewStart, *overviewEnd;
    QLabel *chapterLabel;
    Panel *barRule, *linksRule, *returnRule, *imageRule, *captionBackground, *overviewRule;
    QWidget *actions;
    CaptionButton *linksAction, *returnAction, *imageAction;
    QPushButton *stay, *dismissImage;
    QString imageLabel;
    QString requestedLinksLabel, requestedReturnLabel, requestedReturnNote, requestedImageLabel;
    QTimer renderTimer;
    QVector<QPair<int, QString>> chapters;
    QVector<int> bookmarkedPages;
    int current = 0, candidate = -1, totalPages = 1;
    int zoomLevel = 0, rangeFirst = 0, rangeLast = 0;
    QImage preview;
    PageThumbnail *preceding, *following;
    int pendingPreview = -1;
    // Retain the overlapping pages while a new strip is prepared. Side cards
    // use the middle card's resolution so promotion does not enlarge a small image.
    QImage prepared[3];
    bool requested[3] = {};
    bool preparedBookmarks[3] = {};
    int displayedPage = -1;
    bool ready = false;
    bool navigationPending = false;
    bool previewDismissible = true;
    QSize pageSize{ 3, 4 };
    QRect linksFrame, returnFrame, imageFrame;
    QPoint pressPosition;
    bool pointerActive = false, pointerMoved = false;
    int pressedCard = -1;
    bool chapterBar = false, currentPageBookmarked = false;
    bool chapterJumps = false, barVisible = true, relocating = false, overviewExpanded = false;
    void choose(int page);
    void step(int direction);
    void updateButtons();
    int maximumZoomLevel() const;
    int initialZoomLevel() const;
    int zoomSpan(int level) const;
    bool zoomAvailable() const;
    void zoom(int direction);
    void centerRange(int page);
    void syncRange(bool chaptersChanged = false);
    void pan(int first);
    void requestPreview();
    void publishPreviews();
    void clearPreview();
    void updateMask();
    void updateCaption();
    void beginPointer(QPoint point);
    void movePointer(QPoint point);
    void release(QPoint point);
    bool pointerEvent(QEvent *event, QPoint offset = {});
    QRect imageBounds() const;
    int actionsHeight() const;
    void syncActions();
    void arrange();
};
} // namespace kobalt::ui
