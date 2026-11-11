#pragma once
#include <QColor>
#include <QIcon>
#include <QRectF>
#include <QString>

class QPainter;
class QPushButton;

namespace kobalt::ui {
enum class ControlIcon {
    Search,
    Light,
    Settings,
    Layout,
    Close,
    Chapters,
    Compass,
    Graph,
    Previous,
    Next,
    Up,
    Down,
    Minus,
    Plus,
    AlignLeft,
    AlignJustify,
    Portrait,
    Landscape,
    InvertedPortrait,
    InvertedLandscape,
    Back,
    Typography,
    More,
    Debug,
    Bookmark,
    Lock,
    FileWarning,
    Link,
    Dictionary,
    Undo,
    Download,
    Bluetooth,
    // The Settings tabs. System belongs to the sleep screen settings.
    Display,
    Advanced,
    Styling,
    Desktop,
    Dictionaries,
    About,
    System,
    RotationBoth,
    Swipe,
    PreviousChapter,
    NextChapter,
    Annotate,
    SelectionStart,
    SelectionEnd,
    SaveHighlight,
    ClearSelection,
    ExtendSelection,
    EditHighlight,
    RemoveHighlight,
    OneColumn,
    TwoColumns,
    Sort,
    ArrowDownRight,
    // The library's views, and menu actions.
    Rows,
    Grid,
    Home,
    Check,
    Trash,
    FolderPlus,
    Folders,
    Export,
    Sync,
    WifiNone,
    WifiLow,
    WifiMedium,
    WifiHigh,
    WifiDisconnected,
    KeyboardShift,
    KeyboardCapsLock,
    KeyboardBackspace,
    KeyboardReturn,
    // Styling settings rows.
    SpreadHorizontal,
    Paragraph,
    SpreadVertical,
    MagicWand,
    Hidden,
    WidowsOrphans,
    HangingPunctuation,
    Star,
    Map,
    Pin,
    PinOff,
    Flag,
    Heart,
    Note,
    Lightbulb,
    Globe,
    Mountains,
    Tree,
    Moon,
    Clock,
    Calendar,
    Person,
    People,
    Crown,
    Key,
    Sword,
    Quotes,
    Question,
    CheckCircle,
    Warning,
    Chat,
    Scroll,
    Image,
    Books,
    Castle,
    Anchor,
    Paw,
    Flower,
    Coffee,
    Music,
    Rocket,
    Planet,
    Lightning,
    Flame,
    Skull,
    Diamond,
    BookmarkAdd,
    BookmarkRemove,
    ExportAction,
    FitImage,
    Door,
    Redirect
};
// Paint vectors at the target resolution without an intermediate icon bitmap.
void paintOpeningIcon(QPainter &painter, ControlIcon icon, const QRectF &bounds, QColor color);
// Use 90% of Bluetooth's visible height, centered on its intersection.
void paintWifiStatusIcon(QPainter &painter, ControlIcon icon, const QRectF &bounds, QColor color);
QIcon controlIcon(ControlIcon icon, QColor color = Qt::black, bool sideButtons = false);
// How far to move an icon's square box so its ink, not the box, is centred.
int iconInkOffset(ControlIcon icon, int size);
QStringList bookmarkIconNames();
ControlIcon bookmarkIcon(const QString &name);
// These Qt-facing helpers configure shared Button instances.
// Original asset selection leaves the button painter unchanged.
void buttonIcon(QPushButton *button, ControlIcon icon);
void refreshButtonIcon(QPushButton *button);
void iconAction(QPushButton *button, ControlIcon icon, const QString &label);
enum class ButtonPaintMode { Native, OutlinedAction, ToolbarSquare, ToolbarCell };
enum class IconRendition { OriginalAsset, Action };

struct ButtonIcon
{
    ButtonIcon(ControlIcon glyph = ControlIcon{},
               IconRendition rendition = IconRendition::OriginalAsset,
               bool rotationSideButtons = false)
        : glyph(glyph), rendition(rendition), rotationSideButtons(rotationSideButtons)
    {
    }

    ControlIcon glyph;
    IconRendition rendition = IconRendition::OriginalAsset;
    bool rotationSideButtons = false;

    bool operator==(const ButtonIcon &other) const
    {
        return glyph == other.glyph && rendition == other.rendition
                && rotationSideButtons == other.rotationSideButtons;
    }
};

} // namespace kobalt::ui
