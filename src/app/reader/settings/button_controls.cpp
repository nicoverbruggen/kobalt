#include "settings_panel.h"
#include "support/strings.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include <QHBoxLayout>
#include <QPainter>
#include <memory>

namespace kobalt::reader {
namespace chrome = ui::metrics::chrome;

namespace {
QString actionLabel(int direction)
{
    return strings::get(direction < 0           ? "buttons.previous"
                                : direction > 0 ? "buttons.next"
                                                : "buttons.none");
}

class DiagramButton final : public ui::Button
{
public:
    using ui::Button::Button;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto &colors = ui::themeColors(this);
        painter.setPen(QPen(colors.ink, ui::px(2)));
        painter.setBrush(isDown() ? colors.pressedPaper : colors.paper);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), width() / 3., width() / 3.);
    }
};

// Draw shapes only. Labels and hit targets use the shared controls.
class ButtonDiagram final : public QWidget
{
public:
    ButtonDiagram(QWidget *parent, const pageButtons::Buttons &buttons, int width,
                  std::function<void(int)> choose)
        : QWidget(parent), volume(buttons.first().id.startsWith("volume_"))
    {
        setFixedSize(width, qRound(width * 350. / 260));
        const qreal scale = width / 260.;
        for (int i = 0; i < buttons.size(); ++i) {
            auto *button = new DiagramButton(this);
            button->setObjectName("rrButtonDiagram_" + buttons[i].id);
            button->setAccessibleName(strings::get(buttons[i].labelKey.toUtf8().constData()));
            const int position = volume && buttons[i].key == Qt::Key_VolumeDown ? 1 : i;
            button->setGeometry(
                    qRound((volume ? 211 : 201) * scale), qRound((100 + position * 82) * scale),
                    qRound((volume ? 10 : 24) * scale), qRound((volume ? 52 : 64) * scale));
            ui::readerStyle(button,
                            "min-width:0;min-height:0;padding:0;border:0;background:transparent;");
            connect(button, &QPushButton::clicked, this, [choose, i] { choose(i); });
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.scale(width() / 260., height() / 350.);
        const auto &colors = ui::themeColors(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(colors.ink, 2));
        p.setBrush(colors.paper);
        if (volume) {
            p.drawRoundedRect(QRect(42, 8, 172, 330), 24, 24);
            p.setPen(QPen(colors.ink, 1));
            p.drawRoundedRect(QRect(54, 26, 148, 285), 10, 10);
            p.setBrush(colors.ink);
            p.drawEllipse(QPointF(128, 17), 3, 3);
            p.drawLine(110, 325, 146, 325);
        } else {
            p.drawRoundedRect(QRect(12, 8, 224, 330), 16, 16);
            p.setPen(QPen(colors.ink, 1));
            p.drawRect(QRect(29, 25, 158, 296));
        }
        p.setPen(QPen(colors.secondaryInk, 1));
        for (int line = 0; line < 5; ++line)
            p.drawLine(volume ? 74 : 57, 134 + 17 * line,
                       line == 4 ? (volume ? 124 : 112) : (volume ? 176 : 158), 134 + 17 * line);
    }

private:
    bool volume;
};
} // namespace

void SettingsPanel::updateButtonControls(const pageButtons::Buttons &buttons,
                                         const QJsonObject &preferences, bool saving)
{
    buttonControlsButton->setVisible(!buttons.isEmpty());
    buttonControlsButton->setEnabled(!saving && !buttons.isEmpty());
    QStringList actions;
    for (const auto &button : buttons)
        actions.append(actionLabel(
                pageButtons::direction(preferences, button.id, button.defaultDirection)));
    buttonControlsButton->setCaption(actions.join(" / ").toUpper());
}

