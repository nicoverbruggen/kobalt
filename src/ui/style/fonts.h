#pragma once
#include "metrics.h"
#include <QFont>
#include <functional>
#include <memory>

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
struct OpeningFontData;
using OpeningFonts = std::shared_ptr<const OpeningFontData>;
// Read and check the interface faces on the storage worker after bundle verification.
// A superseded storage operation stops before reading the next face.
OpeningFonts readOpeningFonts(const AssetBundle &assets, const std::function<bool()> &current = {});
// On the GUI thread, stage the result for this bundle. An empty result releases it.
void setPreparedOpeningFonts(const AssetBundle &assets, OpeningFonts fonts);
// Whether the bundle's current verification has interface faces that need no
// file read: staged by the worker or already registered.
bool openingFontsPrepared(const AssetBundle &assets);
// Register bundled faces; never change Nickel's application font.
QFont interfaceFont(int pixels = -1);
// Empty families restore the bundled interface and strip defaults.
void setInterfaceFontFamily(const QString &family);
void setInterfaceMonoFontFamily(const QString &family);
void setHeaderFooterFontFamily(const QString &family);
QString defaultHeaderFooterFontFamily();
QFont headerFooterFont(int pixels = -1);
void setHeaderFooterAddedWeight(int steps);
bool interfaceFontsMatch(const QString &mainFace, const QString &monoFace, const QString &stripFace,
                         int stripWeight);
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
// Draws the widgets under `root` again with the current faces. Pass
// `restyled` when `root` has just been given a changed style sheet, which
// already repolished every widget under it.
void refreshInterfaceText(QWidget *root, bool restyled = false);
// A copy of `font` that loads its faces again. Documents that keep a font
// renew it when they receive QEvent::FontChange.
QFont renewedFont(const QFont &font);
} // namespace kobalt::ui
