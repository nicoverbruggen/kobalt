#include "reader_p.h"
#include "platform/api/kobo_refresh.h"
#include "app/reader/settings/settings_panel.h"
#include "app/reader/settings/text_presets_panel.h"
#include "assets/font_catalog.h"
#include "reading/style_sample.h"
#include "reading/text_presets.h"
#include "support/qt_compat.h"
#include "support/strings.h"
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
QString presetDetail(const Preset &preset)
{
    return textpresets::sizeLabel(preset.halfPoints) + QStringLiteral(" · ")
            + strings::get("common.percent").arg(preset.lineSpacing);
}

QString presetSummary(const Preset &preset)
{
    return fontDisplayName(preset.fontFace) + QStringLiteral(" · ") + presetDetail(preset);
}

constexpr qreal previewScale = 0.8;

// A gallery tile's preview: the sample's first lines on the theme's paper.
// A failed sample gives a null image.
QImage presetPreviewImage(const StyleSample &sample, QSize size, bool dark)
{
    if (!sample.valid() || sample.crops.isEmpty())
        return {};
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(ui::themeColors(dark).paper);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // The crop spans the sample page's width; fit that width and keep the
    // first lines.
    const auto crop = sample.crops.first().scaledToWidth(size.width(), Qt::SmoothTransformation);
    painter.drawImage(QPoint(), crop, QRect(QPoint(), size));
    painter.end();
    return image;
}
} // namespace

ReaderSession *Reader::textPresetSettings()
{
    return appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : session;
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
    // Holds the prerender's hidden gallery probe. It inherits the reader's
    // style sheet, so the probe measures the same tiles as the gallery.
    textPresetProbeHost = new QWidget(this);
    textPresetProbeHost->hide();
    textPresetSection = new TextPresetSection(menu);
    menu->setTopSection(textPresetSection);
    textPresetSection->toggled = [this] {
        auto next = preferences;
        next[textpresets::rowKey] = !textPresetSection->isOpen();
        if (!writePreferences(next))
            return;
        preferences = next;
        syncTextPresets();
        refresh();
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
    const auto state = textPresetSettings()->state();
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
                                         const QString &selected, bool modified)
{
    QVector<TextPresetView> all;
    for (const auto &preset : presets)
        all.append(textPresetView(preset));
    gallery->setPresets(all, selected,
                        // Saving is offered only for text that no preset holds.
                        presets.size() < textpresets::maximumCount
                                && (selected.isEmpty() || modified),
                        presetSummary(textpresets::capture(textPresetSettings()->state())));
}

void Reader::syncTextPresets()
{
    if (!textPresetSection)
        return;
    QVector<Preset> presets;
    QString selected;
    bool modified = false;
    textPresetChoice(&presets, &selected, &modified);
    // A book whose text matches a preset uses that preset, even when it got
    // there without a tap, so a later change shows as a change to it.
    if (!selected.isEmpty() && !modified && book && selected != book->textPreset())
        book->setTextPreset(selected);
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
        setTextPresetGalleryPresets(textPresetGallery, presets, selected, modified);
}

void Reader::applyTextPreset(const QString &id)
{
    const auto presets = textpresets::load(preferences);
    const auto *preset = textpresets::find(presets, id);
    if (!preset || session->temporaryTypography())
        return;
    book->setTextPreset(id);
    if (textpresets::matches(*preset, textPresetSettings()->state())) {
        syncTextPresets();
        refresh();
        return;
    }
    const auto values = textpresets::values(*preset);
    mutateAppearance([values](ReaderSession &preview) {
        preview.editAppearance(appearance::Patch(values));
    });
}

bool Reader::saveTextPresets(QJsonObject next, const QVector<Preset> &presets)
{
    textpresets::store(next, presets);
    if (!writePreferences(next)) {
        appearanceEdit.setError(strings::get("text_presets.could_not_save"));
        refresh();
        return false;
    }
    preferences = next;
    syncTextPresets();
    if (general && general->isVisible()) {
        showTextPresetSettings();
        fitSettings();
    }
    refresh();
    return true;
}

void Reader::updateTextPreset(const QString &id)
{
    auto presets = textpresets::load(preferences);
    for (auto &preset : presets) {
        if (preset.id != id)
            continue;
        auto captured = textpresets::capture(textPresetSettings()->state());
        captured.id = preset.id;
        captured.name = preset.name;
        preset = captured;
        book->setTextPreset(id);
        saveTextPresets(preferences, presets);
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
        notice->setProperty("reader_standard_modal", true);
        notice->setGeometry(rect());
        const auto close = [notice] {
            notice->hide();
            notice->deleteLater();
        };
        connect(notice->primaryButton(), &QPushButton::clicked, notice, close);
        notice->dismissed = close;
        notice->fitCard();
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
    auto preset = existing ? *existing : textpresets::capture(textPresetSettings()->state());
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
    auto *editor = new QLineEdit(card);
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
            presets.append(saved);
        } else {
            for (auto &entry : presets)
                if (entry.id == id)
                    entry.name = name;
        }
        // Native submission runs inside the controller; close after it returns.
        textPresetEditor->hide();
        qt::later(0, this, close);
        if (saveTextPresets(preferences, presets) && id.isEmpty()) {
            book->setTextPreset(saved.id);
            syncTextPresets();
        }
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
    dialog->fitCard();
    dialog->fitCard();
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

StyleSampleRequest Reader::textPresetPreviewRequest(const Preset &preset, QSize size)
{
    // The book's own settings with the preset's text, at the size of the
    // reading page. The preview shows the opening lines at 80%, so a tile
    // can show a few lines even of a large preset.
    auto settings = styleSampleSettings(session->state());
    const auto values = textpresets::values(preset);
    for (auto it = values.begin(); it != values.end(); ++it)
        settings[it.key()] = it.value();
    const int margin = ui::px(24);
    StyleSampleRequest request;
    request.settings = settings;
    request.settings["margin_pixels"] = margin;
    request.settings["top_margin_pixels"] = margin;
    request.key = "font_pixels";
    request.values = { values.value("font_pixels") };
    const QSize page(qRound(size.width() / previewScale),
                     qRound(size.height() / previewScale) + 4 * margin);
    request.page = page;
    request.thumbnail = QSize(1, 1);
    request.cropOrigin = { 0, 0 };
    request.boldHeading = true;
    request.firstPageOnly = true;
    request.crop = QSize(page.width(), qRound(size.height() / previewScale));
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
    dialog->setProperty("reader_standard_modal", true);
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
        if (book->textPreset() == id)
            book->setTextPreset({});
        saveTextPresets(next, list);
    });
    dialog->dismissed = close;
    dialog->fitCard();
    dialog->show();
    dialog->raise();
}

