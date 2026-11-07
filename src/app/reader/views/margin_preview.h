#pragma once
#include <QImage>
#include <QWidget>
#include <functional>

namespace kobalt::reader {
// Shows the whole page, scaled down, in the space between the toolbar and the
// Layout panel. The toolbar and the panel cover all possible vertical margins,
// so the reading page behind them cannot show these margins.
class MarginPreview final : public QWidget
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
    };

    explicit MarginPreview(QWidget *parent);
    void setContent(Content next);
    // Runs when `panel` hides or changes size, including paths that do not refresh the
    // reader. Opening a panel refreshes the reader after its stacking is final.
    void followPanel(QWidget *panel, std::function<void()> changed);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *) override;

private:
    struct ScaledImage
    {
        qint64 source = 0;
        QSize size;
        QImage image;
    };

    const QImage &scaled(ScaledImage &cache, const QImage &source, QSize target) const;
    qreal scale() const;
    QRect pageBounds() const;
    Content content;
    QWidget *panel = nullptr;
    std::function<void()> panelChanged;
    mutable ScaledImage scaledHeader, scaledFooter, scaledBody;
};
} // namespace kobalt::reader
