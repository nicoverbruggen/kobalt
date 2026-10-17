#pragma once
#include "metrics.h"
#include <QFont>

class QWidget;
namespace kobalt {
class AssetBundle;
}

namespace kobalt::ui {
// Consume verified bytes on the GUI thread before constructing reading controls.
// The registration cache owns its data and temporary copies, so Qt never
// needs the asset storage to remain mounted.
// `changed` reports replaced interface faces, which widgets drawn before
// this call must apply again.
bool initializeInterfaceFonts(const AssetBundle &assets, bool *changed = nullptr);
// Registers only the interface faces, each checked against the asset
// manifest, so the opening view draws in its own fonts before the whole
// bundle is verified. Later openings reuse the registered faces.
bool initializeOpeningFonts(const AssetBundle &assets);
// Register bundled faces; never change Nickel's application font.
QFont interfaceFont(int pixels = -1);
// How far above its line box centre a block of text must sit to look centred:
// half of the room above the first line's capitals minus the room below the
// last line's baseline. Line boxes include descender room, so centring them
// leaves the letters low.
int opticalCenterShift(const QFont &first, const QFont &last);
// Keep every widget in this window unhinted. Qt 5.2 polishes a style sheet
// font with the default hinting, which drops the interface fonts' preference.
void keepInterfaceUnhinted(QWidget *window);
QFont chromeFont(int pixels = -1);
QFont brandFont(int pixels, bool italic = false);
// Uppercase, spaced monospace for notes, counts and section captions.
QFont captionFont(int pixels = metrics::chrome::captionText);
inline QString interfaceFontFamily() {
    return interfaceFont().family();
}

// Interface text weight in steps of 1/256 em, the unit of the reading weight
// setting. Kobalt always adds three steps; IBM Plex Medium is too heavy.
inline constexpr int heavierTextSteps = 3;
// Replaces the registered faces with faces of this weight. Text drawn
// afterwards uses them; call refreshInterfaceText() for widgets already
// shown. Returns whether the faces changed. Before the first font is used,
// this only sets the weight to register.
bool setInterfaceWeight(int steps);
int interfaceWeight();
// Draws the widgets under `root` again with the current faces.
void refreshInterfaceText(QWidget *root);
// A copy of `font` that loads its faces again. Documents that keep a font
// renew it when they receive QEvent::FontChange.
QFont renewedFont(const QFont &font);
}
