#pragma once
#include <QObject>
#include <QPaintEvent>
#include <QRegion>

namespace kobalt::test {
class PaintRegions final : public QObject
{
public:
    QRegion painted;
    int count = 0;

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) {
            painted += static_cast<QPaintEvent *>(event)->region();
            ++count;
        }
        return false;
    }
};
} // namespace kobalt::test
