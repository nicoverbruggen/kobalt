#pragma once
#include <QMargins>
#include <QRect>
#include <QSize>
#include <QString>
#include <QtGlobal>
#include "scale.h"

namespace kobalt::ui::metrics {
// Sizes are pixels on a 300 ppi screen and scale to the device's density; see
// scale.h. Counts and times stay plain numbers. Keep these
// independent of Qt version and firmware. Icon paths use their own fixed
// drawing coordinates; book typography and refresh rectangles are not UI metrics.
namespace lightSwipe {
inline constexpr Px width{580}, height{252}, inset{64}, iconWidth{140}, iconHeight{156}, gap{48};
inline constexpr Px percentText{78}, captionText{32}, captionGap{16};
inline constexpr int hideMilliseconds = 850;
}
namespace spacing {
inline constexpr Px tight{8}, row{16}, section{24}, gutter{48};
}
namespace text {
inline constexpr auto explanationLight = "#333333", explanationDark = "#cccccc";
inline constexpr Px small{32}, secondary{30}, explanation{32}, body{36};
inline constexpr Px choice{36}, heading{36}, opening{40}, typography{44}, pageHeading{48};
}
namespace control {
inline constexpr Px border{2}; // Two pixels at 300 ppi, one at the Nia's 212 ppi.
inline constexpr auto lightOutline = "#666666", darkOutline = "#bbbbbb";
inline constexpr Px minimumHeight{60}, padding{4};
inline constexpr Px captionValueText{24}, captionWidth{280}, valueWidth{84}, captionGap{8}, captionRight{12};
inline constexpr Px rowGap{16}, sectionGap{8}, itemGap{16}, fontPickerGap{16};
inline constexpr Px segmentHeight{48}, segmentVerticalPadding{12}, segmentHorizontalPadding{16};
inline constexpr Px stepSize{72}, stepIconInset{10};
inline constexpr Px sliderHeight{72}, sliderInset{25}, sliderRadius{23}, sliderInnerRadius{16};
inline constexpr Px sliderFillHeight{4};
inline constexpr Px sliderMarkerTop{3}, sliderMarkerHeight{8}, sliderMarkerWidth{2};
inline constexpr Px iconCanvas{64}, actionInkSize{52}, closeInkSize{44}, iconSize{48}, chapterIconSize{44};
inline constexpr Px iconButtonWidth{72}, typographyButtonWidth{72}, buttonRadius{0};
inline constexpr Px toolbarButtonSize{68};
inline constexpr Px elidingIconGap{16};
inline constexpr Px outlinedHeight{54}, outlinedVerticalPadding{8}, outlinedHorizontalPadding{24};
}
namespace panel {
inline constexpr PxMargins toolbarMargins{16, 12, 16, 12},
    footerMargins{spacing::gutter, Px{16}, spacing::gutter, Px{16}};
inline constexpr PxMargins margins{32, 32, 32, 32}, rowsMargins{16, 0, 16, 0};
inline constexpr PxMargins contentMargins{margins.left() + rowsMargins.left(), spacing::section,
                                          margins.right() + rowsMargins.right(), spacing::section};
inline constexpr Px gap{8}, toolbarGap{18}, footerGap{18}, toolbarHeight{152}, footerHeight{280};
inline constexpr Px exitLabelExtraRight{6};
inline constexpr PxMargins statusMargins{12, 0, 15, 0};
inline constexpr Px statusHeight{40}, statusGap{8}, statusText{30}, sessionText{32};
inline constexpr Px batteryWidth{132}, batteryIconSize{56}, batteryText{24};
inline constexpr Px bluetoothIconSize{32}, bluetoothGap{12};
inline constexpr Px batteryTextInset{68}, batteryBodyWidth{46}, batteryBodyHeight{24};
inline constexpr qreal batteryStroke = 1.5;
inline QRect toolbar(const QSize &size) {
    return {0, 0, size.width(), toolbarHeight};
}
inline QRect footer(const QSize &size) {
    return {0, size.height() - footerHeight, size.width(), footerHeight};
}
}
namespace light {
inline constexpr auto hintPaperLight = "#e6e6e6", hintPaperDark = "#222222";
inline constexpr int pollMilliseconds = 500;
}
namespace sleep {
inline constexpr Px inset{48}, bottom{96}, width{620}, padding{36}, gap{20}, lineGap{8}, border{2};
inline constexpr Px titleText{60}, detailText{32}, batteryText{28}, batteryGap{12};
inline constexpr int dimAlpha = 40;
inline constexpr auto lightPaper = "#ffffff", darkPaper = "#202020";
}
namespace settings {
inline constexpr PxMargins displayMargins{16, 24, 16, 24}, screenMargins{16, 24, 16, 24};
inline constexpr PxMargins miscellaneousMargins{16, 24, 16, 24};
inline constexpr Px columnGap{28}, rowGap{16}, screenGap{16}, miscellaneousGap{24};
inline constexpr int tabBorder = 1;
inline constexpr Px tabHeight{56}, tabVerticalPadding{8}, tabHorizontalPadding{24};
inline constexpr Px optionHeight{116}, displayOptionHeight{116};
inline constexpr Px checkboxSize{40}, checkboxTop{4}, labelLeft{0}, toggleWidth{80}, toggleHeight{48}, toggleGap{24};
inline constexpr Px descriptionTop{48}, descriptionBottom{16};
}
namespace about {
inline constexpr PxMargins margins{16, 16, 16, 16};
inline constexpr Px logoSize{112}, wordmarkText{72}, exitText{40};
inline constexpr Px gap{20}, textWidth{760}, actionHeight{48}, actionGap{20};
// Version, Qt build and commit form one block with tighter lines.
inline constexpr Px buildGap{4}, commitText{24};
}
namespace navigation {
inline constexpr Px headingTop{20}, headingSide{224}, height{control::stepSize};
inline constexpr Px backLeft{16}, backWidth{192};
inline constexpr Px arrowLeft{8}, arrowSize{48};
inline constexpr Px labelLeft{64}, labelRight{8};
inline constexpr Px margin{spacing::gutter}, compactMargin{control::border}, buttonWidth{control::stepSize},
    compactButtonWidth{control::stepSize};
inline constexpr Px bottom{88}, compactBottom{80};
}
namespace choices {
inline constexpr Px pickerHeader{112}, pickerRow{144};
inline constexpr Px fontArrowSize{40};
inline constexpr Px textInset{28};
inline constexpr Px fontTop{0}, fontReservedHeight{96}, fontRowHeight{72}, fontSectionHeight{56};
inline constexpr Px fontDropdownHeight{600}, fontButtonHeight{72};
inline constexpr Px fontButtonLeft{24}, fontButtonWidthReserve{104}, fontArrowRight{38};
inline constexpr Px sampleBase{34}, sampleInkHeight{25}, sampleMinimum{28}, sampleMaximum{48};
inline int fontRows(int height) {
    return qMax(1, (height - fontReservedHeight) / fontRowHeight);
}
}
namespace list {
inline constexpr Px oneLine{96}, twoLines{146}, threeLines{168};
}
namespace chrome {
// Kobalt navigation: the header, the bottom tabs and the Settings rows.
// Full-screen views and sheets reuse these so they match the reading chrome.
inline constexpr auto dividerLight = "#b8b8b8", dividerDark = "#666666";
inline constexpr Px statusHeight{60}, actionHeight{112}, heavyRule{3};
inline constexpr Px headerHeight{statusHeight + actionHeight};
inline constexpr Px cellWidth{118}, cellIcon{40}, backIcon{44}, inset{36};
inline constexpr Px labelText{30}, descriptionText{25}, countText{28}, tabText{26}, titleText{about::exitText};
// A bottom tab centers its icon and label as one group.
inline constexpr Px tabIcon{48}, tabGap{10};
inline constexpr Px captionText{22}, captionSpacing{2}, numberText{22};
inline constexpr Px listActionHeight{list::twoLines}, optionHeight{186}, checkSize{48};
// Settings tabs stack an icon over a label, as the bottom tabs do. The rail
// is as wide as its widest label with this margin on each side.
inline constexpr Px railRowHeight{120}, railInset{28};
inline constexpr Px chevronRight{42}, actionRight{112}, statsTopPadding{heavyRule};
inline constexpr Px queryText{36}, selectorGap{16}, selectorIcon{36};
// A label and its description, or a caption and the text below it.
inline constexpr Px textGap{12}, blockTop{28};
}
namespace chapters {
// Stats: the chapter row, the chart of every chapter and three figure cells.
inline constexpr Px rowHeight{120}, captionTop{20}, titleTop{60}, chartHeight{248};
inline constexpr Px barsTop{30}, barsHeight{150}, tickGap{4}, tickHeight{8}, tickWidth{2}, numberTop{14};
// Cells have the same padding above the caption and below the detail, which
// may take two lines on narrow screens.
inline constexpr int detailLines = 2;
inline constexpr Px cellPadding{30}, figureBaseline{129}, detailTop{156}, cellHeight{253};
inline constexpr Px figureText{72}, unitGap{12}, unitText{chrome::descriptionText};
inline constexpr Px graphHeight{rowHeight + chartHeight + cellHeight};
// Dense charts show every chapter. Past that, and in the sparse chart, a pager
// bar appears between the chart and the cells.
inline constexpr int wideChapters = 30, mediumChapters = 60, sparseBars = 20, scaleCap = 3;
inline constexpr Px gapWide{8}, gapMedium{6}, gapNarrow{4};
inline constexpr Px minimumBarWidth{6}, maximumBarWidth{64}, narrowBarWidth{16};
inline constexpr Px sparseGap{12};
inline constexpr Px outline{2}, fillInset{4}, narrowFillInset{2};
}
namespace list {
inline constexpr Px headerHeight{chrome::statusHeight}, inset{chrome::inset}, gap{chrome::textGap};
inline constexpr Px leadingWidth{72}, lineInset{18};
}
namespace contents {
inline constexpr Px stripHeight{list::headerHeight};
inline constexpr Px pageGap{24}, indent{40};
// Exit Kobalt ignores taps this soon after the list closes from the same spot.
// A first guess at a double tap on e-ink; not yet measured on a device.
inline constexpr int exitGuardMilliseconds = 400;
}
namespace reading {
// Reading chrome keeps its original default, independently of control text.
inline constexpr Px defaultHeaderText{24}, defaultFooterText{26};
inline int textPixels(const QString &size, bool footer) {
    const int base = (footer ? defaultFooterText : defaultHeaderText).design();
    return px(base + (size == "large" ? 4 : size == "medium" ? 2 : 0));
}
inline QString textSize(const QString &size) {
    return size == "medium" || size == "large" ? size : QString("small");
}
inline int stripHeight(const QString &size) {
    return px(size == "large" ? 80 : size == "medium" ? 64 : 48);
}
inline constexpr Px progressHeight{24};
inline constexpr Px headerSide{28}, headerTextGap{24}, batteryGap{8};
inline constexpr Px progressBottom{14}, progressFillHeight{4};
}
namespace headerFooter {
// The slot map is a short page: one-line slot rows, their text at the top
// and bottom edges as on the page, and a few lines between them.
inline constexpr Px mapRowHeight{list::oneLine}, mapLineHeight{4};
inline constexpr Px mapLineGap{spacing::section}, mapLineInset{list::inset}, mapEdgeInset{10};
// Each slot is a dashed box around its text, so it reads as something to tap.
inline constexpr Px slotGap{8}, slotPadding{8}, slotDash{4}, mapSideInset{7};
inline constexpr Px valueText{control::captionValueText}, emptyText{chrome::captionText};
// Preset miniatures: a page, then its name in a block as tall as a segment.
inline constexpr Px miniatureWidth{184}, selectedBorder{5};
inline constexpr Px miniatureTop{spacing::section}, nameGap{spacing::row}, nameHeight{control::segmentHeight};
inline constexpr Px namePadding{control::segmentHorizontalPadding};
// Field rows are one-line list rows, as in Chapters. Described fields grow.
inline constexpr Px fieldRowHeight{list::oneLine};
// Dashes stand for slot text inside a miniature; they are drawing sizes.
inline constexpr Px dashHeight{6}, shortDash{22}, longDash{46};
// A sheet that would stop this close below the toolbar meets it instead, so
// the two heavy rules do not read as one thick line.
inline constexpr Px toolbarSnap{spacing::section};

}
namespace opening {
inline constexpr int hintDelayMilliseconds = 4000;
inline constexpr qreal referenceWidth = 402, referenceHeight = 543;
inline constexpr qreal brandText = 40.5, labelText = 14.25, hintText = 8;
inline const QRectF icon{172, 166, 58.5, 63};
inline constexpr qreal iconBarHeight = 6.5, iconBarPitch = 11.3, finalBarWidth = 42.3;
inline const QRectF brand{100, 248, 202, 58};
inline const QRectF label{60, 418, 282, 24};
inline const QRectF progress{102, 457, 198, 4.5};
inline const QRectF hint{60, 474, 282, 18};
inline constexpr qreal failureText = 23, handoffText = 19, detailText = 13;
inline const QRectF failureBrandIcon{172, 58, 58.5, 63};
inline const QRectF failureBrand{100, 139, 202, 58};
inline const QRectF failureIcon{183, 255, 36, 36};
inline const QRectF failureLabel{28, 301, 346, 32};
inline const QRectF failureDetail{28, 341, 346, 48};
inline const QRectF fallbackButton{28, 407, 346, 44};
inline const QRectF backButton{28, 459, 346, 44};
inline const QRectF handoffIcon{183, 384, 36, 36};
inline const QRectF handoffLabel{28, 429, 346, 28};
inline const QRectF handoffDetail{28, 463, 346, 24};
inline const QRectF handoffHint{28, 499, 346, 18};
}
namespace notice {
inline constexpr Px cardWidth{760}, height{144}, sideClearance{32};
inline constexpr PxMargins cardMargins{32, 28, 32, 28};
inline constexpr Px cardGap{20};
inline constexpr Px progressHeight{32};
inline QRect centered(const QSize &size) {
    const int w = qMin<int>(cardWidth, qMax(1, size.width() - 2 * sideClearance));
    return {(size.width() - w) / 2, (size.height() - height) / 2, w, height};
}
}
namespace acknowledgments {
inline constexpr Px textMargin{12}, noticeText{24}, captionGap{40};
}
namespace lookup {
inline constexpr int heightPercent = 46, holdMilliseconds = 500;
inline constexpr Px moveTolerance{40};
inline constexpr int choiceRows = 6;
inline constexpr Px choiceHeight{list::oneLine};
}

namespace links {
inline constexpr Px returnWidth{400}, returnHeight{120}, popupWidth{440}, actionHeight{96};
// Link discovery. Above the tabs its row matches the return row. The link
// indicator sits in the footer strip and keeps the strip's full height as its
// touch area.
inline constexpr Px hintGap{24}, stripButtonInset{6}, stripButtonPadding{16};
// Revealed links: a tint and an outline around each link rectangle. Stacked
// links keep a gap between their outlines.
inline constexpr Px revealPadding{6}, revealGap{6}, revealOutline{2};
inline constexpr auto revealLight = "#dddddd", revealDark = "#333333";
}

namespace search {
inline constexpr Px clearWidth{64}, clearIcon{32}, clearRight{20};
inline constexpr int excerptLines = 2;
inline constexpr Px excerptText{32}, captionTop{list::lineInset}, excerptTop{58}, rowBottom{list::lineInset};
inline constexpr int excerptLineHeight = 130; // Percentage, independent of screen density.
inline constexpr Px emptyTop{48}, emptyBottom{40};
}

}
