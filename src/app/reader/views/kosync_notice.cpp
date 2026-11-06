#include "kosync_notice.h"
#include "ui/controls/icons.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QStyle>
#include <QVariant>

namespace kobalt::reader {
KosyncNotice::KosyncNotice(QWidget *parent)
    : ui::NoticeModal(
              parent,
              { strings::get("kosync.opening_description"), strings::get("kosync.opening"), {} },
              { strings::get("kosync.cancel_sync"), "rrKosyncCancel" })
{
    auto *content = card();
    heading = content->findChild<QLabel *>("rrModalTitle");
    const int index = content->rows()->indexOf(heading);
    content->rows()->removeWidget(heading);
    auto *headingRow = new ui::Panel(content);
    descriptionLabel()->setAlignment(Qt::AlignCenter);
    auto *row = new QHBoxLayout(headingRow);
    const auto padding = ui::metrics::notice::cardMargins;
    row->setContentsMargins(padding.left(), padding.top(), padding.right(), 0);
    row->setSpacing(ui::px(20));
    row->addStretch(1);
    statusIcon = new QLabel(headingRow);
    statusIcon->setObjectName("rrKosyncStatusIcon");
    const int size = ui::px(48);
    statusIcon->setFixedSize(size, size);
    statusIcon->setPixmap(
            ui::controlIcon(ui::ControlIcon::Sync,
                            ui::darkTheme(this) ? QColor(Qt::white) : QColor(Qt::black))
                    .pixmap(size, size));
    row->addWidget(statusIcon, 0, Qt::AlignVCenter);
    // The heading and description share one content block above the action.
    heading->setProperty("reader_modal_content", false);
    heading->setProperty("reader_modal_heading", false);
    heading->style()->unpolish(heading);
    heading->style()->polish(heading);
    heading->setMinimumHeight(0);
    heading->setMaximumHeight(QWIDGETSIZE_MAX);
    row->addWidget(heading);
    row->addStretch(1);
    content->rows()->insertWidget(index, headingRow);
    auto *cancel = primaryButton();
    cancel->setProperty("reader_primary_action", false);
    ui::readerStyle(cancel,
                    "QPushButton{border:none;padding:0;min-height:0;font-weight:400;"
                            + ui::styles::font(ui::metrics::chrome::labelText) + "}");
    // The member timer belongs to the notice, so a check of its children finds it.
    successTimer.setParent(this);
    successTimer.setObjectName("rrKosyncSuccessTimer");
    successTimer.setSingleShot(true);
    successTimer.setTimerType(Qt::PreciseTimer);
    connect(&successTimer, &QTimer::timeout, this, [this] {
        if (completed)
            completed();
    });
}

void KosyncNotice::complete()
{
    if (successTimer.isActive())
        return;
    heading->setText(strings::get("kosync.completed"));
    descriptionLabel()->setText(strings::get("kosync.up_to_date"));
    const int size = statusIcon->width();
    statusIcon->setPixmap(
            ui::controlIcon(ui::ControlIcon::Check,
                            ui::darkTheme(this) ? QColor(Qt::white) : QColor(Qt::black))
                    .pixmap(size, size));
    primaryButton()->hide();
    dismissed = {};
    fitCard();
    successTimer.start(1500);
}

void KosyncNotice::hideEvent(QHideEvent *event)
{
    // A hidden parent can return while this completed notice still covers it.
    // Only an explicit dismissal cancels the timer.
    if (isHidden())
        successTimer.stop();
    ui::NoticeModal::hideEvent(event);
}
} // namespace kobalt::reader