void Reader::openTextPresetGallery()
{
    if (textPresetGallery)
        return;
    // The gallery renders what the prerender has not finished.
    if (textPresetPrerenderCancel)
        textPresetPrerenderCancel->store(true);
    textPresetPrerenderQueue.clear();
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
    const int bottom = navigation ? navigation->contentBottom(size()) : height();
    return { 0, top, width(), qMax(1, bottom - top) };
}

storage::PreviewStore &Reader::textPresetTiles()
{
    if (!textPresetTileStore)
        textPresetTileStore = std::make_unique<storage::PreviewStore>(book->paths().previews());
    return *textPresetTileStore;
}

QVector<Reader::TextPresetPreview> Reader::textPresetPreviews(const QVector<Preset> &presets,
                                                              const QStringList &ids, QSize size)
{
    const auto fonts = session->fontInputs();
    // The tile's size and the paper colour also decide its pixels.
    const QByteArray tile = QByteArray::number(size.width()) + 'x'
            + QByteArray::number(size.height()) + (darkTheme ? 'd' : 'l');
    QVector<TextPresetPreview> previews;
    for (const auto &id : ids)
        if (const auto *preset = textpresets::find(presets, id)) {
            TextPresetPreview preview{
                preset->id, preset->fontFace, textPresetPreviewRequest(*preset, size), {}
            };
            preview.tile = styleSampleKey(fonts, preview.request) + tile;
            previews.append(preview);
        }
    return previews;
}

