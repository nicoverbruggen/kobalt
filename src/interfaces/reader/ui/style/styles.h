#pragma once
#include "metrics.h"
#include "fonts.h"
#include <QString>

namespace kobalt::ui::styles {
// QSS dimensions use the same measurements as widget and painter geometry.
inline QString font(int size, bool bold = false) {
    return QString("font-size:%1px;").arg(size) + (bold ? "font-weight:bold;" : "");
}
inline QString brand(int pixels, bool italic = false) {
    const auto face = brandFont(pixels, italic);
    return QString("font-family:'%1';font-size:%2px;font-weight:400;font-style:%3;")
        .arg(face.family())
        .arg(pixels)
        .arg(italic ? "italic" : "normal");
}
// Caption labels also need captionFont() for their spacing and capitals.
inline QString caption(int pixels = metrics::chrome::captionText) {
    return QString(
               "font-family:'%1';font-size:%2px;font-weight:400;background:transparent;border:none;color:palette(placeholder-text);")
        .arg(chromeFont().family())
        .arg(pixels);
}
inline QString reader() {
    return QString(
               "QWidget{font-weight:400;font-family:\"%4\";background:palette(window);color:palette(window-text);} QPushButton{font-size:%1px;min-height:%2px;border:none;padding:%3px;} "
               "QPushButton:pressed{background:palette(midlight);} QPushButton:disabled{color:palette(mid);} QLabel{font-size:%1px;} "
               "QFrame#rrControlsTop,QFrame#rrControlsBottom,QFrame#rrMenu,QFrame#rrGeneralSettings{border:none;} "
               "QPushButton#rrReturn,QPushButton#rrChapters{font-weight:bold;} "
               "QPushButton#rrLight:checked,QPushButton#rrTypography:checked,QPushButton#rrLayout:checked,QPushButton#rrSettings:checked,QPushButton#rrGraph:checked{background:palette(window-text);color:palette(window);}")
        .arg(metrics::text::body)
        .arg(metrics::control::minimumHeight)
        .arg(metrics::control::padding)
        .arg(interfaceFontFamily());
}
inline QString segment() {
    return QString(
               "QPushButton{border-radius:0;min-height:%1px;max-height:%1px;padding:%2px %3px;border:%4px solid palette(window-text);border-left:none;background:palette(window);color:palette(window-text);} QPushButton[segment_first=\"true\"]{border-left:%4px solid palette(window-text);} QPushButton:checked{background:palette(window-text);color:palette(window);} QPushButton:pressed{background:palette(window-text);color:palette(window);}")
               .arg(metrics::control::segmentHeight)
               .arg(metrics::control::segmentVerticalPadding)
               .arg(metrics::control::segmentHorizontalPadding)
               .arg(metrics::control::border) +
           "QPushButton{" + font(metrics::text::secondary) + "}";
}
inline QString outlined() {
    return QString(
               "QPushButton{min-height:%1px;padding:%2px %3px;border:%4px solid palette(dark);} QPushButton:disabled{color:palette(mid);} QPushButton:pressed{background:palette(midlight);}")
        .arg(metrics::control::outlinedHeight)
        .arg(metrics::control::outlinedVerticalPadding)
        .arg(metrics::control::outlinedHorizontalPadding)
        .arg(metrics::control::border);
}
inline QString primary() {
    return outlined() +
           "QPushButton{background:palette(window-text);color:palette(window);} QPushButton:pressed{background:palette(midlight);color:palette(window-text);} QPushButton:disabled{background:palette(mid);color:palette(window);}";
}
inline QString textAction() {
    return font(metrics::text::secondary) +
           QString("border:none;color:%1;text-decoration:underline;padding:%3px %4px;min-height:%2px;max-height:%2px;")
               .arg("palette(placeholder-text)")
               .arg(metrics::about::actionHeight - px(8))
               .arg(px(4))
               .arg(px(12));
}
inline QString tabFrame() {
    return QString("QStackedWidget#rrSettingsPages{border:%1px solid palette(mid);border-top:none;}")
        .arg(metrics::settings::tabBorder);
}
inline QString tab(bool first) {
    return QString(
               "QPushButton{border-radius:0;min-height:%1px;padding:%2px %3px;border:%4px solid palette(mid);border-left-width:%5px;background:palette(midlight);} QPushButton:checked{background:palette(window-text);color:palette(window);padding-bottom:%2px;} QPushButton[kobalt_selected=\"true\"]{font-weight:normal;} QPushButton:pressed{background:palette(mid);}")
        .arg(metrics::settings::tabHeight)
        .arg(metrics::settings::tabVerticalPadding)
        .arg(metrics::settings::tabHorizontalPadding)
        .arg(metrics::settings::tabBorder)
        .arg(first ? metrics::settings::tabBorder : 0);
}
inline QString popup() {
    return QString(
               "QLabel{background:palette(window);color:palette(window-text);border:none;font-size:%1px;} "
               "QPushButton{background:palette(window);color:palette(window-text);font-size:%1px;padding:%2px %5px;min-height:%3px;border:%4px solid palette(dark);} "
               "QPushButton:pressed{background:palette(midlight);} QPushButton:disabled{color:palette(mid);}")
        .arg(metrics::chrome::labelText)
        .arg(metrics::control::outlinedVerticalPadding)
        .arg(metrics::control::outlinedHeight)
        .arg(metrics::control::border)
        .arg(metrics::control::outlinedHorizontalPadding);
}
inline QString dropdownChoice(bool selected, bool divider) {
    return QString("QPushButton{border-radius:0;border:none;background:%1;}")
               .arg(selected ? "palette(window-text)" : "palette(window)") +
           QString("QPushButton{color:%1;text-align:left;padding:0 %2px;min-height:0;}")
               .arg(selected ? "palette(window)" : "palette(window-text)")
               .arg(metrics::choices::textInset) +
           (divider ? QString("QPushButton{border-top:%1px solid palette(mid);}").arg(metrics::control::border)
                    : QString());
}
}
