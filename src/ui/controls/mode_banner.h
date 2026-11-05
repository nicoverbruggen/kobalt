#pragma once
#include "text.h"
#include "ui/style/fonts.h"
#include "ui/style/styles.h"
#include "support/qt_font.h"
#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QtMath>
#include <functional>

namespace kobalt::ui {
// A persistent mode title and changing instruction, independent of the page layout.
class ModeBanner : public QWidget
{
public:
    ModeBanner(const QString &title, const QString &text, QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        heading = new kobalt::ui::InterfaceLabel(title, this);
        helper = new kobalt::ui::InterfaceLabel(text, this);
        for (auto *label : { heading, helper }) {
            label->setWordWrap(true);
            label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            label->setFont(chromeFont(px(25)));
            const bool strong = label == heading;
            readerStyle(
                    label,
                    QString("QLabel{background:transparent;color:%1;font-family:'%2';font-size:%3px;font-weight:%4;}")
                            .arg(strong ? "palette(window)" : "palette(midlight)")
                            .arg(chromeFont().family())
                            .arg(px(25))
                            .arg(strong ? 700 : 400));
            label->ensurePolished();
            label->installEventFilter(this);
        }
    }

    QLabel *titleLabel() const { return heading; }

    void setText(const QString &text)
    {
        if (helper->text() == text) {
            return;
        }
        helper->setText(text);
        arrange();
        if (contentChanged) {
            contentChanged();
        }
    }

    int preferredHeight(int width) const
    {
        const int room = qMax(1, width - 2 * px(36));
        const int minimum = px(56) - int(metrics::chrome::heavyRule);
        if (fitsInline(room)) {
            return qMax(minimum,
                        qMax(heading->fontMetrics().height(), helper->fontMetrics().height())
                                + 2 * px(8));
        }
        return qMax(minimum,
                    wrappedHeight(heading, room) + px(4) + wrappedHeight(helper, room) + 2 * px(8));
    }

    std::function<void()> contentChanged;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::WindowText));
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        arrange();
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::FontChange) {
            arrange();
            if (contentChanged) {
                contentChanged();
            }
        }
        return QWidget::eventFilter(object, event);
    }

private:
    QLabel *heading = nullptr, *helper = nullptr;

    int titleWidth() const { return interfaceTextWidth(heading->text(), heading->font()); }

    bool fitsInline(int room) const
    {
        return titleWidth() + px(24) + interfaceTextWidth(helper->text(), helper->font()) <= room;
    }

    static int wrappedHeight(const QLabel *label, int room)
    {
        return qMax(label->fontMetrics().height(), label->heightForWidth(room));
    }

    void arrange()
    {
        const int inset = px(36), room = qMax(1, width() - 2 * inset);
        if (fitsInline(room)) {
            const int title = titleWidth();
            heading->setGeometry(inset, 0, title, height());
            helper->setGeometry(inset + title + px(24), 0, qMax(1, room - title - px(24)),
                                height());
        } else {
            const int title = wrappedHeight(heading, room);
            heading->setGeometry(inset, px(8), room, title);
            helper->setGeometry(inset, px(8) + title + px(4), room, wrappedHeight(helper, room));
        }
    }
};
} // namespace kobalt::ui
