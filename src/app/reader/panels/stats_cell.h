#pragma once
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include <QWidget>

namespace kobalt {
// A caption with an optional percentage, a large figure with its unit, and one
// grey line of detail. A short screen adds the word total as a second line.
class StatsCell : public QWidget
{
public:
    StatsCell(QWidget *parent, const QString &prefix, const QString &title) : QWidget(parent)
    {
        setObjectName(prefix + "Cell");
        caption = new ui::InterfaceLabel(title, this);
        caption->setObjectName(prefix + "Caption");
        ui::captionLabel(caption);
        percent = new ui::InterfaceLabel(this);
        percent->setObjectName(prefix + "Percent");
        ui::captionLabel(percent);
        percent->setAlignment(Qt::AlignRight | Qt::AlignTop);
        caption->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        figure = new ui::InterfaceLabel(this);
        figure->setObjectName(prefix + "Figure");
        figure->setTextFormat(Qt::PlainText);
        ui::readerStyle(figure,
                        ui::styles::brand(ui::metrics::chapters::figureText)
                                + "background:transparent;");
        unit = new ui::InterfaceLabel(this);
        unit->setObjectName(prefix + "Unit");
        detail = new ui::InterfaceLabel(this);
        detail->setObjectName(prefix + "Detail");
        for (auto *label : { figure, unit, detail })
            label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        for (auto *label : { unit, detail }) {
            label->setTextFormat(Qt::PlainText);
            // twoLines() breaks a long unit or detail with '\n'. Only the
            // wrapped painting turns it into a line; the single-line painting
            // drew it as a missing glyph.
            label->setWordWrap(true);
            ui::readerStyle(label,
                            ui::styles::font(ui::metrics::chapters::unitText)
                                    + "background:transparent;color:palette(placeholder-text);");
        }
    }

    QLabel *caption, *percent, *figure, *unit, *detail;

    // A short detail replaces the full one when only the short one fits on a line.
    void set(const QString &value, const QString &unitText, const QString &detailText,
             const QString &percentText = {}, const QString &shortDetail = {},
             const QString &extraLine = {})
    {
        figure->setText(value);
        percent->setText(percentText);
        unitValue = unitText;
        detailValue = detailText;
        shortValue = shortDetail;
        extraValue = extraLine;
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *) override { arrange(); }

private:
    QString unitValue, detailValue, shortValue, extraValue;

    void arrange()
    {
        const int inner = qMax(1, width() - 2 * ui::metrics::chrome::inset);
        const QFont captionMetrics(ui::captionFont()),
                figureMetrics(ui::brandFont(ui::metrics::chapters::figureText)),
                unitMetrics(ui::interfaceFont(ui::metrics::chapters::unitText));
        caption->setGeometry(ui::metrics::chrome::inset, ui::metrics::chapters::cellPadding, inner,
                             ui::interfaceLineHeight(captionMetrics));
        percent->setGeometry(ui::metrics::chrome::inset, ui::metrics::chapters::cellPadding, inner,
                             ui::interfaceLineHeight(captionMetrics));
        const int figureWidth =
                qMin(inner, ui::interfaceTextWidth(figure->text(), figureMetrics) + 2);
        figure->setGeometry(ui::metrics::chrome::inset,
                            ui::metrics::chapters::figureBaseline
                                    - ui::interfaceLineAscent(figureMetrics),
                            figureWidth, ui::interfaceLineHeight(figureMetrics));
        const int unitLeft =
                ui::metrics::chrome::inset + figureWidth + ui::metrics::chapters::unitGap;
        // A large figure keeps every digit. A unit that does not fit beside it
        // takes two lines, the last on the figure's baseline, as in a narrow
        // cell on a phone; only then does it shorten.
        const int unitWidth = qMax(1, width() - ui::metrics::chrome::inset - unitLeft);
        const auto unitText = twoLines(unitMetrics, unitValue, unitWidth);
        const int unitLines = unitText.count('\n') + 1;
        const int lineHeight = ui::interfaceLineHeight(unitMetrics);
        unit->setGeometry(unitLeft,
                          ui::metrics::chapters::figureBaseline
                                  - ui::interfaceLineAscent(unitMetrics)
                                  - (unitLines - 1) * lineHeight,
                          unitWidth, unitLines * lineHeight);
        unit->setText(unitText);
        detail->setGeometry(ui::metrics::chrome::inset, ui::metrics::chapters::detailTop, inner,
                            ui::interfaceLineHeight(unitMetrics)
                                    + (ui::metrics::chapters::detailLines - 1)
                                            * ui::interfaceLineHeight(unitMetrics));
        const bool shorten = !shortValue.isEmpty()
                && ui::interfaceTextWidth(detailValue, unitMetrics) > inner
                && ui::interfaceTextWidth(shortValue, unitMetrics) <= inner;
        const auto text = shorten ? shortValue : detailValue;
        detail->setText(extraValue.isEmpty()
                                ? twoLines(unitMetrics, text, inner)
                                : ui::elideInterfaceText(text, unitMetrics, inner) + '\n'
                                        + ui::elideInterfaceText(extraValue, unitMetrics, inner));
    }

    // A detail that does not fit breaks at the space that gives the most even
    // lines, so "≈ 12 h 45 min left" never leaves one word alone. Without such
    // a space, the first line takes what fits and the second line shortens.
    static QString twoLines(const QFont &metrics, const QString &text, int width)
    {
        if (ui::interfaceTextWidth(text, metrics) <= width)
            return text;
        int best = -1, bestWidth = width + 1, last = -1;
        for (int space = text.indexOf(' '); space > 0; space = text.indexOf(' ', space + 1)) {
            const int first = ui::interfaceTextWidth(text.left(space), metrics);
            if (first > width)
                break;
            last = space;
            const int longer = qMax(first, ui::interfaceTextWidth(text.mid(space + 1), metrics));
            if (longer < bestWidth) {
                best = space;
                bestWidth = longer;
            }
        }
        if (best < 0)
            best = last;
        if (best < 0)
            return ui::elideInterfaceText(text, metrics, width);
        return text.left(best) + '\n' + ui::elideInterfaceText(text.mid(best + 1), metrics, width);
    }
};
} // namespace kobalt
