#pragma once
#include "metrics.h"
#include <QFont>

class QWidget;
class QLineEdit;

namespace kobalt {
class AssetBundle;
}

namespace kobalt::ui {
// Load native-editor script fallbacks only when their characters are displayed.
void prepareNativeEditor(QLineEdit *editor);
// Consume verified bytes on the GUI thread before constructing reading controls.
// Loaded faces own their bytes. Script fallback readers stay deferred until
// a native editor needs their characters.
// `changed` reports replaced interface faces, which widgets drawn before
// this call must apply again.
bool initializeInterfaceFonts(const AssetBundle &assets, bool *changed = nullptr);
// Registers only the interface faces, each checked against the asset
// manifest, so the opening view draws in its own fonts before the whole
// bundle is verified. Later openings reuse the registered faces.
bool initializeOpeningFonts(const AssetBundle &assets);
// Reads the interface faces and checks them against the manifest on a worker
// thread. The next initializeOpeningFonts() takes the result, so the first
// opening view does no storage reads for them. Call on the GUI thread. The
// worker keeps its own asset readers, so `assets` need not outlive it.
void prepareOpeningFonts(const AssetBundle &assets);
// Register bundled faces; never change Nickel's application font.
QFont interfaceFont(int pixels = -1);
// Empty families restore the bundled interface and strip defaults.
void setInterfaceFontFamily(const QString &family);
void setInterfaceMonoFontFamily(const QString &family);
void setHeaderFooterFontFamily(const QString &family);
QFont headerFooterFont(int pixels = -1);
void setHeaderFooterAddedWeight(int steps);
int headerFooterAddedWeight();
// Default keeps the native text path. Extra weight uses synthetic FreeType strokes.
int headerFooterTextWeight();
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

inline QString interfaceFontFamily()
{
    return interfaceFont().family();
}

// Interface text weight in steps of 1/256 em, the unit of the reading weight
// setting. Kobalt always adds three steps; IBM Plex Medium is too heavy.
inline constexpr int heavierTextSteps = 3;
// Sets the weight that the private renderer adds to interface text. Qt draws
// its own controls in the regular faces. Text drawn afterwards uses the new
// weight; call refreshInterfaceText() for widgets already shown. Returns
// whether the weight changed.
bool setInterfaceWeight(int steps);
int interfaceWeight();
// Draws the widgets under `root` again with the current faces.
void refreshInterfaceText(QWidget *root);
// A copy of `font` that loads its faces again. Documents that keep a font
// renew it when they receive QEvent::FontChange.
QFont renewedFont(const QFont &font);
} // namespace kobalt::ui
