#pragma once
#include <QImage>
class QPainter;

namespace kobalt::ui {
class ListHeader;
}

#include <QWidget>
#include <functional>

namespace kobalt::reader {
// Shows a preview in the space between the toolbar and the appearance panel. For
// Layout, it is the whole page, scaled down: the toolbar and the panel cover all possible
// vertical margins, so the reading page behind them cannot show these margins. For Text,
// it is the text at its true size, with whole lines only.
class AppearancePreview final : public QWidget
{
public:
    enum class Axis { None, Horizontal, Vertical };

    struct Content
    {
        QSize page;
        int bodyTop = 0, bodyHeight = 0;
        QImage header, footer;
        QImage body;
        int horizontalMargin = 0, verticalMargin = 0;
        Axis axis = Axis::None;
        // Show the page text at its true size, from cropTop in the body image.
        bool text = false;
        int cropTop = 0;
    };

    explicit AppearancePreview(QWidget *parent);
    void setContent(Content next);

    bool showsText() const { return content.text; }

    // True when the page has text in most of the space that the Text preview shows.
    bool fillsTextCrop(const QImage &body, int cropTop) const;
    // Runs when `panel` hides or changes size, including paths that do not refresh the
    // reader. Opening a panel refreshes the reader after its stacking is final.
    void followPanel(QWidget *panel, std::function<void()> changed);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    struct ScaledImage
    {
        qint64 source = 0;
        QSize size;
        QImage image;
    };

    const QImage &scaled(ScaledImage &cache, const QImage &source, QSize target) const;
    void paintCaption(QPainter &painter, int right) const;
    QRect textCard() const;
    void paintText(QPainter &painter);
    qreal scale() const;
    QRect pageBounds() const;
    Content content;
    ui::ListHeader *textHeader = nullptr;
    QWidget *panel = nullptr;
    std::function<void()> panelChanged;
    mutable ScaledImage scaledHeader, scaledFooter, scaledBody;
};
} // namespace kobalt::reader
