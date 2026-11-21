#include "reader_p.h"
#include "platform/api/kobo_refresh.h"
#include "app/reader/settings/settings_panel.h"
#include "app/reader/settings/text_presets_panel.h"
#include "assets/font_catalog.h"
#include "reading/style_sample.h"
#include "reading/style_sample_cache.h"
#include "reading/text_presets.h"
#include "support/qt_compat.h"
#include "support/percentage_text.h"
#include "support/diagnostics.h"
#include "ui/controls/text.h"
#include "ui/style/styles.h"
#include "ui/dialogs/action_menu.h"
#include "ui/style/font_preview.h"
#include "ui/style/metrics.h"
#include "ui/style/scale.h"
#include "ui/style/styles.h"

#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace kobalt::reader {
namespace strings = kobalt::strings;
namespace metrics = kobalt::ui::metrics;
using textpresets::Preset;

namespace {
// Nickel clears a QLineEdit's selection immediately before forwarding a key.
// Keep that selection for the synchronous key delivery, but let a tap or a
// later event leave the cursor wherever the user put it.
class PresetNameEdit final : public QLineEdit
{
public:
    explicit PresetNameEdit(QWidget *parent) : QLineEdit(parent)
    {
        connect(this, &QLineEdit::textChanged, this, [this] { forgetSelection(); });
        connect(this, &QLineEdit::selectionChanged, this, [this] {
            if (hasSelectedText()) {
                start = selectionStart();
                length = selectedText().size();
                ++revision;
            } else {
                const auto pending = revision;
                qt::later(0, this, [this, pending] {
                    if (revision == pending)
                        forgetSelection();
                });
            }
        });
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (property("reader_native_keyboard_editor").toBool() && !hasSelectedText() && start >= 0)
            setSelection(start, length);
        forgetSelection();
        QLineEdit::keyPressEvent(event);
        if (hasSelectedText()) {
            start = selectionStart();
            length = selectedText().size();
            ++revision;
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        forgetSelection();
        QLineEdit::mousePressEvent(event);
    }

private:
    void forgetSelection()
    {
        start = -1;
        length = 0;
        ++revision;
    }

    int start = -1, length = 0;
    quint64 revision = 0;
};

QString presetDetail(const Preset &preset)
{
    return textpresets::sizeLabel(preset.halfPoints) + QStringLiteral(" · ")
            + strings::percentage(preset.lineSpacing);
}

QString presetSummary(const Preset &preset)
{
    return fontDisplayName(preset.fontFace) + QStringLiteral(" · ") + presetDetail(preset);
}

// Preset tiles are drawn once on this canvas, whatever the device, and
// scaled to the gallery's tiles. Its pixels are at 300 PPI, so a preset's
// text size in points decides its size on the canvas.
constexpr int sampleCanvasPpi = 300;
constexpr int sampleCanvasWidth = 680, sampleCanvasHeight = 480;
constexpr int sampleMargin = 32;

// A gallery tile: the top of a preset's sample, scaled to the tile's width
// and drawn in the theme's ink on its paper.
QImage presetTileImage(const QImage &sample, QSize size, const ui::ThemeColors &colors)
{
    if (sample.isNull() || size.isEmpty())
        return {};
    const auto source = sample.convertToFormat(QImage::Format_RGB32);
    if (source.isNull())
        return {};
    QImage grey(source.size(), QImage::Format_Indexed8);
    if (grey.isNull())
        return {};
    for (int y = 0; y < source.height(); ++y) {
        const auto *from = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        auto *to = grey.scanLine(y);
        for (int x = 0; x < source.width(); ++x)
            to[x] = uchar(qGray(from[x]));
    }
    // The sample's black becomes the ink and its white the paper.
    const auto ink = colors.ink, paper = colors.paper;
    QVector<QRgb> table(256);
    for (int level = 0; level < 256; ++level) {
        const auto mix = [level](int dark, int light) {
            return (dark * (255 - level) + light * level + 127) / 255;
        };
        table[level] = qRgb(mix(ink.red(), paper.red()), mix(ink.green(), paper.green()),
                            mix(ink.blue(), paper.blue()));
    }
    grey.setColorTable(table);
    const auto scaled = grey.scaledToWidth(size.width(), Qt::SmoothTransformation);
    if (scaled.isNull())
        return {};
    // Stop at a blank row so a short tile never cuts across the last line.
    const auto hasInk = [&scaled, paper](int y) {
        for (int x = 0; x < scaled.width(); ++x)
            if (scaled.pixel(x, y) != paper.rgb())
                return true;
        return false;
    };
    int visible = qMin(scaled.height(), size.height() - ui::px(12));
    while (visible > 0 && hasInk(visible - 1))
        --visible;
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    image.fill(paper);
    QPainter painter(&image);
    if (visible > 0)
        painter.drawImage(QPoint(), scaled, QRect(0, 0, size.width(), visible));
    painter.end();
    return image;
}
} // namespace

ReaderSession *Reader::textPresetSettings()
{
    return appearanceEdit.edited() && appearanceEdit.preview()
                    && appearanceEdit.previewState() != AppearanceEditSession::PreviewState::Failed
            ? appearanceEdit.preview()
            : session;
}

TextPresetView Reader::textPresetView(const Preset &preset)
{
    TextPresetView view;
    view.id = preset.id;
    view.name = preset.name;
    view.detail = presetDetail(preset);
    const auto face =
            preset.fontFace.isEmpty() ? QString::fromLatin1(defaultFontFace) : preset.fontFace;
    for (const auto &font : textPresetSettings()->fontCatalog())
        if (font.family.compare(face, Qt::CaseInsensitive) == 0) {
            view.font = ui::fontPreviewSource(font.family, font.files);
            view.thickness = qBound(0, preset.weightSteps, 12);
            if (font.weightAxis.available())
                view.axisWeight =
                        qBound(font.weightAxis.minimum, preset.fontWeight, font.weightAxis.maximum);
            break;
        }
    return view;
}

void Reader::buildTextPresets()
{
    textPresetSection = new TextPresetSection(menu);
    menu->setTopSection(textPresetSection);
    textPresetSection->toggled = [this] {
        auto next = preferences;
        next[textpresets::rowKey] = !textPresetSection->isOpen();
        writePreferences(next, [this](const QJsonObject &applied, bool saved) {
            if (!saved)
                return;
            preferences = applied;
            syncTextPresets();
            // While the preview opens, refresh skips the appearance controls and their layout.
            if (menu->isVisible()
                && appearanceEdit.previewState() == AppearanceEditSession::PreviewState::Opening)
                fitAppearance();
            refresh();
        });
    };
    textPresetSection->chosen = [this](const QString &id) { applyTextPreset(id); };
    textPresetSection->held = [this](const QString &id) { showTextPresetActions(id); };
    textPresetSection->saveAsRequested = [this] { saveNewTextPreset(); };
    textPresetSection->updateRequested = [this] { updateTextPreset(book->textPreset()); };
    textPresetSection->allRequested = [this] { openTextPresetGallery(); };
}

void Reader::textPresetChoice(QVector<Preset> *presets, QString *selected, bool *modified)
{
    *presets = textpresets::load(preferences);
    const auto state = textPresetSettings()->requestedAppearance().fields();
    const QString applied = book ? book->textPreset() : QString();
    selected->clear();
    for (const auto &preset : *presets)
        if (textpresets::matches(preset, state) && (selected->isEmpty() || preset.id == applied))
            *selected = preset.id;
    *modified = false;
    if (selected->isEmpty() && textpresets::find(*presets, applied)) {
        *selected = applied;
        *modified = true;
    }
}

void Reader::setTextPresetGalleryPresets(TextPresetGallery *gallery, const QVector<Preset> &presets,
                                         const QString &selected)
{
    QVector<TextPresetView> all;
    for (const auto &preset : presets)
        all.append(textPresetView(preset));
    gallery->setPresets(all, selected,
                        // The first cell always offers Save current text.
                        true,
                        presetSummary(textpresets::capture(
                                textPresetSettings()->requestedAppearance().fields())));
}

void Reader::syncTextPresets()
{
    QVector<Preset> presets;
    QString selected;
    bool modified = false;
    textPresetChoice(&presets, &selected, &modified);
    // A book whose text matches a preset uses that preset, even when it got
    // there without a tap, so a later change shows as a change to it.
    if (!selected.isEmpty() && !modified && book && selected != book->textPreset())
        book->setTextPreset(selected);
    // The bar is built with the Text controls.
    if (!textPresetSection)
        return;
    QVector<TextPresetView> cards;
    int selectedIndex = -1;
    for (int index = 0; index < presets.size(); ++index) {
        if (presets[index].id == selected)
            selectedIndex = index;
        if (index < TextPresetSection::cardCount)
            cards.append(textPresetView(presets[index]));
    }
    // The cards keep their order. A preset further down takes the last card.
    if (selectedIndex >= TextPresetSection::cardCount && !cards.isEmpty())
        cards.last() = textPresetView(presets[selectedIndex]);
    const auto *chosen = textpresets::find(presets, selected);
    textPresetSection->setState(cards, selected, chosen ? chosen->name : QString(), modified,
                                preferences.value(textpresets::rowKey).toBool(false));
    if (!textPresetChoosingNewBook)
        textPresetGalleryEntries = presets;
    if (textPresetGallery && !textPresetChoosingNewBook)
        setTextPresetGalleryPresets(textPresetGallery, presets, selected);
}

void Reader::applyTextPreset(const QString &id)
{
    const auto presets = textpresets::load(preferences);
    const auto *preset = textpresets::find(presets, id);
    if (!preset || session->temporaryTypography())
        return;
    book->setTextPreset(id);
    // A pending preview can still show the preset we are switching back to.
    if (!appearanceEdit.edited()
        && textpresets::matches(*preset, textPresetSettings()->requestedAppearance().fields())) {
        syncTextPresets();
        refresh();
        return;
    }
    const auto values = textpresets::values(*preset);
    mutateAppearance([values](ReaderSession &preview) {
        preview.editAppearance(appearance::Patch(values));
    });
}

void Reader::saveTextPresets(QJsonObject next, const QVector<Preset> &presets,
                             std::function<void()> saved)
{
    textpresets::store(next, presets);
    writePreferences(next, [this, presets, saved = std::move(saved)](const QJsonObject &applied,
                                                                     bool ok) {
        if (!ok) {
            appearanceEdit.setError(strings::get("text_presets.could_not_save"));
            refresh();
            return;
        }
        preferences = applied;
        if (saved)
            saved();
        drawTextPresetTiles(presets);
        syncTextPresets();
        if (general && general->isVisible()) {
            showTextPresetSettings();
            fitSettings();
        }
        refresh();
    });
}

void Reader::updateTextPreset(const QString &id)
{
    auto presets = textpresets::load(preferences);
    for (auto &preset : presets) {
        if (preset.id != id)
            continue;
        auto captured = textpresets::capture(textPresetSettings()->requestedAppearance().fields());
        captured.id = preset.id;
        captured.name = preset.name;
        preset = captured;
        saveTextPresets(preferences, presets, [this, id] { book->setTextPreset(id); });
        return;
    }
}

void Reader::saveNewTextPreset()
{
    const auto presets = textpresets::load(preferences);
    if (presets.size() >= textpresets::maximumCount) {
        auto *notice = new ui::NoticeModal(
                this,
                { strings::get("text_presets.limit").arg(textpresets::maximumCount),
                  strings::get("text_presets.save_title"),
                  {} },
                { strings::get("common.ok"), "rrTextPresetLimitOk" });
        notice->setObjectName("rrTextPresetLimit");
        notice->setDimmed(true);
        notice->setGeometry(rect());
        const auto close = [notice] {
            notice->hide();
            notice->deleteLater();
        };
        connect(notice->primaryButton(), &QPushButton::clicked, notice, close);
        notice->dismissed = close;
        notice->show();
        notice->raise();
        return;
    }
    editTextPresetName({});
}

// The save and rename dialogs: a name field, with the widget keyboard where
// the host has no input method. Like the
// Night greeting editor, the card stays above the keyboard.
void Reader::editTextPresetName(const QString &id)
{
    if (textPresetEditor)
        return;
    const auto presets = textpresets::load(preferences);
    const auto *existing = textpresets::find(presets, id);
    if (!id.isEmpty() && !existing)
        return;
    auto preset = existing
            ? *existing
            : textpresets::capture(textPresetSettings()->requestedAppearance().fields());
    auto *dialog = new ui::ChoiceModal(
            this,
            { QString(),
              strings::get(existing ? "text_presets.rename_title" : "text_presets.save_title"),
              {} },
            { strings::get("common.cancel"), "rrTextPresetNameCancel" },
            { strings::get("common.save"), "rrTextPresetNameSave" });
    textPresetEditor = dialog;
    dialog->setObjectName("rrTextPresetName");
    dialog->setDimmed(true);
    dialog->setGeometry(rect());
    auto *card = dialog->card();
    auto *editor = new PresetNameEdit(card);
    ui::prepareNativeEditor(editor);
    editor->setObjectName("rrTextPresetNameInput");
    dialog->setFocusProxy(editor);
    editor->setMaxLength(textpresets::maximumName);
    editor->setText(existing ? preset.name : textpresets::suggestedName(presets, preset));
    editor->selectAll();
    editor->setAccessibleName(strings::get("text_presets.name"));
    editor->setInputMethodHints(Qt::ImhNoPredictiveText);
    ui::readerStyle(
            editor,
            // The suggested name starts selected; show the selection in ink,
            // not the system's accent colour.
            QString("QLineEdit{border:%1px solid palette(mid);border-radius:0;padding:0;background:palette(window);color:palette(window-text);selection-background-color:palette(window-text);selection-color:palette(window);")
                            .arg(int(metrics::control::border))
                    + ui::styles::font(metrics::chrome::labelText) + "}");
    editor->setTextMargins(metrics::notice::cardMargins.left(), 0,
                           metrics::notice::cardMargins.right(), 0);
    editor->setMinimumHeight(metrics::chrome::actionHeight);
    auto *inputRow = new QWidget(card);
    auto *inputLayout = new QVBoxLayout(inputRow);
    inputLayout->setContentsMargins(metrics::notice::cardMargins.left(),
                                    metrics::notice::cardMargins.top(),
                                    metrics::notice::cardMargins.right(), metrics::notice::cardGap);
    inputLayout->addWidget(editor);
    // The dialog has no description, so it has no description label. A taken
    // name shows its note here, in the description's type, under the input.
    auto *note = new ui::InterfaceLabel(QString(), inputRow);
    note->setObjectName("rrTextPresetNameNote");
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    note->setAlignment(Qt::AlignCenter);
    ui::readerStyle(note, ui::styles::font(int(metrics::chrome::descriptionText)));
    note->hide();
    inputLayout->addSpacing(metrics::notice::headingGap);
    inputLayout->addWidget(note);
    card->rows()->insertWidget(1, inputRow);
    const auto close = [this] {
        if (!textPresetEditor)
            return;
        if (textPresetKeyboard) {
            textPresetKeyboard->hide();
            textPresetKeyboard->submitted = {};
        }
        textPresetEditor->hide();
        textPresetKeyboard.reset();
        textPresetEditor->deleteLater();
        textPresetEditor.clear();
    };
    const auto submit = [this, dialog, editor, note, close, id, preset] {
        auto name = editor->text().simplified();
        auto presets = textpresets::load(preferences);
        if (name.isEmpty())
            name = textpresets::suggestedName(presets, preset);
        if (textpresets::nameTaken(presets, name, id)) {
            note->setText(strings::get("text_presets.name_taken"));
            note->show();
            dialog->fitCard();
            return;
        }
        auto saved = preset;
        saved.name = name;
        if (id.isEmpty()) {
            saved.id = textpresets::newId(presets);
            saved.example = false;
            presets.prepend(saved);
        } else {
            for (auto &entry : presets)
                if (entry.id == id)
                    entry.name = name;
        }
        // Native submission runs inside the controller; close after it returns.
        textPresetEditor->hide();
        qt::later(0, this, close);
        saveTextPresets(preferences, presets, [this, id, created = saved.id] {
            if (id.isEmpty())
                book->setTextPreset(created);
        });
    };
    connect(dialog->secondaryButton(), &QPushButton::clicked, this, close);
    connect(dialog->primaryButton(), &QPushButton::clicked, this, submit);
    connect(editor, &QLineEdit::returnPressed, this, submit);
    dialog->dismissed = close;
    if (services.keyboard) {
        textPresetKeyboard = services.keyboard(dialog, editor);
        textPresetKeyboard->setSubmitText(strings::get("common.save"));
        textPresetKeyboard->submitted = submit;
    }
    dialog->show();
    dialog->raise();
    editor->setFocus();
    if (textPresetKeyboard)
        textPresetKeyboard->show();
    // Keep the card above the keyboard.
    const auto bounds =
            textPresetKeyboard ? window()->property("reader_keyboard_bounds").toRect() : QRect();
    if (!bounds.isEmpty()) {
        const int bottom = dialog->mapFrom(window(), bounds.topLeft()).y();
        // As for the Night greeting: a wider card on a short landscape.
        if (card->height() > bottom && dialog->width() > dialog->height())
            card->fitToWidth(dialog->width() - 2 * int(metrics::notice::sideClearance));
        // On short landscapes, as for the Night greeting, the input row
        // gives up its spacing so the name and actions stay above the keyboard.
        if (card->height() > bottom) {
            inputLayout->setContentsMargins(metrics::notice::cardMargins.left(), 0,
                                            metrics::notice::cardMargins.right(), 0);
            card->fitToWidth(card->width());
        }
        const QRect area(0, 0, dialog->width(), qMax(1, bottom));
        dialog->setCardArea(area);
        // Changed margins invalidate nested layout constraints on the next turn.
        qt::later(0, dialog, [dialog, area] {
            dialog->card()->fitToWidth(dialog->card()->width());
            dialog->setCardArea(area);
        });
    }
    for (auto *widget : dialog->findChildren<QWidget *>())
        platform::textRefresh(widget);
}

TextSampleRequest Reader::textPresetSample(const Preset &preset)
{
    // Only the preset decides the sample, not the book or the device, so a
    // tile is drawn once and kept.
    TextSampleRequest request;
    request.face =
            preset.fontFace.isEmpty() ? QString::fromLatin1(defaultFontFace) : preset.fontFace;
    request.ppi = sampleCanvasPpi;
    request.pixels = qRound(preset.halfPoints * sampleCanvasPpi / 144.0);
    request.lineSpacing = preset.lineSpacing;
    request.alignment = preset.alignment;
    // Automatic hyphenation follows the book, and the sample has no rules.
    request.hyphenate = preset.hyphenation == QLatin1String("on");
    request.extraWeight = qBound(0, preset.weightSteps, 12);
    request.opticalSize = preset.opticalSize;
    // A face without a weight axis is read at its regular weight.
    for (const auto &font : session->fontCatalog())
        if (font.family.compare(request.face, Qt::CaseInsensitive) == 0
            && font.weightAxis.available())
            request.weight =
                    qBound(font.weightAxis.minimum, preset.fontWeight, font.weightAxis.maximum);
    request.canvas = QSize(sampleCanvasWidth, sampleCanvasHeight);
    request.margin = sampleMargin;
    setTextSampleText(request);
    return request;
}

void Reader::showTextPresetActions(const QString &id)
{
    const auto presets = textpresets::load(preferences);
    const auto *preset = textpresets::find(presets, id);
    if (!preset || textPresetActions)
        return;
    QVector<ui::ActionMenuItem> items{
        ui::actionItem(strings::get("text_presets.update_from_current"), "rrTextPresetUpdateFrom",
                       ui::ControlIcon::Check, [this, id] { updateTextPreset(id); }),
        ui::actionItem(strings::get("text_presets.delete"), "rrTextPresetDelete",
                       ui::ControlIcon::Trash, [this, id] { confirmDeleteTextPreset(id); }),
    };
    ui::ActionMenuHeader header;
    header.title = preset->name;
    header.caption = presetSummary(*preset);
    if (textpresets::newBookChoice(preferences) == id)
        header.detail = strings::get("text_presets.used_for_new_books");
    // Rename is the header's edit cell.
    const auto rename =
            ui::actionItem(strings::get("text_presets.rename"), "rrTextPresetRename",
                           ui::ControlIcon::EditHighlight, [this, id] { editTextPresetName(id); });
    auto *actions = new ui::ActionMenu(this, header, items, rename);
    textPresetActions = actions;
    actions->setObjectName("rrTextPresetActions");
    actions->dismissed = [this] {
        if (!textPresetActions)
            return;
        textPresetActions->hide();
        textPresetActions->deleteLater();
        textPresetActions.clear();
    };
    actions->setDimmed(true);
    actions->setProperty("reader_standard_modal", true);
    actions->setGeometry(rect());
    actions->fitCard();
    actions->show();
    actions->raise();
    actions->setFocus();
}

void Reader::confirmDeleteTextPreset(const QString &id)
{
    const auto presets = textpresets::load(preferences);
    const auto *preset = textpresets::find(presets, id);
    if (!preset)
        return;
    auto *dialog = new ui::ChoiceModal(
            this,
            { strings::get("text_presets.delete_detail"),
              strings::get("text_presets.delete_prompt").arg(preset->name),
              {} },
            { strings::get("common.cancel"), "rrTextPresetDeleteCancel" },
            { strings::get("text_presets.delete"), "rrTextPresetDeleteConfirm" });
    dialog->setObjectName("rrTextPresetDeleteDialog");
    dialog->setDimmed(true);
    dialog->setGeometry(rect());
    const auto close = [dialog] {
        dialog->hide();
        dialog->deleteLater();
    };
    connect(dialog->secondaryButton(), &QPushButton::clicked, dialog, close);
    connect(dialog->primaryButton(), &QPushButton::clicked, this, [this, id, close] {
        close();
        auto list = textpresets::load(preferences);
        for (int at = 0; at < list.size(); ++at)
            if (list[at].id == id) {
                list.removeAt(at);
                break;
            }
        auto next = preferences;
        if (next.value(textpresets::newBookKey).toString() == id)
            next.remove(textpresets::newBookKey);
        saveTextPresets(next, list, [this, id] {
            if (book->textPreset() == id)
                book->setTextPreset({});
        });
    });
    dialog->dismissed = close;
    dialog->show();
    dialog->raise();
}

void Reader::openTextPresetGallery()
{
    if (textPresetGallery)
        return;
    textPresetGallery = new TextPresetGallery(this);
    textPresetGallery->setGeometry(textPresetGalleryBounds());
    textPresetGallery->closed = [this] { closeTextPresetGallery(); };
    textPresetGallery->chosen = [this](const QString &id) {
        // The release that ends a long press can arrive as a tap. The open
        // actions own it.
        if (textPresetActions)
            return;
        applyTextPreset(id);
        closeTextPresetGallery();
    };
    textPresetGallery->held = [this](const QString &id) { showTextPresetActions(id); };
    textPresetGallery->saveRequested = [this] { saveNewTextPreset(); };
    textPresetGallery->pageChanged = [this] { renderTextPresetGallery(); };
    syncTextPresets();
    textPresetGallery->show();
    textPresetGallery->raise();
    renderTextPresetGallery();
    refresh();
}

QRect Reader::textPresetGalleryBounds() const
{
    const int top = navigation ? navigation->statusHeight() : 0;
    return { 0, top, width(), qMax(1, height() - top) };
}

storage::PreviewStore &Reader::textPresetTiles()
{
    if (!textPresetTileStore)
        textPresetTileStore = std::make_unique<storage::PreviewStore>(book->paths().previews());
    return *textPresetTileStore;
}

QVector<Reader::TextPresetPreview> Reader::textPresetPreviews(const QVector<Preset> &presets,
                                                              const QStringList &ids)
{
    const auto fonts = session->fontInputs();
    const auto catalog = session->fontCatalog();
    QVector<TextPresetPreview> previews;
    for (const auto &id : ids)
        if (const auto *preset = textpresets::find(presets, id)) {
            TextPresetPreview preview{ preset->id, textPresetSample(*preset), {} };
            preview.tile = textSampleKey(fonts, preview.request, catalog);
            previews.append(preview);
        }
    return previews;
}

void Reader::drawTextPresetTile(const TextPresetPreview &preview,
                                std::function<void(const QImage &)> finished)
{
    // Tests read how many tiles this reader drew.
    setProperty("text_preset_tile_draws", property("text_preset_tile_draws").toInt() + 1);
    renderTextSample(session->fontInputs(), preview.request, this,
                     [this, key = preview.tile, finished = std::move(finished)](QImage sample) {
                         if (!sample.isNull())
                             textPresetTiles().insert(key, sample);
                         finished(sample);
                     });
}

void Reader::drawTextPresetTiles(const QVector<Preset> &presets)
{
    // A saved preset's tile is drawn now, so the gallery shows it at once.
    // The examples are drawn the first time a gallery needs them. An open
    // gallery draws the tiles it shows itself.
    if (textPresetGallery)
        return;
    QStringList ids;
    for (const auto &preset : presets)
        if (!preset.example)
            ids.append(preset.id);
    const auto previews = textPresetPreviews(presets, ids);
    QVector<QByteArray> keys;
    for (const auto &preview : previews)
        keys.append(preview.tile);
    if (keys.isEmpty())
        return;
    textPresetTiles().load(keys, this, [this, previews] {
        for (const auto &preview : previews) {
            QImage kept;
            if (!textPresetTiles().find(preview.tile, &kept))
                drawTextPresetTile(preview, [](const QImage &) { });
        }
    });
}

void Reader::renderTextPresetGallery()
{
    // Showing the gallery resizes it and refreshes the reader more than once.
    // Render only for the laid-out grid, and only once for each page.
    if (!textPresetGallery || !textPresetGallery->isVisible())
        return;
    const auto size = textPresetGallery->previewSize();
    if (size.isEmpty())
        return;
    const auto wanted =
            textPresetPreviews(textPresetGalleryEntries, textPresetGallery->visibleIds());
    // The tile's size and the theme decide how a sample is shown.
    QVector<QByteArray> keys{ QByteArray::number(size.width()) + 'x'
                              + QByteArray::number(size.height()) + (darkTheme ? 'd' : 'l') };
    for (const auto &preview : wanted)
        keys.append(preview.tile);
    if (keys == textPresetPreviewKeys)
        return;
    // A newer page replaces an unfinished one.
    if (textPresetPreviewRender)
        textPresetPreviewRender->store(true);
    textPresetPreviewKeys = keys;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    textPresetPreviewRender = cancelled;

    // A page's previews arrive one by one. Hand them over together, so the
    // e-ink screen gets one update for the page, not one per preview. When
    // memory holds every tile, that happens before the gallery's first paint.
    struct Round
    {
        QHash<QString, QImage> images;
        int pending = 0;
    };

    auto round = std::make_shared<Round>();
    round->pending = wanted.size();
    const QPointer<TextPresetGallery> gallery(textPresetGallery);
    const auto colors = ui::themeColors(this);
    const auto finished = [gallery, round, cancelled, size, colors](const QString &id,
                                                                    const QImage &sample) {
        // A failed sample still finishes, so a page of previews is not held
        // back by one of them.
        if (cancelled->load() || !gallery)
            return;
        if (!sample.isNull())
            round->images.insert(id, presetTileImage(sample, size, colors));
        if (--round->pending == 0)
            gallery->setPreviews(round->images);
    };
    QVector<TextPresetPreview> missing;
    QVector<QByteArray> missingKeys;
    for (const auto &preview : wanted) {
        QImage kept;
        if (textPresetTiles().find(preview.tile, &kept)) {
            finished(preview.id, kept);
            continue;
        }
        missing.append(preview);
        missingKeys.append(preview.tile);
    }
    if (missing.isEmpty())
        return;
    // Then storage, read on its worker, and the engine for the rest.
    textPresetTiles().load(
            missingKeys, textPresetGallery, [this, gallery, missing, finished, cancelled] {
                if (cancelled->load())
                    return;
                int started = 0;
                for (const auto &preview : missing) {
                    QImage kept;
                    if (textPresetTiles().find(preview.tile, &kept)) {
                        finished(preview.id, kept);
                    } else {
                        ++started;
                        drawTextPresetTile(preview,
                                           [finished, id = preview.id](const QImage &sample) {
                                               finished(id, sample);
                                           });
                    }
                }
                // Tests read how many tiles this gallery sent to the engine.
                gallery->setProperty("preview_renders",
                                     gallery->property("preview_renders").toInt() + started);
            });
}

void Reader::closeTextPresetGallery()
{
    if (!textPresetGallery)
        return;
    if (textPresetPreviewRender)
        textPresetPreviewRender->store(true);
    textPresetPreviewRender.reset();
    textPresetPreviewKeys.clear();
    textPresetGallery->hide();
    textPresetGallery->deleteLater();
    textPresetGallery.clear();
    textPresetChoosingNewBook = false;
    refresh();
}

// Settings > Defaults: what new books start with.
void Reader::showTextPresetSettings()
{
    auto *panel = static_cast<SettingsPanel *>(general);
    if (!panel->textForNewBooksButton)
        return;
    const auto presets = textpresets::load(preferences);
    const auto choice = textpresets::newBookChoice(preferences);
    const auto *chosen = textpresets::find(presets, choice);
    panel->textForNewBooksButton->setCaption(
            chosen ? chosen->name
                   : strings::get(choice == "book" ? "settings.new_book_book_default"
                                                   : "settings.new_book_last_used"));
}

// Text for new books uses the All presets grid: the book defaults, the last
// book's text and every preset, each as its own sample. A tap chooses.
void Reader::openNewBookText()
{
    if (textPresetGallery)
        return;
    const auto starting = [this](const char *choice, const char *id, const char *label) {
        auto settings = preferences;
        settings[textpresets::newBookKey] = QString::fromLatin1(choice);
        auto preset = textpresets::capture(appearance::bookDefaults(settings));
        preset.id = QString::fromLatin1(id);
        preset.name = strings::get(label);
        return preset;
    };
    textPresetGalleryEntries = { starting("book", "book", "settings.new_book_book_default"),
                                 starting("last", "last", "settings.new_book_last_used") };
    textPresetGalleryEntries += textpresets::load(preferences);
    QVector<TextPresetView> views;
    for (const auto &preset : textPresetGalleryEntries)
        views.append(textPresetView(preset));
    textPresetChoosingNewBook = true;
    textPresetGallery = new TextPresetGallery(this);
    textPresetGallery->setHeading(strings::get("settings.text_for_new_books_title"),
                                  strings::get("settings.new_book_text_heading"));
    textPresetGallery->setGeometry(general ? general->geometry() : rect());
    textPresetGallery->setPresets(views, textpresets::newBookChoice(preferences), false, {});
    textPresetGallery->closed = [this] { closeTextPresetGallery(); };
    textPresetGallery->chosen = [this](const QString &value) {
        auto next = preferences;
        next[textpresets::newBookKey] = value;
        // Older builds read only the inheritance switch.
        next[appearance::inheritTextLayoutKey] = value != "book";
        writePreferences(next, [this](const QJsonObject &applied, bool saved) {
            if (saved) {
                preferences = applied;
                showTextPresetSettings();
            }
            closeTextPresetGallery();
        });
    };
    textPresetGallery->pageChanged = [this] { renderTextPresetGallery(); };
    for (auto *widget : textPresetGallery->findChildren<QWidget *>())
        platform::textRefresh(widget);
    textPresetGallery->show();
    textPresetGallery->raise();
    renderTextPresetGallery();
}
} // namespace kobalt::reader