void Reader::renderTextPresetTile(TextPresetPreview preview, QSize size,
                                  std::shared_ptr<std::atomic_bool> cancelled,
                                  std::function<void(const QImage &)> finished)
{
    registerPickerFonts(preview.face);
    preview.request.cancelled = cancelled;
    const bool dark = darkTheme;
    renderStyleSample(session->fontInputs(), preview.request, this,
                      [this, cancelled, size, dark, key = preview.tile,
                       finished = std::move(finished)](StyleSample sample) {
                          const auto image = presetPreviewImage(sample, size, dark);
                          // Keep a finished tile even when its page was
                          // replaced: turning back shows it at once.
                          textPresetTiles().insert(key, image);
                          finished(cancelled->load() ? QImage() : image);
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
            textPresetPreviews(textPresetGalleryEntries, textPresetGallery->visibleIds(), size);
    QVector<QByteArray> keys{ QByteArray::number(size.width()) + 'x'
                              + QByteArray::number(size.height()) };
    for (const auto &preview : wanted)
        keys.append(preview.tile);
    if (keys == textPresetPreviewKeys)
        return;
    // A newer page replaces an unfinished one. Its renders stop, so the
    // engine is free for the page the reader shows.
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
    const auto finished = [gallery, round, cancelled](const QString &id, const QImage &image) {
        // A failed sample still finishes, so a page of previews is not held
        // back by one of them.
        if (!image.isNull())
            round->images.insert(id, image);
        if (--round->pending > 0 || cancelled->load() || !gallery)
            return;
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
            missingKeys, textPresetGallery, [this, gallery, missing, finished, cancelled, size] {
                int started = 0;
                for (const auto &preview : missing) {
                    QImage kept;
                    if (cancelled->load()) {
                        finished(preview.id, {});
                    } else if (textPresetTiles().find(preview.tile, &kept)) {
                        finished(preview.id, kept);
                    } else {
                        ++started;
                        renderTextPresetTile(preview, size, cancelled,
                                             [finished, id = preview.id](const QImage &image) {
                                                 finished(id, image);
                                             });
                    }
                }
                // Tests read how many tiles this gallery sent to the engine.
                gallery->setProperty("preview_renders",
                                     gallery->property("preview_renders").toInt() + started);
            });
}

void Reader::startTextPresetPrerender()
{
    if (textPresetPrerenderStarted)
        return;
    textPresetPrerenderStarted = true;
    // After the first page and the work that follows the opening: the
    // engine reports idle once it has drawn the pages beside this one.
    textPresetPrerenderWaiting = true;
    if (!session->busy() && !session->preparingNeighbours())
        qt::later(0, this, [this] { resumeTextPresetPrerender(); });
}

void Reader::resumeTextPresetPrerender()
{
    if (!std::exchange(textPresetPrerenderWaiting, false))
        return;
    if (textPresetPrerenderCancel)
        prerenderNextTextPreset();
    else
        prerenderTextPresets();
}

void Reader::prerenderTextPresets()
{
    // Landscape has no presets bar, so no All.
    if (closing || exitPending || lifecycleHidden || textPresetGallery || !textPresetProbeHost
        || width() > height())
        return;
    QVector<Preset> presets;
    QString selected;
    bool modified = false;
    textPresetChoice(&presets, &selected, &modified);
    // Lay the gallery out without showing it, for its first page and tile size.
    QSize size;
    QStringList ids;
    {
        // A child added to or removed from the reader repaints the page,
        // so the probe lives in a hidden holder made with the window.
        TextPresetGallery probe(textPresetProbeHost);
        probe.setGeometry(textPresetGalleryBounds());
        // The presets decide whether the pager shows, so lay out after them.
        setTextPresetGalleryPresets(&probe, presets, selected, modified);
        probe.layout()->activate();
        size = probe.previewSize();
        ids = probe.visibleIds();
    }
    if (size.isEmpty() || ids.isEmpty())
        return;
    if (textPresetPrerenderCancel)
        textPresetPrerenderCancel->store(true);
    textPresetPrerenderCancel = std::make_shared<std::atomic_bool>(false);
    textPresetPrerenderSize = size;
    textPresetPrerenderQueue = textPresetPreviews(presets, ids, size);
    QVector<QByteArray> keys;
    for (const auto &preview : textPresetPrerenderQueue)
        keys.append(preview.tile);
    textPresetTiles().load(keys, this, [this] { prerenderNextTextPreset(); });
}

void Reader::prerenderNextTextPreset()
{
    if (!textPresetPrerenderCancel || textPresetPrerenderCancel->load())
        return;
    QImage kept;
    while (!textPresetPrerenderQueue.isEmpty()
           && textPresetTiles().find(textPresetPrerenderQueue.first().tile, &kept))
        textPresetPrerenderQueue.removeFirst();
    if (textPresetPrerenderQueue.isEmpty()) {
        // Tests read when the first page is ready.
        setProperty("text_presets_prerendered", true);
        return;
    }
    // One tile at a time, only while nothing else needs the engine, so a page
    // turn waits for one tile at most.
    if (closing || exitPending || lifecycleHidden || services.suspended())
        return;
    if (session->busy() || appearanceEdit.applying()
        || appearanceEdit.previewState() == AppearanceEditSession::PreviewState::Opening
        || stylingSamplesPending > 0) {
        // Continue when the engine is next idle, at the latest after the
        // next page turn.
        textPresetPrerenderWaiting = true;
        return;
    }
    const auto preview = textPresetPrerenderQueue.takeFirst();
    renderTextPresetTile(preview, textPresetPrerenderSize, textPresetPrerenderCancel,
                         [this](const QImage &image) {
                             if (!image.isNull())
                                 setProperty("text_preset_prerender_renders",
                                             property("text_preset_prerender_renders").toInt() + 1);
                             prerenderNextTextPreset();
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
        if (writePreferences(next)) {
            preferences = next;
            showTextPresetSettings();
        }
        closeTextPresetGallery();
    };
    textPresetGallery->pageChanged = [this] { renderTextPresetGallery(); };
    for (auto *widget : textPresetGallery->findChildren<QWidget *>())
        platform::textRefresh(widget);
    textPresetGallery->show();
    textPresetGallery->raise();
    renderTextPresetGallery();
}
} // namespace kobalt::reader
