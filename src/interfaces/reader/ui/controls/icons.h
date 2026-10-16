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
    Desktop,
    Dictionaries,
    About,
    System,
    RotationBoth,
    Swipe
};
// Paint vectors at the target resolution without an intermediate icon bitmap.
void paintOpeningIcon(QPainter &painter, ControlIcon icon, const QRectF &bounds, QColor color);
QIcon controlIcon(ControlIcon icon, QColor color = Qt::black, bool sideButtons = false);
// These Qt-facing helpers configure shared Button instances.
// Original asset selection leaves the button painter unchanged.
void buttonIcon(QPushButton *button, ControlIcon icon);
void refreshButtonIcon(QPushButton *button);
void iconAction(QPushButton *button, ControlIcon icon, const QString &label);
enum class ButtonPaintMode { Native, OutlinedAction, ToolbarSquare, ToolbarCell };
enum class IconRendition { OriginalAsset, Action };
struct ButtonIcon {
    ButtonIcon(ControlIcon glyph = ControlIcon{}, IconRendition rendition = IconRendition::OriginalAsset,
               bool rotationSideButtons = false)
        : glyph(glyph), rendition(rendition), rotationSideButtons(rotationSideButtons) {}
    ControlIcon glyph;
    IconRendition rendition = IconRendition::OriginalAsset;
    bool rotationSideButtons = false;
    bool operator==(const ButtonIcon &other) const {
        return glyph == other.glyph && rendition == other.rendition && rotationSideButtons == other.rotationSideButtons;
    }
};

}
