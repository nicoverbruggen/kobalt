#pragma once
#include <QtGlobal>
class QWidget;

namespace kobalt::nickel {
// Nickel's Large Print Mode stores its scale in the application property
// "fontScale". Kobo's Qt multiplies the pixel size of every style sheet font
// by that value when it polishes a widget. Kobalt sizes its text and the
// controls around it together, so text scaled on its own is clipped.
//
// Each holder keeps the property clear. Nickel's value returns when the last
// holder ends, and Nickel widgets restyled meanwhile are styled again with it.
// Nickel's own windows shown over Kobalt use the standard size until then.

// Holds while a Kobalt window is built, before Qt can polish it.
class NativeFontScaleScope
{
public:
    NativeFontScaleScope();
    ~NativeFontScaleScope();
    NativeFontScaleScope(const NativeFontScaleScope &) = delete;
    NativeFontScaleScope &operator=(const NativeFontScaleScope &) = delete;
};

// Holds while `window` is visible or not yet shown, until it is destroyed.
void ignoreNativeFontScale(QWidget *window);
// Gives a Nickel dialog shown over Kobalt Nickel's scale until it hides, so
// it looks as it does in Nickel. Call when Qt is about to polish the dialog,
// or when it shows; a dialog polished earlier is styled again.
void allowNativeFontScale(QWidget *dialog, bool polishing);
// The scale Large Print Mode asks for, also while a holder keeps it out of
// Qt, and 1 when it is off. Kobalt applies it to its interface text and icons
// instead; see ui::setLargeText.
qreal nativeFontScale();
} // namespace kobalt::nickel
