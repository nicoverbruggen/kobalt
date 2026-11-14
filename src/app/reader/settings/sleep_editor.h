#pragma once
#include "reading/sleep_screen.h"
#include "ui/controls/panels.h"

#include <QHash>
#include <QImage>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <functional>

class QLabel;
class QPushButton;

namespace kobalt::ui {
class CellButton;
class DescribedOption;
class InterfaceLabel;
} // namespace kobalt::ui

namespace kobalt::reader {
class SleepEditorPreview;

// One full-screen page for two jobs, so moving a status works the same in
// both. The Custom editor chooses a backdrop and a status for sleep and for
// power off. The arrange page, opened by holding a design in the gallery,
// moves that design's status. Both show the screen large, with a dashed area
// for every other place the status can go.
class SleepEditor : public ui::Panel
{
public:
    struct Setup
    {
        // False edits `custom`; true arranges `preset`.
        bool arrange = false;
        sleep::Custom custom{};
        sleep::Look preset{};
        // Arrange only: the position belongs to the open book.
        bool bookOnly = false;
        // Text and theme for the preview, as the gallery draws it.
        sleep::Snapshot sample{};
        QSize screen{};
        // The current place of each status design, by `int(Design)`.
        QHash<int, sleep::Place> places{};
    };

    SleepEditor(QWidget *parent, Setup setup);
    ~SleepEditor() override;

    void setImages(const QImage &cover, const QImage &page, const QImage &overlay);
    // A cover that finished decoding after the editor opened.
    void setCover(const QImage &cover);
    // The files in the wallpaper folders, sorted.
    void setWallpapers(const QStringList &names);
    // A decoded wallpaper; an empty name is the random one.
    void setWallpaper(const QString &name, const QImage &image);

    sleep::Custom custom() const { return setup_.custom; }

    // The places the reader changed, by status design.
    QHash<int, sleep::Place> moved() const { return moved_; }

    bool bookOnly() const { return setup_.bookOnly; }

    bool arranging() const { return setup_.arrange; }

    // The status design the arrange page moves.
    sleep::Design arranged() const { return sleep::statusDesign(setup_.preset); }

    // Done saves; there is no cancel, as in the rest of Settings.
    std::function<void()> done;
    // The question cell in the Custom editor's header: where wallpapers go.
    std::function<void()> help;
    // Asks the reader to decode a wallpaper and to list the folder.
    std::function<void(const QString &name)> wallpaperWanted;

private:
    friend class SleepEditorPreview;
    sleep::Layer &layer();
    const sleep::Layer &layer() const;
    sleep::Look look() const;
    sleep::Place place() const;
    void move(sleep::Place place);
    void stepBackdrop(int direction);
    void stepStatus(int direction);
    void refresh();
    QImage wallpaper();

    Setup setup_;
    bool powerOff_ = false;
    QImage cover_, page_, overlay_;
    QStringList wallpapers_;
    QHash<QString, QImage> wallpaperImages_;
    QSet<QString> wallpaperRequests_;
    QHash<int, sleep::Place> moved_;
    SleepEditorPreview *preview_ = nullptr;
    QWidget *stateRow_ = nullptr;
    QPushButton *sleepingTab_ = nullptr, *poweredOffTab_ = nullptr;
    ui::InterfaceLabel *backdropValue_ = nullptr, *backdropCount_ = nullptr;
    ui::InterfaceLabel *statusValue_ = nullptr, *statusCount_ = nullptr;
    ui::DescribedOption *same_ = nullptr, *bookOnly_ = nullptr;
    ui::CellButton *done_ = nullptr, *help_ = nullptr;
};
} // namespace kobalt::reader
