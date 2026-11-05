#include "action_menu.h"
#include "ui/controls/buttons.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QTextLayout>
#include <QVBoxLayout>

namespace kobalt::ui {
namespace {
// One action: an optional icon and a label across the card's full width,
// with a divider above. A held row uses the shared gray list feedback.
class ActionRow final : public Button
{
public:
    ActionRow(const ActionMenuItem &item, bool divider, QWidget *parent)
        : Button(item.text, parent), item(item), divider(divider)
    {
        setObjectName(item.name);
        setAccessibleName(item.text);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        readerStyle(this,
                    QString("QPushButton{min-height:%1px;max-height:%1px;padding:0;border:none;}")
                            .arg(int(metrics::chrome::menuActionHeight)));
        setFixedHeight(metrics::chrome::menuActionHeight);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto colors = parentWidget() ? parentWidget()->palette() : palette();
        const auto paper = colors.color(QPalette::Window), ink = colors.color(QPalette::WindowText);
        const bool pressed = isEnabled() && isDown();
        const auto foreground = ink;
        painter.fillRect(rect(), pressed ? themeColors(this).pressedPaper : paper);
        if (divider)
            painter.fillRect(0, 0, width(), metrics::control::border, dividerColor(this));
        const int iconSize = metrics::control::iconSize, left = metrics::chrome::inset;
        if (item.hasIcon)
            controlIcon(item.icon, foreground)
                    .paint(&painter, QRect(left, (height() - iconSize) / 2, iconSize, iconSize));
        const int textLeft = left + iconSize + metrics::chrome::selectorGap;
        painter.setFont(interfaceFont(metrics::chrome::labelText));
        painter.setPen(isEnabled() ? foreground : colors.color(QPalette::Mid));
        const int chevronSpace =
                item.chevron ? int(metrics::chrome::cellIcon + metrics::chrome::selectorGap) : 0;
        const int available = qMax(1, width() - textLeft - metrics::chrome::inset - chevronSpace);
        if (item.chevron) {
            const int side = metrics::chrome::cellIcon;
            controlIcon(ControlIcon::Next, foreground)
                    .paint(&painter,
                           QRect(width() - metrics::chrome::inset - side, (height() - side) / 2,
                                 side, side));
        }
        drawOpticalText(painter, QRect(textLeft, 0, available, height()),
                        elideInterfaceText(text(), painter.font(), available));
    }

private:
    ActionMenuItem item;
    bool divider;
};

// The header measures its own text, so it is exactly as tall as the
// thumbnail or the text, whichever is taller. Wrapped labels in nested layouts
// report more height than they use.
class MenuHeader final : public QWidget
{
public:
    MenuHeader(const ActionMenuHeader &content, QWidget *parent) : QWidget(parent), content(content)
    {
        setObjectName("rrActionMenuHeader");
        setAccessibleName(QStringList({ content.title, content.detail, content.caption })
                                  .join(' ')
                                  .simplified());
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (!content.image.isNull())
            thumbnail = content.image.scaled(QSize(metrics::actionMenu::thumbnailWidth,
                                                   metrics::actionMenu::thumbnailHeight),
                                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        const auto margins = QMargins(metrics::notice::cardMargins);
        return margins.top() + qMax(thumbnail.height(), textHeight(textWidth(width)))
                + margins.bottom();
    }

    QSize sizeHint() const override
    {
        return { metrics::notice::cardWidth, heightForWidth(metrics::notice::cardWidth) };
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        const int height = heightForWidth(width());
        if (this->height() != height)
            setFixedHeight(height);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto margins = QMargins(metrics::notice::cardMargins);
        const int body = height() - margins.top() - margins.bottom();
        if (!thumbnail.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(
                    QPoint(margins.left(), margins.top() + (body - thumbnail.height()) / 2),
                    thumbnail);
        }
        const int left = textLeft(), width = textWidth(this->width());
        int y = margins.top() + (body - textHeight(width)) / 2;
        for (const auto &part : parts())
            y += draw(&painter, part, left, y, width) + metrics::chrome::textGap;
    }

private:
    struct Part
    {
        QString text;
        QFont font;
        QColor color;
        int lines;
    };

    ActionMenuHeader content;
    QImage thumbnail;

    int textLeft() const
    {
        return QMargins(metrics::notice::cardMargins).left()
                + (thumbnail.isNull() ? 0 : thumbnail.width() + metrics::notice::cardGap);
    }

    int textWidth(int width) const
    {
        return qMax(1, width - textLeft() - QMargins(metrics::notice::cardMargins).right());
    }

    QVector<Part> parts() const
    {
        QVector<Part> result;
        auto title = interfaceFont(metrics::chrome::labelText);
        title.setBold(true);
        const auto ink = palette().color(QPalette::WindowText);
        if (!content.title.isEmpty())
            result.append({ content.title, title, ink, 3 });
        if (!content.detail.isEmpty())
            result.append({ content.detail, interfaceFont(metrics::chrome::descriptionText),
                            noteColor(this), 2 });
        // A caption that does not fit, such as with large text, wraps once
        // rather than lose its end.
        if (!content.caption.isEmpty())
            result.append({ content.caption.toUpper(), captionFont(), noteColor(this), 2 });
        return result;
    }

    // Draws or, without a painter, measures one part in up to `lines` lines,
    // eliding the last. Returns its height.
    static int draw(QPainter *painter, const Part &part, int left, int top, int width)
    {
        const int height = wrappedInterfaceTextHeight(part.text, part.font, width, part.lines);
        if (painter) {
            painter->setFont(part.font);
            painter->setPen(part.color);
            drawWrappedInterfaceText(*painter, QRect(left, top, width, height), part.text,
                                     Qt::AlignLeft | Qt::AlignTop, part.lines);
        }
        return height;
    }

    int textHeight(int width) const
    {
        int height = 0;
        for (const auto &part : parts())
            height +=
                    (height ? int(metrics::chrome::textGap) : 0) + draw(nullptr, part, 0, 0, width);
        return height;
    }
};
} // namespace

ActionMenu::ActionMenu(QWidget *parent, const ActionMenuHeader &header,
                       const QVector<ActionMenuItem> &items)
    : ModalOverlay(parent)
{
    setProperty("reader_standard_modal", true);
    auto *rowsLayout = card()->rows();
    // The rows reach the card's two-pixel border; only the header keeps the card's margins.
    rowsLayout->setContentsMargins(2, 2, 2, 2);
    rowsLayout->setSpacing(0);
    const bool hasHeader = !header.image.isNull() || !header.title.isEmpty()
            || !header.detail.isEmpty() || !header.caption.isEmpty();
    if (hasHeader)
        rowsLayout->addWidget(new MenuHeader(header, card()));
    for (const auto &item : items) {
        // Without a header, the first row's divider would double the card's border.
        auto *row = new ActionRow(item, hasHeader || !rows.isEmpty(), card());
        rows.append(row);
        rowsLayout->addWidget(row);
        const auto triggered = item.triggered;
        connect(row, &QPushButton::clicked, this, [this, triggered] {
            if (dismissed)
                dismissed();
            if (triggered)
                triggered();
        });
    }
    fitCard();
}

QPushButton *ActionMenu::action(const QString &name) const
{
    for (auto *row : rows)
        if (row->objectName() == name)
            return row;
    return nullptr;
}
} // namespace kobalt::ui
