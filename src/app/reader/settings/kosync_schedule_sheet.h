#pragma once
#include "ui/dialogs/modals.h"
#include "sync/kosync_schedule.h"
#include <QPointer>
#include <QPair>
#include <QVector>
#include <functional>

class QLabel;
class QPushButton;

namespace kobalt::ui {
class DropdownButton;
}

namespace kobalt::reader {
class KosyncWifiTile;
class MomentCheck;

// Automatic sync in one sheet: whether it is on, when Kobalt syncs and
// whether a sync while reading may turn on Wi-Fi. Choices apply at once.
// Close or a tap outside dismisses it. With automatic sync off, the rest
// stays visible in gray, so the reader still sees what it would do.
class KosyncScheduleSheet final : public ui::ModalOverlay
{
public:
    // What the host platform can do. A moment or choice it lacks is left out.
    struct Features
    {
        bool sleepWake = false;
        bool wifi = false;
    };

    KosyncScheduleSheet(QWidget *parent, bool automatic, const kosync::Schedule &schedule,
                        Features features);
    ~KosyncScheduleSheet() override;
    void setChoices(bool automatic, const kosync::Schedule &schedule);
    // Sizes the card for the parent. A short screen pages the rows.
    void fit();
    void dismissIntervals();
    std::function<void(bool)> automaticChanged;
    std::function<void(const kosync::Schedule &)> scheduleChanged;
    std::function<void()> closed;
    static QString intervalLabel(int minutes);
    // The suffix of the interval's strings and object names.
    static QString intervalKey(int minutes);

private:
    void showIntervals();
    void present();
    bool automatic_;
    kosync::Schedule schedule_;
    QPushButton *off_, *on_;
    // Rows that gray out while automatic sync is off.
    QVector<QWidget *> details_;
    ui::DropdownButton *interval_;
    QVector<QPair<MomentCheck *, bool kosync::Schedule::*>> checks_;
    QWidget *wifi_ = nullptr;
    KosyncWifiTile *wait_ = nullptr, *wake_ = nullptr;
    QPointer<ui::ModalOverlay> intervals_;
};
} // namespace kobalt::reader