void SettingsPanel::showButtonControls(const pageButtons::Buttons &buttons,
                                       const QJsonObject &current,
                                       std::function<void(QJsonObject)> chosen)
{
    if (fontDialog || buttons.isEmpty())
        return;
    const bool volume = buttons.first().id.startsWith("volume_");
    const int cardWidth = qMin<int>(
            ui::px(1064), parentWidget()->width() - 2 * ui::metrics::notice::sideClearance);
    fontDialog = new ui::ModalOverlay(parentWidget());
    fontDialog->setObjectName("rrButtonControlsPopup");
    fontDialog->setDimmed(true);
    fontDialog->setGeometry(parentWidget()->rect());
    fontDialog->dismissed = [this] { dismissFontChoices(); };
    auto *card = fontDialog->card();
    auto *rows = card->rows();
    rows->setContentsMargins(ui::metrics::control::border, ui::metrics::notice::cardMargins.top(),
                             ui::metrics::control::border, ui::metrics::control::border);
    rows->setSpacing(0);
    card->heading(strings::get(volume ? "buttons.volume_title" : "buttons.title"),
                  strings::get(volume ? "buttons.volume_description" : "buttons.description"));
    auto draft =
            std::make_shared<QJsonObject>(current.value(pageButtons::preferenceKey).toObject());
    QVector<ui::CaptionButton *> controls;
    QVector<ui::InterfaceLabel *> previewValues;

    auto *overview = new QWidget(card);
    auto *overviewRows = new QVBoxLayout(overview);
    overviewRows->setContentsMargins(0, 0, 0, 0);
    overviewRows->setSpacing(0);
    auto *preview = new QWidget(overview);
    auto *previewRows = new QHBoxLayout(preview);
    previewRows->setContentsMargins(chrome::inset, ui::px(12), chrome::inset, ui::px(20));
    previewRows->setSpacing(0);
    auto *drawing = new QWidget(preview);
    const int drawingWidth = qMin<int>(ui::px(760), cardWidth - 2 * chrome::inset);
    drawing->setFixedWidth(drawingWidth);
    auto *drawingRows = new QHBoxLayout(drawing);
    drawingRows->setContentsMargins(0, 0, 0, 0);
    drawingRows->setSpacing(chrome::inset);
    auto *labels = new QWidget(drawing);
    auto *labelRows = new QVBoxLayout(labels);
    labelRows->setContentsMargins(0, 0, 0, 0);
    labelRows->setSpacing(ui::px(20));
    labelRows->addStretch();
    for (const auto &button : buttons) {
        auto *callout = new QWidget(labels);
        callout->setProperty("reader_button_callout", true);
        ui::readerStyle(
                callout,
                "QWidget[reader_button_callout=\"true\"]{border-left:1px solid palette(light);}");
        auto *calloutRows = new QVBoxLayout(callout);
        calloutRows->setContentsMargins(ui::px(24), 0, 0, 0);
        calloutRows->setSpacing(ui::px(6));
        auto *name =
                new ui::InterfaceLabel(strings::get(button.labelKey.toUtf8().constData()), callout);
        name->setWordWrap(true);
        ui::readerStyle(name, ui::styles::font(chrome::descriptionText));
        auto *value = new ui::InterfaceLabel(callout);
        value->setObjectName("rrButtonPreview_" + button.id);
        value->setWordWrap(true);
        ui::readerStyle(value, ui::styles::font(chrome::labelText));
        QJsonObject prefs;
        prefs[pageButtons::preferenceKey] = *draft;
        value->setText(
                actionLabel(pageButtons::direction(prefs, button.id, button.defaultDirection)));
        calloutRows->addWidget(name);
        calloutRows->addWidget(value);
        labelRows->addWidget(callout);
        previewValues.append(value);
    }
    labelRows->addStretch();
    overviewRows->addWidget(preview);
    if (volume) {
        auto *note = card->message(strings::get("buttons.volume_note"));
        note->setContentsMargins(chrome::inset, 0, chrome::inset, chrome::inset);
        ui::readerStyle(note, ui::styles::font(chrome::descriptionText));
        rows->removeWidget(note);
        overviewRows->addWidget(note);
    }
    rows->addWidget(overview);
    auto *mapping = new QWidget(card);
    auto *mappingRows = new QVBoxLayout(mapping);
    mappingRows->setContentsMargins(0, 0, 0, 0);
    mappingRows->setSpacing(0);
    for (int i = 0; i < buttons.size(); ++i) {
        const auto &button = buttons[i];
        auto *row = ui::makeCaptionButton(
                mapping, nullptr, ("rrButtonAction_" + button.id).toUtf8().constData(),
                strings::get(button.labelKey.toUtf8().constData()), [] { });
        ui::readerStyle(
                row,
                QString("QPushButton{font-size:%1px;min-height:%2px;max-height:%2px;border:0;border-radius:0;text-align:left;padding:0 %3px;background:palette(window);color:palette(window-text);} QPushButton:pressed{background:palette(midlight);}")
                        .arg(int(chrome::labelText))
                        .arg(int(chrome::listActionHeight))
                        .arg(int(chrome::inset)));
        ui::Edges rule(Qt::BottomEdge);
        if (i == 0)
            rule |= Qt::TopEdge;
        row->setRule(rule);
        row->setFixedHeight(chrome::listActionHeight);
        row->setCaption(previewValues[i]->text().toUpper());
        mappingRows->addWidget(row);
        controls.append(row);
    }
    rows->addWidget(mapping);
    auto choose = [this, buttons, draft, controls, previewValues](int index) {
        const int pickerWidth =
                qMin<int>(ui::metrics::notice::cardWidth,
                          parentWidget()->width() - 2 * ui::metrics::notice::sideClearance);
        auto *overlay = new ui::ModalOverlay(fontDialog);
        overlay->setObjectName("rrButtonActionPopup");
        overlay->setDimmed(true);
        overlay->setGeometry(fontDialog->rect());
        overlay->dismissed = [this, overlay] {
            overlay->hide();
            overlay->deleteLater();
            if (fontDialog)
                fontDialog->card()->show();
        };
        auto *picker = overlay->card();
        picker->rows()->setContentsMargins(
                ui::metrics::control::border, ui::metrics::notice::cardMargins.top(),
                ui::metrics::control::border, ui::metrics::control::border);
        picker->rows()->setSpacing(0);
        picker->heading(strings::get(buttons[index].labelKey.toUtf8().constData()),
                        strings::get("buttons.while_reading"));
        auto *choices = new ui::RadioList(picker);
        choices->setObjectName("rrButtonActionChoices");
        choices->setMinimumRowHeight(ui::metrics::list::oneLine);
        choices->setUniformRows(false);
        choices->setTopDivider(true);
        const QVector<int> directions{ -1, 1, 0 };
        QJsonObject prefs;
        prefs[pageButtons::preferenceKey] = *draft;
        for (int direction : directions) {
            choices->add("rrButtonChoose_" + pageButtons::action(direction), actionLabel(direction),
                         direction == 0 ? strings::get("buttons.none_description") : QString{});
            if (pageButtons::direction(prefs, buttons[index].id, buttons[index].defaultDirection)
                == direction)
                choices->setCurrent(choices->options().size() - 1);
        }
        choices->chosen = [overlay, draft, buttons, controls, previewValues, directions,
                           index](int selected) {
            const int direction = directions[selected];
            (*draft)[buttons[index].id] = pageButtons::action(direction);
            previewValues[index]->setText(actionLabel(direction));
            controls[index]->setCaption(actionLabel(direction).toUpper());
            overlay->dismissed();
        };
        picker->rows()->addWidget(choices);
        choices->setFixedHeight(
                choices->heightForWidth(pickerWidth - 2 * ui::metrics::control::border));
        picker->fitToWidth(pickerWidth);
        overlay->centerCard();
        // The chooser owns paging while open, including when it has no pager.
        fontDialog->card()->hide();
        overlay->show();
        overlay->raise();
        overlay->setFocus();
    };
    for (int i = 0; i < controls.size(); ++i)
        connect(controls[i], &QPushButton::clicked, card, [choose, i] { choose(i); });
    const int diagramWidth =
            qMin<int>(ui::px(260), qMin(drawingWidth * 2 / 5, parentWidget()->height() / 5));
    auto *diagram = new ButtonDiagram(drawing, buttons, diagramWidth, choose);
    drawingRows->addWidget(diagram);
    drawingRows->addWidget(labels, 1);
    previewRows->addStretch();
    previewRows->addWidget(drawing);
    previewRows->addStretch();
    auto *note = card->message(strings::get("buttons.navigation_hint"));
    const int notePadding =
            parentWidget()->height() >= ui::px(900) ? int(chrome::inset) : ui::px(12);
    note->setContentsMargins(chrome::inset, notePadding, chrome::inset, notePadding);
    ui::readerStyle(note, ui::styles::font(chrome::descriptionText));
    rows->removeWidget(note);
    mappingRows->addWidget(note);
    auto *cancel = ui::makeModalButton(
            card, { strings::get("common.cancel"), "rrButtonControlsCancel" }, false);
    auto *done = ui::makeModalButton(card, { strings::get("common.done"), "rrButtonControlsDone" },
                                     true);
    card->adaptiveActions({ cancel, done });
    connect(cancel, &QPushButton::clicked, this, [this] { dismissFontChoices(); });
    connect(done, &QPushButton::clicked, this, [this, draft, chosen] {
        const auto value = *draft;
        dismissFontChoices();
        chosen(value);
    });
    card->fitToWidth(cardWidth);
    fontDialog->centerCard();
    fontDialog->show();
    fontDialog->raise();
    fontDialog->setFocus();
}
} // namespace kobalt::reader
