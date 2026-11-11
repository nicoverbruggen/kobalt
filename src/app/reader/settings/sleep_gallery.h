#pragma once
#include "reading/sleep_screen.h"
#include "ui/controls/panels.h"

#include <QHash>
#include <QImage>
#include <QVector>
#include <functional>

class QLabel;

namespace kobalt::ui {
class CellButton;
class PagerBar;
} // namespace kobalt::ui

namespace kobalt::reader {
class SleepGalleryGrid;

// Settings > System > Sleep and power-off screen opens this full-screen page.
// It shows every sleep screen as a small preview, four to a page, drawn with
// the open book's cover and the reader's own progress. Tapping one chooses it.
class SleepGallery : public ui::Panel
{
public:
    struct Entry
    {
        sleep::Design design;
        bool enabled = true;
    };

    // `sample` carries the text and theme the previews draw; its image is unused.
    SleepGallery(QWidget *parent, QVector<Entry> entries, sleep::Design current,
                 const sleep::Snapshot &sample, QSize screen, sleep::NativePreview native = {});
    ~SleepGallery() override;

    // Images that arrive later redraw the previews that use them.
    void setCover(const QImage &cover);
    void setPage(const QImage &page);
    void setOverlay(const QImage &overlay);
    void setEnabled(sleep::Design design, bool enabled);
    // New text, such as a changed greeting, redraws every preview.
    void setSample(const sleep::Snapshot &sample);

    std::function<void(sleep::Design)> chosen;
    // A tap on an unavailable entry, so the reader can say why.
    std::function<void(sleep::Design)> unavailable;
    std::function<void()> closed;
    // A long press on an entry with options. Only Night has one: its greeting.
    std::function<void(sleep::Design)> customize;

    static QString label(sleep::Design design);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    friend class SleepGalleryGrid;
    void showPage(int page);
    QImage preview(const Entry &entry, QSize size);
    int pageCount() const;

    QVector<Entry> entries_;
    sleep::Design current_;
    sleep::Snapshot sample_;
    sleep::NativePreview native_;
    QImage cover_, pageImage_, overlay_;
    QSize screen_;
    QHash<int, QImage> previews_;
    QLabel *title_;
    ui::CellButton *close_;
    SleepGalleryGrid *grid_;
    ui::PagerBar *pager_;
    int pageIndex_ = 0;
};
} // namespace kobalt::reader
