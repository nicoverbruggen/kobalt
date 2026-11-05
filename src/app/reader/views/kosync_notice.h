#pragma once
#include "ui/dialogs/modals.h"
#include <QTimer>

namespace kobalt::reader {
class KosyncNotice final : public ui::NoticeModal
{
public:
    explicit KosyncNotice(QWidget *parent);
    void complete();

    bool completing() const { return successTimer.isActive(); }

    std::function<void()> completed;

protected:
    void hideEvent(QHideEvent *event) override;

private:
    QLabel *heading, *statusIcon;
    QTimer successTimer;
};
} // namespace kobalt::reader
