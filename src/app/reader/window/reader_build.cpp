#include "render/page_content.h"
#include "ui/controls/text.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/sliders.h"
#include "ui/controls/panels.h"
#include "ui/dialogs/modals.h"
#include "ui/style/theme.h"
#include "assets/bundled_fonts.h"
#include "reader_p.h"
#include "dictionaries/manager.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "app/reader/window/font_registration.h"
#include "platform/api/kobo_refresh.h"
#include "support/diagnostics.h"
#include "ui/style/metrics.h"
#include "ui/panel_timing.h"
#include "storage/reader_storage.h"
#include "app/reader/settings/settings_panel.h"
#include "render/page_refresh_regions.h"
#include "reading/appearance_settings.h"
#include "reading/page_navigation.h"
#include <QApplication>
#include <QGridLayout>
#include <QPainter>
#include <QMap>
#include <QHBoxLayout>
#include <QSlider>
#include <QScreen>
#include <QVBoxLayout>
#include <utility>

namespace kobalt::reader {
namespace strings = kobalt::strings;
using namespace kobalt::storage;
namespace styles = kobalt::ui::styles;
namespace metrics = kobalt::ui::metrics;
using kobalt::Choice;
using kobalt::ChoiceScreen;
using kobalt::platform::textRefresh;
using kobalt::ui::buttonIcon;
using kobalt::ui::ControlIcon;
using kobalt::ui::makeButton;
using kobalt::ui::readerStyle;
using kobalt::ui::SettingSlider;

namespace {
class BookmarkTray final : public ui::Panel
{
public:
    explicit BookmarkTray(QWidget *parent) : ui::Panel(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int padding = ui::px(12);
        QMap<int, QVector<QRect>> columns;
        for (auto *button : findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly))
            if (!button->isHidden())
                columns[button->x()].append(button->geometry());
        for (const auto &buttons : columns) {
            QRect content;
            for (const auto &button : buttons)
                content |= button;
            const qreal inset = int(metrics::control::border) / 2.0;
            const auto bounds =
                    QRectF(content.adjusted(-padding, -2 * padding, padding, 2 * padding))
                            .adjusted(inset, inset, -inset, -inset);
            painter.setPen(QPen(palette().color(QPalette::WindowText), metrics::control::border));
            painter.setBrush(ui::themeColors(this).paper);
            painter.drawRoundedRect(bounds, bounds.width() / 2, bounds.width() / 2);
            painter.setPen(QPen(ui::dividerColor(this), metrics::control::border));
            const qreal dividerInset = bounds.width() / 4;
            for (int row = 1; row < buttons.size(); ++row) {
                const qreal y = (buttons[row - 1].bottom() + 1 + buttons[row].top()) / 2.0;
                painter.drawLine(QPointF(bounds.left() + dividerInset, y),
                                 QPointF(bounds.right() - dividerInset, y));
            }
        }
    }
};

class ReturnTitle final : public ui::InterfaceLabel
{
public:
    using ui::InterfaceLabel::InterfaceLabel;

protected:
    void paintEvent(QPaintEvent *) override
    {
        const auto *button = qobject_cast<QPushButton *>(parentWidget());
        // A large interface scale can leave the button only the arrow's cell.
        if (button && button->width() <= int(metrics::chrome::cellWidth))
            return;
        QPainter painter(this);
        painter.setFont(font());
        const bool pressed = button && button->isEnabled() && button->isDown();
        const auto colors = button ? button->palette() : palette();
        painter.setPen(colors.color(!isEnabled()      ? QPalette::Mid
                                            : pressed ? QPalette::Window
                                                      : QPalette::WindowText));
        const auto bounds = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
        // Font line boxes include padding above and below the visible letters.
        ui::drawOpticalText(painter, bounds.translated(0, metrics::chrome::returnTitleOffset),
                            text(), true);
    }
};

class ReturnButton final : public ui::Button
{
public:
    ReturnButton(const QString &caption, QWidget *parent) : ui::Button(caption, parent)
    {
        QImage mask(glyph, glyph, QImage::Format_ARGB32_Premultiplied);
        mask.fill(Qt::transparent);
        QPainter painter(&mask);
        ui::controlIcon(ControlIcon::Back, Qt::black).paint(&painter, mask.rect());
        painter.end();
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x)
                if (qAlpha(mask.pixel(x, y)))
                    arrowInk |= QRect(x, y, 1, 1);
    }

    int labelStart(const QFont &captionFont) const
    {
        const int arrowLeft = (int(metrics::chrome::cellWidth) - glyph) / 2;
        return arrowLeft + arrowInk.right() + 1 + int(metrics::chrome::returnLabelGap)
                - ui::interfaceInkBounds(text(), captionFont).left();
    }

    QSize sizeHint() const override
    {
        ensurePolished();
        const auto content = layout() ? layout()->sizeHint() : QSize();
        return { content.width(), qMax(content.height(), int(metrics::chrome::actionHeight)) };
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool pressed = isEnabled() && isDown();
        const auto paper = palette().color(QPalette::Window);
        const auto ink = palette().color(QPalette::WindowText);
        painter.fillRect(rect(), pressed ? ink : paper);
        // Zen mode shows the arrow alone, centered in its cell.
        const bool arrowOnly = property("reader_arrow_only").toBool();
        const QRect iconCell =
                arrowOnly ? rect() : QRect(0, 0, metrics::chrome::cellWidth, height());
        if (property("reader_return_divider").toBool())
            ui::paintEdges(painter, rect(), Qt::RightEdge, palette().color(QPalette::Mid));
        if (!isEnabled())
            painter.setOpacity(.45);
        const auto *title = findChild<QLabel *>("rrReturnTitle");
        qreal letterOffset = 0;
        if (title && !arrowOnly && !title->text().isEmpty()) {
            const auto word = ui::interfaceInkBounds(title->text(), title->font());
            const auto first = ui::interfaceInkBounds(title->text().left(1), title->font());
            letterOffset = (first.top() + first.bottom() - word.top() - word.bottom()) / 2.0;
        }
        const int top = qRound((height() - 1) / 2.0 + letterOffset
                               - (arrowInk.top() + arrowInk.bottom()) / 2.0)
                + int(metrics::chrome::returnArrowOffset);
        ui::controlIcon(ControlIcon::Back, pressed ? paper : ink)
                .paint(&painter, QRect((iconCell.width() - glyph) / 2, top, glyph, glyph));
    }

private:
    const int glyph = metrics::chrome::backIcon;
    QRect arrowInk;
};

kobalt::ui::Button *iconButton(QWidget *parent, QBoxLayout *layout, const char *name,
                               const QString &label, ControlIcon icon, std::function<void()> action)
{
    auto *button = static_cast<kobalt::ui::Button *>(
            makeButton(parent, layout, name, {}, std::move(action)));
    button->setAccessibleName(label);
    button->setToolTip(label);
    kobalt::ui::iconAction(button, icon, label);
    button->setFixedWidth(metrics::control::iconButtonWidth);
    return button;
}
} // namespace

ChapterHeader::ChapterHeader(QWidget *parent) : QPushButton(parent)
{
    setObjectName("rrReadingMenu");
    readerStyle(this, "min-height:0;padding:0;");
    setUpdatesEnabled(false);
    // The page handles taps on the strip, including the bookmark corner.
    // On firmware 4 a touch on this button never became a click.
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

Reader::Reader(std::unique_ptr<kobalt::ReadingBook> openedBook,
               kobalt::ReaderOrientation *orientation, kobalt::ReaderHostServices hostServices)
    : ReaderWindow(nullptr, Qt::Window | Qt::FramelessWindowHint),
      services(std::move(hostServices)),
      book(std::move(openedBook)),
      session(book->session()),
      orientation(orientation)
{
    Q_ASSERT(services.complete());
    annotationDraft = std::make_unique<AnnotationDraft>(
            *session, [this] { return !lifecycleHidden && !services.suspended(); });
    if (services.annotationExporter)
        annotationExporter = services.annotationExporter(this);
    building.start();
    // Qt 5.2 leaves default-constructed timers looking valid.
    openingStarted.invalidate();
    showing.invalidate();
    turnStarted.invalidate();
    setObjectName("rrReader");
    setAttribute(Qt::WA_DeleteOnClose);
    // Member timers belong to the window, so a check of its children finds them.
    for (auto *timer : { &kosyncOpeningGrace, &renderNoticeTimer, &resizeTimer, &annotationModeHide,
                         &brightnessHide, &lightFeedback })
        timer->setParent(this);
    presentation = services.presentation(this);
    Q_ASSERT(presentation);
    dictionary = services.dictionary();
    if (services.downloadableDictionaries)
        dictionary = std::make_unique<kobalt::DictionaryManager>(
                book->paths().directory + "/dictionaries", std::move(dictionary));
    if (!book->dictionaryChoice().isEmpty()) {
        QString error;
        dictionary->select(book->identity(), book->dictionaryChoice(), &error);
    }
    kobalt::platform::setReaderRefreshWindow(this);
    qApp->installEventFilter(this);
    setWindowModality(Qt::ApplicationModal);
    // A damaged settings file still falls back to the defaults, as a
    // missing one does, but the diagnostics say which it was.
    const auto savedSettings = kobalt::storage::readPreferences(book->paths().preferences());
    if (savedSettings.status != kobalt::storage::ReadStatus::Ok
        && savedSettings.status != kobalt::storage::ReadStatus::Missing)
        kobalt::diagnostic("settings-unreadable",
                           { { "status", kobalt::storage::readStatusName(savedSettings.status) } },
                           false);
    preferences = appearance::readerPreferences(savedSettings.object, book->layoutPreferences());
    // Remember this book's layout as the last layout in the shared profile.
    // A damaged settings file is left for the next explicit settings save to
    // replace.
    const bool settingsReadable = savedSettings.status == kobalt::storage::ReadStatus::Ok
            || savedSettings.status == kobalt::storage::ReadStatus::Missing;
    if (settingsReadable && !session->temporaryTypography()) {
        auto global = appearance::globalPreferences(preferences);
        appearance::rememberTextLayout(global, session->state());
        appearance::rememberPageMargins(global, session->state());
        if (global != savedSettings.object) {
            // The shared settings must be on disk before the reader uses them.
            // The window is still being built, so no input can arrive.
            mainthread::AllowBlocking allow("remembered layout before reader input");
            if (!kobalt::storage::writePreferences(book->paths().preferences(), global))
                kobalt::diagnostic("last-appearance-unsaved", {},
                                   kobalt::DiagnosticSeverity::Error);
        }
    }
    bookmarks = std::make_unique<kobalt::BookmarkSession>(
            *session, [this] { return layoutKey(); }, book->paths().annotations(), book->identity(),
            book->fingerprint(), session->title(), book->paths().database());
    highlights = std::make_unique<kobalt::HighlightSession>(
            *session, [this] { return layoutKey(); }, book->paths().annotations(), book->identity(),
            book->fingerprint(), session->title(), book->paths().database());
    highlights->changed = [this] {
        syncMarkupRequest();
        annotationFrame.clear();
        if (bookmarkList && bookmarkList->isVisible() && bookmarkList->annotationsShown()) {
            const bool ready = highlights->resolved() || highlights->sync();
            bookmarkList->showAnnotations(ready ? annotationRows() : QVector<BookmarkRow>(),
                                          !ready);
        }
        if (!annotationSaving) {
            refresh();
        }
    };
    // A highlight save on the writer thread failed and the saved list is back.
    highlights->saveFailed = [this] {
        kobalt::qt::later(0, this, [this] {
            if (exitPending) {
                failExit(closeRequest, strings::get("annotations.change_not_saved"));
                return;
            }
            showHighlightError();
        });
    };
    bookmarks->changed = [this] {
        if (bookmarkList && bookmarkList->isVisible() && !bookmarkList->annotationsShown()) {
            bookmarkList->showBookmarks(bookmarkRows());
        }
        refresh();
    };
    bookmarks->added = [this] {
        page->bookmarked = true;
        if (bookmarks->resolved()) {
            pageScrubber->setBookmarkedPages(bookmarks->pages());
        }
        page->refreshContent();
        if (bookmarkList && bookmarkList->isVisible() && !bookmarkList->annotationsShown()) {
            bookmarkList->showBookmarks(bookmarkRows());
        }
        saveSoon();
    };
    bookmarks->failed = [this] {
        kobalt::qt::later(0, this, [this] {
            if (exitPending) {
                failExit(closeRequest, bookmarks->error());
                return;
            }
            showBookmarkError();
        });
    };
    // Choose the text weight before the first font is used.
    kobalt::ui::setInterfaceWeight(kobalt::ui::heavierTextSteps);
    settingsUiFontFace = preferences.value("ui_font_face").toString();
    settingsUiMonoFontFace = preferences.value("ui_mono_font_face").toString();
    settingsHeaderFooterFontFace = preferences.value("header_footer_font_face").toString();
    settingsDictionaryFontFace = preferences.value("dictionary_font_face").toString();
    settingsDictionaryFontSize = preferences.value("dictionary_font_size").toInt(100);
    for (auto *face : { &settingsUiFontFace, &settingsUiMonoFontFace, &settingsHeaderFooterFontFace,
                        &settingsDictionaryFontFace }) {
        if (!session->fontFaces().contains(*face))
            face->clear();
        if (!face->isEmpty())
            registerPickerFonts(*face);
    }
    ui::setInterfaceFontFamily(settingsUiFontFace);
    ui::setInterfaceMonoFontFamily(settingsUiMonoFontFace);
    ui::setHeaderFooterFontFamily(settingsHeaderFooterFontFace);
    ui::setHeaderFooterAddedWeight(preferences.value("header_footer_added_weight").toInt(2));
    // Set only our window; native Nickel controls keep their own font.
    setFont(kobalt::ui::interfaceFont());
    kobalt::ui::keepInterfaceUnhinted(this);
    readerStyle(this, styles::reader());
    QElapsedTimer part;
    part.start();
    const auto built = [this, &part](const char *name) {
        openingStages[QString("build_%1_ms").arg(QLatin1String(name))] = part.nsecsElapsed() / 1e6;
        part.restart();
    };
    buildReadingPage();
    built("reading_page");
    startClockTimer();
    buildToolbar();
    built("toolbar");
    buildBottomControls();
    built("bottom_controls");
    const auto appearanceLayout = buildMenu();
    built("menu");
    buildOverlays();
    built("overlays");
    buildAppearance(appearanceLayout);
    built("appearance");
    buildSettings();
    built("settings");
    buildNotices();
    built("notices");
    buildAnnotations();
    built("annotations");
    connectLightGestures();
    configureNavigation();
    textRefresh(this);
    for (auto *widget : findChildren<QWidget *>())
        textRefresh(widget);
    menu->hide();
    topControls->hide();
    bottomControls->hide();
    if (services.observeBluetooth)
        services.observeBluetooth(this, [this](bool enabled) {
            if (menuBluetooth->active() == enabled)
                return;
            menuBluetooth->setActive(enabled);
            if (topControls->isVisible() && !lifecycleHidden)
                syncNavigation();
        });
    // With the Wi-Fi power saver, the host's own background requests must not
    // turn Wi-Fi back on while this book is open, for example after a wake.
    if (services.holdWifiOff)
        services.holdWifiOff(this, [this] { return !closing && wifiPowerSaverApplies(); });
    if (services.observeWifi)
        services.observeWifi(this, [this](WifiStatus state) {
            if (menuWifi->setStatus(state.enabled, state.connected, state.strength)
                && topControls->isVisible() && !lifecycleHidden)
                syncNavigation();
        });
    connect(header, &QPushButton::clicked, this, [this] {
        if (topControls->isVisible()) {
            flashAfterMenu = true;
            dismissControls();
        } else
            showControls();
    });
    // Keep the displayed page untouched while a turn renders. Refreshing
    // at request time sends the old page to the panel a second time.
    this->session->updated = [this] { refresh(); };
    this->session->renderProgressChanged = [this] {
        if (session->renderProgress().active) {
            book->pauseLocalPace();
        }
        refreshRenderNotice();
    };
    this->session->turnBoundary = [this](int direction) { turnPage(direction, 0, false); };
    connect(QGuiApplication::primaryScreen(), &QScreen::geometryChanged, this,
            [this](const QRect &geometry) {
                if (isVisible() && !closing)
                    setGeometry(geometry);
            });
    resize(QGuiApplication::primaryScreen()->size());
    prepareSleepCover();
    loadTodayReading();
    if (sleep::mode(preferences) == sleep::Mode::Overlay)
        prepareSleepOverlay();
    const auto battery = services.battery();
    lowBattery = !battery.charging && !battery.fullyCharged && battery.percent >= 0
            && battery.percent <= 5;
    if (services.observeBattery) {
        services.observeBattery(this, [this] {
            const auto battery = services.battery();
            lowBattery = !battery.charging && !battery.fullyCharged && battery.percent >= 0
                    && battery.percent <= 5;
            updateSleepBattery();
        });
    }
    book->onCheckpointFailed([this] { localSaveDue = true; });
    connectPageInput();
    // Android can suspend the app without hiding the embedded reader widget.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this] {
        observeReading();
        if (services.suspended())
            book->checkpointReadingHistory();
    });
    openingStages["build_ms"] = building.nsecsElapsed() / 1e6;
}

void Reader::buildReadingPage()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    page = new kobalt::ReadingPage(this, services.cancelAnimation);
    page->rightClickAnnotations = services.rightClickAnnotations;
    layout->addWidget(page, 1);
    page->setFont(kobalt::ui::headerFooterFont());
    readerStyle(page, "font-family:@header-footer-font@;");
    dictionaryPanel = new kobalt::DictionaryPanel(this, services.keyboard);
    dictionaryPanel->prepareEditorFont = [](const QFont &font) { registerEditorFont(font); };
    dictionaryPanel->setDefinitionFont(settingsDictionaryFontFace, settingsDictionaryFontSize);
    dictionaryPanel->setLanguage(session->state().value("language").toString());
    dictionaryPanel->dismissed = [this] { dismissDictionary(); };
    dictionaryPanel->requested = [this](const QString &query) {
        if (lifecycleHidden || services.suspended() || closing)
            return;
        lookupText = query;
        const auto result = dictionary->lookup(book->identity(), lookupText);
        dictionaryPanel->setChoices(dictionary->choices());
        dictionaryPanel->setBookFont(bookFace());
        dictionaryPanel->open(lookupText, result, lookupBounds);
    };
    dictionaryPanel->searchRequested = [this](const QString &query) { openSearch(query); };
    dictionaryPanel->chosen = [this](const QString &id) {
        if (lifecycleHidden || services.suspended() || closing)
            return;
        kobalt::DictionaryResult result;
        QString error;
        if (dictionary->select(book->identity(), id, &error)) {
            result = dictionary->lookup(book->identity(), lookupText);
            if (!book->saveDictionaryChoice(id))
                result.error = strings::get("reader.could_not_save_reader_settings");
        } else
            result.error = error;
        dictionaryPanel->setChoices(dictionary->choices());
        dictionaryPanel->setBookFont(bookFace());
        dictionaryPanel->open(lookupText, result, lookupBounds);
    };
    linkPanel = new kobalt::LinkPanel(this);
    linkPanel->dismissed = [this] {
        dismissLookup();
        updateLinkReturn();
    };
    linkPanel->navigate = [this](const QString &target) { goToPassage(target); };
    linkReturn = new kobalt::ui::BackButton(strings::get("reader.return_to_passage"), this);
    linkReturn->setObjectName("rrLinkReturn");
    readerStyle(linkReturn, styles::outlined() + "QPushButton{min-height:0;padding:0;}");
    linkReturn->setFixedSize(metrics::links::returnWidth, metrics::links::returnHeight);
    linkReturn->hide();
    connect(linkReturn, &QPushButton::clicked, this, [this] {
        if (this->session->busy() || linkHistory.isEmpty() || lifecycleHidden || closing
            || services.suspended()) {
            return;
        }
        dismissLookup();
        dismissControls();
        returningFromLink = true;
        this->session->goTo(linkHistory.last().position);
        refresh();
    });
    searchPanel = new kobalt::SearchPanel(this, services.keyboard);
    searchPanel->requested = [this](const QString &query) {
        const auto epoch = ++searchEpoch;
        QPointer<Reader> guard(this);
        this->session->search(query, [guard, epoch](kobalt::SearchBatch batch) {
            if (!guard || guard->searchEpoch != epoch || guard->lifecycleHidden)
                return;
            guard->searchPanel->append(batch, guard->session->pages());
        });
    };
    searchPanel->dismissed = [this] { closeSearch(); };
    searchPanel->returnToStart = [this] {
        // Returning ends the search trip; the next search starts from here.
        const auto start = std::exchange(searchOriginal, QString());
        closeSearch();
        this->session->goTo(start);
    };
    searchPanel->selected = [this](kobalt::SearchHit hit) {
        closeSearch();
        pendingSearchHit = hit;
        this->session->goTo(hit.start);
    };
    page->held = [this](QPoint point) {
        if (beginAnnotationDrag(point, -1))
            finishAnnotationDrag(false);
    };
    imageViewer = new ImageViewer(this);
    imageViewer->rememberView = [this](QString id, QJsonObject view) {
        if (book->rememberImageView(id, view)) {
            QJsonObject snapshot;
            book->checkpoint(&snapshot, true);
        }
    };
    imageViewer->dismissed = [this] {
        ++imageRequest;
        page->setFocus();
        observeReading();
        page->cleanRefresh(true);
        updateOrientationBlock();
        progressTick();
    };
    page->imageHeld = [this](QPoint point) {
        if (session->busy() || lifecycleHidden || closing || exitPending || exportRunning
            || annotationSaving || services.suspended() || book->deviceTransition()
            || appearanceEdit.panelOpen() || general->isVisible() || searchPanel->isVisible()
            || linksActive() || !pageImageAtPoint(page->image, point))
            return false;
        // Small image-only note references keep their link/selection behavior.
        for (const auto &link : session->pageLinks())
            if (link.kind == LinkKind::Footnote)
                for (const auto &rect : link.rectangles)
                    if (rect.contains(point))
                        return false;
        dismissControls();
        dismissLookup();
        clearAnnotationDraft();
        const auto safe = safeArea();
        imageViewer->setGeometry(rect().adjusted(0, safe.top, 0, -safe.bottom));
        imageViewer->open();
        observeReading();
        updateOrientationBlock();
        const auto request = ++imageRequest;
        QPointer<Reader> guard(this);
        session->imageAt(point, [guard, request](QImage image) {
            if (guard && guard->imageRequest == request && guard->imageViewer->isVisible()
                && !guard->lifecycleHidden && !guard->closing) {
                const auto id = imageContentId(image);
                guard->imageViewer->setImage(std::move(image), guard->book->imageView(id));
            }
        });
        return true;
    };
    header = new ChapterHeader(page);
    const auto chrome = kobalt::headerfooter::select(preferences, services.phoneLayout);
    chromePreset = chrome.preset;
    page->chrome = chrome.layout;
}

void Reader::startClockTimer()
{
    book->onEstimatesChanged([this] {
        // A pending page already picks up the new pace in its normal paint.
        if (closing || lifecycleHidden || session->busy() || turnPainted || !isVisible()
            || services.suspended()) {
            return;
        }
        if (chapterGraph->isVisible()) {
            updateReadingStats();
        }
        if (!chromeTicks() || topControls->isVisible() || general->isVisible()
            || dictionaryPanel->isVisible() || searchPanel->isVisible() || linksActive()) {
            return;
        }
        const auto before = page->slotContents;
        showChapterText();
        if (page->slotContents != before) {
            page->refreshContent();
        }
    });
    // The clock and menu status change once a minute. Wake just after each
    // change instead of every second.
    clockTimer = new QTimer(this);
    clockTimer->setObjectName("rrClockTimer");
    clockTimer->setSingleShot(true);
    // Qt rounds a long coarse timer to whole seconds and can fire it up to half
    // a second early, before the minute changes; that costs a second wakeup.
    clockTimer->setTimerType(Qt::PreciseTimer);
    connect(clockTimer, &QTimer::timeout, this, [this] {
        if (!isVisible() || services.suspended())
            return;
        // A timer that fires before the minute changes only re-arms, so the
        // minute work runs once a minute.
        const int now = millisecondsOfDay();
        const bool due = now / 60000 != clockMinute;
        // The tick itself always re-arms from this minute.
        clockTimer->stop();
        startClock(now);
        if (!due)
            return;
        // The minute work runs whether or not a clock is shown.
        book->minuteTick();
        if (minuteTicked)
            minuteTicked();
        if (topControls->isVisible()) {
            updateMenuStatus();
            return;
        }
        if (!chromeTicks() || general->isVisible() || dictionaryPanel->isVisible()
            || searchPanel->isVisible() || linksActive())
            return;
        const auto before = page->slotContents;
        showChapterText();
        if (page->slotContents == before)
            return;
        // A clock tick changes the strips, not the book page. Keep its damage
        // out of the page-turn image and colour refresh policy.
        page->refreshContent();
    });
    startClock();
}

void Reader::buildToolbar()
{
    topControls = new kobalt::ui::Panel(this, Qt::BottomEdge);
    topControls->setObjectName("rrControlsTop");
    topControls->setFrameStyle(QFrame::Box);
    auto *toolbar = new QVBoxLayout(topControls);
    toolbarComposition.panel = topControls;
    toolbarComposition.rows = toolbar;
    toolbar->setContentsMargins(metrics::panel::toolbarMargins);
    toolbar->setSpacing(metrics::panel::statusGap);
    auto *status = new QGridLayout;
    status->setContentsMargins(metrics::panel::statusMargins);
    toolbar->addLayout(status);
    menuClock = new ui::InterfaceLabel(topControls);
    menuClock->setObjectName("rrMenuClock");
    menuClock->setFixedHeight(metrics::panel::statusHeight);
    menuClock->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    readerStyle(menuClock, styles::font(metrics::panel::statusText));
    status->addWidget(menuClock, 0, 0);
    menuSession = new ui::InterfaceLabel(topControls);
    menuSession->setObjectName("rrMenuSession");
    menuSession->setFixedHeight(metrics::panel::statusHeight);
    menuSession->setAlignment(Qt::AlignCenter);
    readerStyle(menuSession, styles::brand(metrics::panel::sessionText));
    status->addWidget(menuSession, 0, 1);
    status->setColumnStretch(0, 1);
    status->setColumnStretch(2, 1);
    menuBattery = new kobalt::ui::BatteryIndicator(topControls);
    menuBattery->setObjectName("rrMenuBattery");
    readerStyle(menuBattery, styles::font(metrics::panel::batteryText));
    status->addWidget(menuBattery, 0, 2, Qt::AlignRight);
    menuBluetooth = new kobalt::ui::BluetoothIndicator(topControls);
    menuBluetooth->setObjectName("rrMenuBluetooth");
    toolbarComposition.bluetooth = menuBluetooth;
    menuWifi = new kobalt::ui::WifiIndicator(topControls);
    menuWifi->setObjectName("rrMenuWifi");
    toolbarComposition.wifi = menuWifi;
    auto *top = new QHBoxLayout;
    toolbar->addLayout(top);
    top->setSpacing(metrics::panel::toolbarGap);
    updateMenuStatus();
    const auto returnLabel = services.backButtonLabel.isEmpty() ? strings::get("reader.exit")
                                                                : services.backButtonLabel;
    auto *back = new ReturnButton(returnLabel, topControls);
    back->setObjectName("rrReturn");
    back->setAccessibleName(returnLabel);
    back->setFocusPolicy(Qt::NoFocus);
    readerStyle(back, "QPushButton{border:none;padding:0;min-width:0;min-height:0;}");
    auto *returnRow = new QHBoxLayout(back);
    returnRow->setContentsMargins(0, 0, 0, 0);
    returnRow->setSpacing(0);

    auto *returnTitle = new ReturnTitle(returnLabel, back);
    returnTitle->setObjectName("rrReturnTitle");
    returnTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    returnTitle->setAlignment(Qt::AlignCenter);
    returnTitle->setContentsMargins(0, 0, metrics::chrome::inset, 0);
    readerStyle(returnTitle, styles::brand(metrics::chrome::titleText) + "background:transparent;");
    returnTitle->ensurePolished();
    returnRow->addSpacing(back->labelStart(returnTitle->font()));
    returnRow->addWidget(returnTitle, 1);
    toolbarComposition.exitAction = back;
    top->addWidget(back);
    const auto repaintTitle = [returnTitle] { returnTitle->update(); };
    connect(back, &QPushButton::pressed, returnTitle, repaintTitle);
    connect(back, &QPushButton::released, returnTitle, repaintTitle);
    connect(back, &QPushButton::clicked, this, [this] {
        // The chapter list's close cell sits where Exit Kobalt is. A second
        // tap that lands after the list closed must not leave the reader.
        if (exitGuarded && !exitGuardElapsed())
            return;
        close();
    });
    back->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    top->addStretch(1);
    auto *lightButton =
            iconButton(topControls, top, "rrLight", strings::get("reader.brightness_and_warmth"),
                       ControlIcon::Light, [this] { toggleLight(); });
    lightButton->setCheckable(true);
    lightButton->setEnabled(services.lightAvailable());
    light = services.light(lightButton);
    Q_ASSERT(light);
    warmthSupported = services.lightAvailable() && light->initialize() && light->hasWarmth();
    applyEdgeLight();
    lightPopup = new kobalt::LightPanel(*light, this);
    lightPopup->setEdgeGesturesEnabled(page->brightnessEdge);
    lightPopup->darkModeChanged = [this](bool enabled) { applyDarkMode(enabled); };
    lightPopup->changed = [lightButton, this] {
        lightButton->setChecked(lightPopup->isVisible());
        if (navigation && lightPopup->isVisible())
            navigation->stackPanel(lightPopup);
        syncNavigation();
    };
    lightPopup->placement = [this](int h) { return panelBounds(h); };
    lightPopup->availableHeight = [this] {
        return navigation ? navigation->contentBottom(size()) - navigation->toolbarHeight()
                          : height() - 2 * int(metrics::panel::toolbarHeight);
    };
    auto *typeButton =
            iconButton(topControls, top, "rrTypography", strings::get("reader.typography"),
                       ControlIcon::Typography, [this] { selectPanel(0); });
    typeButton->setFixedWidth(metrics::control::typographyButtonWidth);
    typeButton->setAccessibleName(strings::get("reader.typography"));
    typeButton->setToolTip(strings::get("reader.typography"));
    readerStyle(typeButton, styles::font(metrics::text::typography));
    typeButton->setCheckable(true);
    iconButton(topControls, top, "rrLayout", strings::get("reader.page_layout"),
               ControlIcon::Layout, [this] { selectPanel(1); })
            ->setCheckable(true);
    iconButton(topControls, top, "rrGraph", strings::get("reader.chapter_lengths"),
               ControlIcon::Graph, [this] { toggleGraph(); })
            ->setCheckable(true);
    toolbarComposition.search =
            iconButton(topControls, top, "rrSearchOpen", strings::get("reader.search_book"),
                       ControlIcon::Search,
                       [this] { activateNavigation(kobalt::NavigationDestination::Search); });
    toolbarComposition.sync =
            iconButton(topControls, top, "rrKosyncManual", strings::get("kosync.sync_now"),
                       ControlIcon::Sync, [this] { requestManualKosync(); });
    toolbarComposition.sync->hide();
    toolbarComposition.bookmarks =
            iconButton(topControls, top, "rrBookmarks", strings::get("reader.bookmarks"),
                       ControlIcon::Bookmark, [this] { openBookmarks(); });
    toolbarComposition.bookmarks->setEnabled(bookmarks->available());
    toolbarComposition.annotate =
            iconButton(topControls, top, "rrAnnotate", strings::get("annotations.title"),
                       ControlIcon::Annotate, [this] {
                           if (annotationMode)
                               leaveAnnotations();
                           else
                               enterAnnotations();
                       });
    toolbarComposition.annotate->setCheckable(true);
    toolbarComposition.annotate->setEnabled(highlights->writable());
    toolbarComposition.formattingWarning = iconButton(
            topControls, top, "rrFormattingWarning", strings::get("reader.formatting_warning"),
            ControlIcon::FileWarning, [this] { showFormattingWarning(); });
    toolbarComposition.formattingWarning->setVisible(session->formattingWarning());

    toolbarComposition.settings =
            iconButton(topControls, top, "rrSettings", strings::get("reader.reader_settings"),
                       ControlIcon::Settings, [this] {
                           // The button stays enabled while a panel's changes apply, so the
                           // visible top bar does not repaint twice. Ignore taps then.
                           if (appearanceEdit.applying())
                               return;
                           if (general->isVisible())
                               applySettings();
                           else
                               openSettings();
                       });
    toolbarComposition.settings->setCheckable(true);
    toolbarComposition.clock = menuClock;
    toolbarComposition.session = menuSession;
    toolbarComposition.battery = menuBattery;
    for (auto *button :
         topControls->findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (button == back)
            continue;
        if (button->objectName() != "rrSettings" && button->objectName() != "rrBookmarks"
            && button->objectName() != "rrAnnotate" && button->objectName() != "rrFormattingWarning"
            && button->objectName() != "rrSearchOpen" && button->objectName() != "rrKosyncManual")
            topActions.append(button);
        if (auto *action = dynamic_cast<kobalt::ui::Button *>(button))
            action->setPaintMode(kobalt::ui::ButtonPaintMode::ToolbarSquare);
        button->setFixedSize(metrics::control::toolbarButtonSize,
                             metrics::control::toolbarButtonSize);
        readerStyle(button,
                    "QPushButton{border:none;padding:0;min-height:0;}"
                            + (button == typeButton ? styles::font(metrics::text::typography)
                                                    : QString()));
    }
}

void Reader::buildBottomControls()
{
    bottomControls = new kobalt::ui::Panel(this, Qt::TopEdge);
    bottomControls->setObjectName("rrControlsBottom");
    bottomControls->setFrameStyle(QFrame::Box);
    auto *bottom = new QVBoxLayout(bottomControls);
    bottom->setContentsMargins(metrics::panel::footerMargins);
    bottom->setSpacing(metrics::panel::footerGap);
    bottom->addWidget(linkReturn, 0, Qt::AlignLeft);
    pageScrubber = new kobalt::ui::PageScrubber(this);
    pageScrubber->setPreviewDismissible(false);
    bookSlider = pageScrubber->slider();
    pageScrubber->linksRequested = [this] { showRevealedLinks(); };
    pageScrubber->returnRequested = [this] { linkReturn->click(); };
    pageScrubber->stayRequested = [this] {
        if (navigation && navigation->stayRequested) {
            navigation->stayRequested();
        }
    };
    pageScrubber->imageRequested = [this] {
        const auto target = pageSingleImage(page->image);
        if (!target.isEmpty() && page->imageHeld)
            page->imageHeld(target.center());
    };
    pageScrubber->imageDismissed = [this] {
        imageSuggestionDismissed = true;
        syncNavigation();
    };
    pageScrubber->previewRequested = [this](int target, QSize bounds) {
        if (closing || lifecycleHidden || services.suspended())
            return;
        QPointer<kobalt::ui::PageScrubber> scrubber(pageScrubber);
        session->previewPage(target, bounds, [scrubber, target](QImage image) {
            if (scrubber)
                scrubber->setPreview(target, std::move(image));
        });
    };
    pageScrubber->cancelled = [this] {
        session->cancelPagePreview();
        seekOpen = false;
    };
    pageScrubber->accepted = [this](int target) {
        if (session->busy() || closing || lifecycleHidden || services.suspended())
            return;
        if (target == session->page()) {
            dismissControls();
            return;
        }
        const LinkVisit origin{ session->state().value("position").toString(), session->page(),
                                session->chapterTitle().toUpper() };
        if (!session->go(target))
            return;
        pendingPassage = origin;
        flashAfterPagePreview = true;
        dismissControls();
    };
    auto *footer = new QHBoxLayout;
    bottom->addLayout(footer);
    auto *chapters = new kobalt::ui::ElidingButton(strings::get("reader.contents"), bottomControls);
    chapters->setObjectName("rrChapters");
    footer->addWidget(chapters, 1);
    connect(chapters, &QPushButton::clicked, this, [this] { showChapters(); });
    chapterButton = chapters;
    chapterButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    readerStyle(chapters, "text-align:left;");
    buttonIcon(chapters, ControlIcon::Chapters);
    chapters->setIconSize(
            QSize(metrics::control::chapterIconSize, metrics::control::chapterIconSize));
}

kobalt::reader::AppearanceLayout Reader::buildMenu()
{
    menu = new AppearanceSheet(this);
    panels = menu->pages;
    previewLabel = menu->message;
    headerFooterView = new kobalt::reader::HeaderFooterView(this);
    auto layout = menu->controls();
    layout.headerFooter = headerFooterView->body();
    layout.headerFooterRows = headerFooterView->rows();
    return layout;
}

void Reader::buildOverlays()
{
    // Link discovery takes its styles in syncLinkDiscovery: they follow the
    // place it occupies and the reading strip's text size.
    linkDiscovery = new kobalt::ui::Panel(this);
    linkDiscovery->setObjectName("rrLinkDiscovery");
    linkDiscovery->setEdgeWidth(metrics::chrome::heavyRule);
    auto *linkRow = new QHBoxLayout(linkDiscovery);
    linkRow->setSpacing(metrics::links::hintGap);
    linkDiscoveryIcon = new ui::InterfaceLabel(linkDiscovery);
    // The link glyph has more space around it than the undo icon in the return
    // row: a larger drawing in the same column matches its size.
    linkDiscoveryIcon->setFixedSize(metrics::chrome::cellIcon, metrics::control::iconSize);
    linkDiscoveryIcon->setAlignment(Qt::AlignCenter);
    linkRow->addWidget(linkDiscoveryIcon);
    linkDiscoveryHint =
            new ui::InterfaceLabel(strings::get("reader.long_press_link"), linkDiscovery);
    linkDiscoveryHint->setObjectName("rrLinkDiscoveryHint");
    linkRow->addWidget(linkDiscoveryHint, 1);
    linkDiscoveryToggle = makeButton(linkDiscovery, linkRow, "rrRevealLinks", {},
                                     [this] { showRevealedLinks(); });
    linkDiscoveryExit = new ui::InterfaceLabel(
            strings::get("reader.tap_elsewhere_to_exit").toUpper(), linkDiscovery);
    linkDiscoveryExit->setObjectName("rrLinkDiscoveryExit");
    linkRow->addWidget(linkDiscoveryExit);
    linkDiscovery->hide();
    chapterGraph = new kobalt::ChapterGraph(this);
    chapterGraph->changed = [this] { saveSoon(); };
    chapterGraph->setDense(preferences.value("dense_stats_chart").toBool(false));
    choices = new ChoiceScreen(this);
    choices->hide();
    pinnedBookmarks = new BookmarkTray(this);
    pinnedBookmarks->setObjectName("rrPinnedBookmarks");
    ui::readerStyle(pinnedBookmarks, "QFrame#rrPinnedBookmarks{background:transparent;}");
    pinnedBookmarks->hide();
    for (int i = 0; i < BookmarkStore::maxPins; ++i) {
        auto *button = new ui::CellButton(ui::ControlIcon::Star, pinnedBookmarks,
                                          qRound(1.2 * int(metrics::chrome::cellIcon)));
        button->setCircular(true);
        button->setObjectName(QString("rrPinnedBookmark%1").arg(i));
        pinnedButtons.append(button);
        connect(button, &QPushButton::clicked, this, [this, button] {
            if (button->isEnabled())
                goToPassage(button->property("bookmark_position").toString(), true);
        });
    }
    bookmarkList = new kobalt::BookmarkList(this, services.keyboard);
    bookmarkList->pinRequested = [this](const QString &position, const QString &name,
                                        const QString &icon) {
        return bookmarks->setPin(position, name, icon);
    };
    bookmarkList->actionsRequested = [this] { openMarksActions(); };
    bookmarkList->annotationDetails = [this](const QStringList &ids, QVector<BookmarkRow> *rows) {
        QVector<HighlightExcerpt> excerpts;
        if (!highlights->excerpts(ids, &excerpts))
            return false;
        rows->clear();
        rows->reserve(excerpts.size());
        for (const auto &excerpt : excerpts)
            rows->append({ excerpt.id, excerpt.chapter, excerpt.text, {}, -1, false });
        return true;
    };
    bookmarkList->loadingCancelled = [session = QPointer<HighlightSession>(highlights.get())] {
        if (session)
            session->cancelLookup();
    };
    bookmarkList->setSortOrder(preferences.value("marks_sort").toString("position"));
    bookmarkList->sortRequested = [this](const QString &order) {
        auto next = preferences;
        next["marks_sort"] = order;
        if (writePreferences(next))
            preferences = next;
        else
            bookmarkList->setSortOrder(preferences.value("marks_sort").toString("position"));
    };
    bookmarkList->selected = [this](const QString &position) {
        if (bookmarkList->annotationsShown()) {
            for (const auto &item : highlights->highlights()) {
                if (item.id == position) {
                    goToPassage(item.start);
                    break;
                }
            }
        } else {
            goToPassage(position);
        }
    };
    bookmarkList->tabRequested = [this](bool annotations) {
        if (!features::enabled(preferences,
                               annotations ? features::Annotations : features::Bookmarks))
            return;
        annotationPopover->hide();
        if (annotations) {
            const bool ready = highlights->resolved() || highlights->sync();
            bookmarkList->showAnnotations(ready ? annotationRows() : QVector<BookmarkRow>(),
                                          !ready);
        } else {
            bookmarkList->showBookmarks(bookmarkRows());
        }
    };
    bookmarkList->back = [this] {
        annotationPopover->hide();
        bookmarkList->hide();
        guardExit();
        showControls();
        refresh();
    };
    bookmarkList->removeRequested = [this](const QString &position) {
        if (bookmarkList->annotationsShown()) {
            removeAnnotation(position);
        } else {
            askRemoveBookmark(position);
        }
    };
    choices->changed = [this] {
        if (choosingTypography && choices->isVisible())
            syncNavigation();
        saveSoon();
    };
    choices->back = [this] {
        choices->hide();
        if (choosingTypography) {
            menu->show();
            if (navigation)
                navigation->stackPanel(menu);
            else
                menu->raise();
        } else {
            guardExit();
            showControls();
        }
        refresh();
    };
}

void Reader::buildAppearance(const kobalt::reader::AppearanceLayout &layout)
{
    auto *typeRows = layout.typeRows, *pageRows = layout.pageRows;
    kobalt::reader::AppearancePanelInputs appearanceInputs;
    appearanceInputs.phone = services.phoneLayout;
    appearanceInputs.sideButtons = services.pageButtons();
    appearanceInputs.reverseLandscapeButtons = services.reverseLandscapeButtons();
    appearanceInputs.automaticRotationAvailable = orientation->automaticAvailable();
    appearanceInputs.systemRotation = services.systemRotation;
    kobalt::reader::AppearancePanel appearance(layout, appearanceInputs);
    fontButton = appearance.fontButton;
    sizeSlider = appearance.sizeSlider;
    opticalSizeSlider = appearance.opticalSizeSlider;
    weightSlider = appearance.weightSlider;
    variableWeightSlider = appearance.variableWeightSlider;
    spacingSlider = appearance.spacingSlider;
    marginSlider = appearance.marginSlider;
    verticalMarginSlider = appearance.verticalMarginSlider;
    appearancePreview = new kobalt::reader::AppearancePreview(this);
    appearancePreview->followPanel(menu, [this] { syncAppearancePreview(); });
    connect(panels, &QStackedWidget::currentChanged, this, [this] { syncAppearancePreview(); });
    const auto previewMargins = [this](kobalt::ui::SettingSlider *slider,
                                       kobalt::reader::AppearancePreview::Axis axis) {
        // The guides appear with the next page. A drag or a press draws once, when it is ready.
        slider->adjusting = [this, axis] { appearancePreviewAxis = axis; };
        connect(slider, &QSlider::valueChanged, this, [this] { syncAppearancePreview(); });
    };
    previewMargins(marginSlider, kobalt::reader::AppearancePreview::Axis::Horizontal);
    previewMargins(verticalMarginSlider, kobalt::reader::AppearancePreview::Axis::Vertical);
    alignmentButtons = appearance.alignmentButtons;
    hyphenButtons = appearance.hyphenButtons;
    rotationButtons = appearance.rotationButtons;
    autoRotateOption = appearance.autoRotateOption;
    connect(fontButton, &QPushButton::clicked, this, [this] {
        if (choices->isVisible() && choosingTypography) {
            choices->hide();
            refresh();
            return;
        }
        showFontPicker();
    });
    sizeSlider->committed = [this](int value) {
        const int pixels = kobalt::halfPointPixels(value);
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        if (appearanceEdit.queued() || pixels != settings->fontSize())
            mutateAppearance(
                    [pixels](ReaderSession &preview) { preview.style(pixels, preview.spacing()); });
    };
    connect(sizeSlider, &QSlider::valueChanged, this, [this](int value) {
        findChild<QLabel *>("rrTextSizeValue")
                ->setText(strings::get("reader.text_size_value")
                                  .arg(QString::number(value / 2.0, 'f', value % 2)));
    });
    opticalSizeSlider->committed = [this](int value) {
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        if (appearanceEdit.queued() || value != settings->opticalSize())
            mutateAppearance([value](ReaderSession &preview) { preview.setOpticalSize(value); });
    };
    connect(opticalSizeSlider, &QSlider::valueChanged, this, [this](int value) {
        findChild<QLabel *>("rrOpticalSizeValue")
                ->setText(strings::get("reader.text_size_value").arg(QString::number(value)));
    });
    weightSlider->committed = [this](int value) {
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        if (appearanceEdit.queued() || value != settings->thickness())
            mutateAppearance([value](ReaderSession &preview) { preview.setThickness(value); });
    };
    variableWeightSlider->committed = [this](int value) {
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        if (appearanceEdit.queued() || value != settings->weight())
            mutateAppearance([value](ReaderSession &preview) { preview.setWeight(value); });
    };
    connect(variableWeightSlider, &QSlider::valueChanged, this, [this](int value) {
        findChild<QLabel *>("rrVariableFontWeightValue")->setText(QString::number(value));
    });
    spacingSlider->committed = [this](int value) {
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        if (appearanceEdit.queued() || value != settings->spacing())
            mutateAppearance(
                    [value](ReaderSession &preview) { preview.style(preview.fontSize(), value); });
    };
    marginSlider->committed = [this](int value) {
        const int pixels = kobalt::readingPixels(value);
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        if (appearanceEdit.queued() || pixels != settings->margin())
            mutateAppearance([pixels](ReaderSession &preview) {
                preview.style(preview.fontSize(), preview.spacing(), preview.fontFace(), pixels);
            });
    };
    verticalMarginSlider->committed = [this](int value) {
        const int pixels = kobalt::readingPixels(value);
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        if (appearanceEdit.queued() || pixels != settings->topMargin()
            || pixels != settings->bottomMargin())
            mutateAppearance(
                    [pixels](ReaderSession &preview) { preview.setVerticalMargins(pixels); });
    };
    for (auto *button : alignmentButtons) {
        const QString value = button->property("alignment").toString();
        connect(button, &QPushButton::clicked, this, [this, value] {
            const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                    ? appearanceEdit.preview()
                    : session;
            if (appearanceEdit.queued() || value != settings->alignment())
                mutateAppearance([value](ReaderSession &preview) {
                    preview.style(preview.fontSize(), preview.spacing(), preview.fontFace(),
                                  preview.margin(), value);
                });
        });
    }
    for (auto *button : hyphenButtons) {
        const QString value = button->property("hyphenation").toString();
        connect(button, &QPushButton::clicked, this, [this, value] {
            const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                    ? appearanceEdit.preview()
                    : session;
            if (appearanceEdit.queued() || value != settings->hyphenation())
                mutateAppearance(
                        [value](ReaderSession &preview) { preview.setHyphenation(value); });
        });
    }
    for (auto *button : rotationButtons) {
        const int value = button->property("orientation").toInt();
        connect(button, &QPushButton::clicked, this, [this, value] {
            if (rotationControlsBlocked()) {
                syncReadingControls();
                return;
            }
            kobalt::diagnostic("reader-orientation-request",
                               { { "target_orientation", value },
                                 { "current_orientation", int(orientation->manual()) },
                                 { "width", width() },
                                 { "height", height() },
                                 { "main_busy", session->busy() },
                                 { "preview_busy",
                                   appearanceEdit.preview() && appearanceEdit.preview()->busy() },
                                 { "updates_enabled", updatesEnabled() },
                                 { "resize_pending", resizePending } },
                               false);
            this->orientation->configure(
                    kobalt::qt::object({ { "orientation", value }, { "auto_rotate", false } }));
            kobalt::diagnostic("reader-orientation-dispatched",
                               { { "target_orientation", value },
                                 { "width", width() },
                                 { "height", height() },
                                 { "main_busy", session->busy() },
                                 { "preview_busy",
                                   appearanceEdit.preview() && appearanceEdit.preview()->busy() },
                                 { "updates_enabled", updatesEnabled() },
                                 { "resize_pending", resizePending } },
                               false);
            refresh();
        });
    }
    if (autoRotateOption)
        connect(autoRotateOption, &QPushButton::clicked, this, [this] {
            if (rotationControlsBlocked()) {
                syncReadingControls();
                return;
            }
            appearanceEdit.setError(
                    this->orientation->setAutomatic(true)
                            ? QString()
                            : strings::get("reader.the_orientation_sensor_is_unavailable"));
            refresh();
        });
    headerFooterRow = appearance.headerFooterRow;
    slotMap = appearance.slotMap;
    presetCards = appearance.presetCards;
    headerSizeControl = appearance.headerSizeControl;
    footerSizeControl = appearance.footerSizeControl;
    progressBarButtons = appearance.progressBarButtons;
    connect(headerFooterRow, &QPushButton::clicked, this, [this] { openHeaderFooterView(); });
    // Close returns to the Layout panel, which stayed open beneath the view.
    headerFooterView->closed = [this] { closeChromeViews(); };
    for (auto *card : presetCards) {
        const auto preset = card->preset();
        connect(card, &QPushButton::clicked, this, [this, preset] {
            if (session->temporaryTypography()) {
                syncChromeControls();
                return;
            }
            if (preset != chromePreset) {
                // Custom starts from the preset in force until it has a layout of its own.
                const auto custom = customChrome();
                saveChrome(preset,
                           preset == kobalt::headerfooter::Preset::Custom ? &custom : nullptr);
            }
        });
    }
    // A slot starts an edit, so a preset in force may replace Custom first.
    slotMap->chosen = [this](int slot) {
        confirmCustomReplace(
                [this, slot] {
                    if (chromePreset != kobalt::headerfooter::Preset::Custom) {
                        const auto start = page->chrome;
                        if (session->temporaryTypography()
                            || !saveChrome(kobalt::headerfooter::Preset::Custom, &start))
                            return;
                    }
                    openFieldChooser(slot);
                },
                [] {});
    };
    // An edit starts from the layout in force, so a preset can be adjusted.
    // The result is the shared Custom layout, and the book switches to Custom.
    const auto editCustom =
            [this](const std::function<void(kobalt::headerfooter::Layout &)> &edit) {
                // The pressed segment already looks chosen; Cancel puts it back.
                confirmCustomReplace(
                        [this, edit] {
                            auto custom = page->chrome;
                            edit(custom);
                            if (session->temporaryTypography()
                                || !saveChrome(kobalt::headerfooter::Preset::Custom, &custom))
                                syncChromeControls();
                        },
                        [this] { syncChromeControls(); });
            };
    for (auto *control : { headerSizeControl, footerSizeControl }) {
        const bool footer = control == footerSizeControl;
        control->slider->committed = [this, footer](int value) {
            auto text = page->chrome;
            (footer ? text.footerEnabled : text.headerEnabled) =
                    value != kobalt::headerfooter::offSize;
            if (value != kobalt::headerfooter::offSize)
                (footer ? text.footerSize : text.headerSize) = value;
            if (session->temporaryTypography() || !saveChrome(chromePreset, nullptr, &text))
                syncChromeControls();
        };
    }

    for (auto *button : progressBarButtons) {
        const bool on = button->property("switch_on").toBool();
        connect(button, &QPushButton::clicked, this, [editCustom, on] {
            editCustom([on](kobalt::headerfooter::Layout &custom) { custom.progressBar = on; });
        });
    }
    fieldChooser = new kobalt::reader::FieldChooser(this);
    fieldChooser->selected = [this, editCustom](kobalt::headerfooter::Field field) {
        const int slot = fieldChooser->slot();
        editCustom([slot, field](kobalt::headerfooter::Layout &custom) {
            custom.slotFields[slot] = field;
        });
        // A choice ends the list, as choosing a chapter does.
        closeFieldChooser();
    };
    fieldChooser->uppercaseChanged = [this, editCustom](bool on) {
        const int slot = fieldChooser->slot();
        editCustom(
                [slot, on](kobalt::headerfooter::Layout &custom) { custom.uppercase[slot] = on; });
    };
    fieldChooser->done = [this] { closeFieldChooser(); };
    // The views belong to the Layout panel and leave with it. The Text tab
    // replaces it with the Text page, which also closes them.
    menu->installEventFilter(this);
    connect(panels, &QStackedWidget::currentChanged, this, [this](int index) {
        if (index == int(kobalt::reader::AppearancePage::Typography))
            closeChromeViews();
    });
    syncChromeControls();
    // Every appearance page uses the same row gap.
    for (auto *rows : { typeRows, pageRows, layout.headerFooterRows }) {
        rows->setSpacing(metrics::control::rowGap);
        rows->addStretch();
    }
}

void Reader::buildSettings()
{
    kobalt::reader::SettingsPanelInputs settingsInputs;
    settingsInputs.keyboard = services.keyboard;
    settingsInputs.downloadableDictionaries = services.downloadableDictionaries;
    settingsInputs.animationSupport = services.animationSupport();
    settingsInputs.lightSupported = services.lightAvailable();
    settingsInputs.warmthSupported = warmthSupported;
    settingsInputs.sleepScreenSupported = presentation->supportsSleepScreen();
    settingsInputs.automaticBookOpeningSupported =
            services.automaticBookOpening && services.setAutomaticBookOpening;
    settingsInputs.wifiPowerSaverSupported = bool(services.turnOffWifi);
    settingsInputs.readerStandbySupported = services.readerStandbySupported;
    settingsInputs.debugProgressAvailable = bool(services.development.simulateProgress);
    auto *settings = new kobalt::reader::SettingsPanel(this, settingsInputs);
    general = settings;
    kosyncPage = settings->kosync;
    buildKosync();
    settings->refitRequested = [this] {
        fitSettings();
        refresh();
    };
    connect(settings->closeButton, &QPushButton::clicked, this, [this] {
        if (appearanceEdit.applying())
            return;
        guardExit();
        applySettings();
    });
    settings->showDictionaries(services.downloadableDictionaries
                               && !(services.devicePreview && services.devicePreview()));
    if (services.downloadableDictionaries) {
        auto *manager = static_cast<kobalt::DictionaryManager *>(dictionary.get());
        auto *dictionarySettings = settings->dictionaries;
        dictionarySettings->setLanguage(session->state().value("language").toString());
        manager->setLanguage(session->state().value("language").toString());
        const auto catalog = kobalt::DictionaryManager::catalog();
        dictionarySettings->setCatalog(catalog);
        auto updateDictionaries = [this, manager, dictionarySettings, catalog,
                                   previous = QSet<QString>{},
                                   sizes = QHash<QString, qint64>{}]() mutable {
            QSet<QString> installed;
            for (const auto &entry : catalog)
                if (manager->installed(entry.id))
                    installed.insert(entry.id);
            if (installed != previous) {
                sizes = manager->installedSizes();
                previous = installed;
            }
            dictionarySettings->setStorage(sizes, manager->availableBytes());
            const auto transfer = manager->transfer();
            dictionarySettings->setTransfer(transfer.id, transfer.received, transfer.total,
                                            transfer.installing);
            dictionarySettings->setState(installed, manager->busy(), manager->status());
            dictionaryPanel->setChoices(manager->choices());
        };
        dictionarySettings->installRequested = [manager](const QString &id) {
            manager->download(id);
        };
        dictionarySettings->removeRequested = [manager, dictionarySettings,
                                               catalog](const QString &id) {
            QString error;
            if (!manager->remove(id, &error)) {
                QSet<QString> installed;
                for (const auto &entry : catalog)
                    if (manager->installed(entry.id))
                        installed.insert(entry.id);
                dictionarySettings->setState(installed, manager->busy(), error);
            }
        };
        dictionarySettings->cancelRequested = [manager] { manager->cancel(); };
        connect(manager, &kobalt::DictionaryManager::changed, this, updateDictionaries);
        updateDictionaries();
    }
    settingsPages = settings->settingsPages;
    darkOption = settings->darkOption;
    inheritTextLayoutOption = settings->inheritTextLayoutOption;
    animationOption = settings->animationOption;
    edgeLightOption = settings->edgeLightOption;
    lightGestureFeedbackOption = settings->lightGestureFeedbackOption;
    denseGraphOption = settings->denseGraphOption;
    sleepScreenButton = settings->sleepScreenButton;
    sessionTimeoutButton = settings->sessionTimeoutButton;
    pageNavigationButton = settings->pageNavigationButton;
    tapZonesButton = settings->tapZonesButton;
    tappableLinksButton = settings->tappableLinksButton;
    navigationBarButton = settings->navigationBarButton;
    chapterGroupsButton = settings->chapterGroupsButton;
    menuViewButton = settings->menuViewButton;
    connect(menuViewButton, &QPushButton::clicked, this, [this] { openMenuView(); });
    minimalAnnotationOption = settings->minimalAnnotationOption;
    automaticOpeningOption = settings->automaticOpeningOption;
    hideTabLabelsOption = settings->hideTabLabelsOption;
    zenModeOption = settings->zenModeOption;
    wifiPowerSaverOption = settings->wifiPowerSaverOption;
    disableStandbyOption = settings->disableStandbyOption;
    settings->showPowerManagement(wifiPowerSaverVisible());
    hideTabLabelsOption->setVisible(!services.phoneLayout);
    hideNonlinearOption = settings->hideNonlinearOption;
    connect(settings->columnSpacingButton, &QPushButton::clicked, this, [this, settings] {
        settings->showColumnSpacingChoices(settingsColumnSpacing, [this, settings](int percent) {
            settingsColumnSpacing = percent;
            settings->columnSpacingButton->setCaption(SettingsPanel::columnSpacingLabel(percent));
        });
    });
    connect(settings->wordSpacingButton, &QPushButton::clicked, this, [this, settings] {
        settings->showAdvancedTextChoices(
                SettingsPanel::AdvancedText::WordSpacing,
                stylingChoice(int(SettingsPanel::AdvancedText::WordSpacing)), [this](int index) {
                    settingsWordSpacing = 80 + index * 20;
                    showAdvancedText(int(SettingsPanel::AdvancedText::WordSpacing));
                });
    });
    connect(settings->beautifierButton, &QPushButton::clicked, this, [this, settings] {
        settings->showBeautifier(settingsBeautifier, [this](QJsonObject value) {
            settingsBeautifier = std::move(value);
            showAdvancedText();
        });
    });
    connect(settings->paragraphSpacingButton, &QPushButton::clicked, this, [this, settings] {
        settings->showParagraphAdjustments(settingsParagraphSpacing, settingsParagraphIndent,
                                           [this](int percent, int indent) {
                                               settingsParagraphSpacing = percent;
                                               settingsParagraphIndent = indent;
                                               showAdvancedText();
                                           });
    });
    connect(settings->trackingButton, &QPushButton::clicked, this, [this, settings] {
        settings->showAdvancedTextChoices(
                SettingsPanel::AdvancedText::Tracking,
                stylingChoice(int(SettingsPanel::AdvancedText::Tracking)),
                [this](int index) {
                    settingsTracking = index == 2 ? 15 : index == 1 ? 5 : 0;
                    showAdvancedText(int(SettingsPanel::AdvancedText::Tracking));
                },
                session->alignment());
    });
    connect(settings->hangingPunctuationButton, &QPushButton::clicked, this, [this, settings] {
        settings->showAdvancedTextChoices(
                SettingsPanel::AdvancedText::HangingPunctuation, settingsHangingPunctuation ? 1 : 0,
                [this](int index) {
                    settingsHangingPunctuation = index != 0;
                    showAdvancedText(int(SettingsPanel::AdvancedText::HangingPunctuation));
                });
    });
    connect(settings->widowsOrphansButton, &QPushButton::clicked, this, [this, settings] {
        settings->showAdvancedTextChoices(
                SettingsPanel::AdvancedText::Pagination,
                stylingChoice(int(SettingsPanel::AdvancedText::Pagination)), [this](int index) {
                    settingsWidowsOrphans = index == 2 ? "allow"
                            : index == 1               ? "avoid"
                                                       : "publisher";
                    showAdvancedText(int(SettingsPanel::AdvancedText::Pagination));
                });
    });
    connect(settings->verticalSpacingButton, &QPushButton::clicked, this, [this, settings] {
        settings->showPageSpacingChoices(
                stylingChoice(int(SettingsPanel::AdvancedText::VerticalSpacing)),
                settingsVerticalSpacingScope == "protection" ? 1 : 0, [this](int mode, int scope) {
                    const QString nextScope = scope ? "protection" : "all";
                    const bool scopeChanged = settingsVerticalSpacingScope != nextScope;
                    settingsVerticalSpacing = QStringList{ "off", "center", "expand" }.at(mode);
                    settingsVerticalSpacingScope = nextScope;
                    showAdvancedText(
                            scopeChanged ? -1 : int(SettingsPanel::AdvancedText::VerticalSpacing));
                });
    });
    bionicOption = settings->bionicOption;
    refreshButton = settings->refreshButton;
    resetDefaults = settings->resetDefaults;
    settingsError = settings->settingsError;
    connect(settings->uiFontButton, &QPushButton::clicked, this,
            [this] { openSettingsFont("ui_font_face"); });
    connect(settings->dictionaryFontButton, &QPushButton::clicked, this,
            [this] { openSettingsFont("dictionary_font_face"); });
    connect(settings->dictionaryFontSizeButton, &QPushButton::clicked, this, [this, settings] {
        const auto family =
                settingsDictionaryFontFace.isEmpty() ? bookFace() : settingsDictionaryFontFace;
        registerPickerFonts(family);
        settings->showDictionaryFontSizes(settingsDictionaryFontSize, family,
                                          !settingsDictionaryFontFace.isEmpty(),
                                          [this](int percent) {
                                              settingsDictionaryFontSize = percent;
                                              showSettingsFonts();
                                          });
        settingsFontPopup = settings->fontDialog;
        if (settingsFontPopup)
            settingsFontPopup->installEventFilter(this);
    });
    connect(settings->uiMonoFontButton, &QPushButton::clicked, this,
            [this] { openSettingsFont("ui_mono_font_face"); });
    connect(settings->headerFooterFontButton, &QPushButton::clicked, this,
            [this] { openSettingsFont("header_footer_font_face"); });
    connect(settings->timeRemainingFormatButton, &QPushButton::clicked, this, [this, settings] {
        settings->showTimeRemainingFormats(settingsTimeRemainingFormat,
                                           [this](const QString &format) {
                                               settingsTimeRemainingFormat = format;
                                               showSettingsFonts();
                                           });
    });
    connect(settings->headerFooterWeightButton, &QPushButton::clicked, this, [this, settings] {
        auto sampleFont = ui::headerFooterFont(page->headerTextPixels());
        if (!settingsHeaderFooterFontFace.isEmpty())
            sampleFont.setFamily(settingsHeaderFooterFontFace);
        settings->showHeaderFooterWeights(settingsHeaderFooterWeight, sampleFont,
                                          [this](int steps) {
                                              settingsHeaderFooterWeight = steps;
                                              showSettingsFonts();
                                              refresh();
                                          });
    });
    connect(settings->defaultFontButton, &QPushButton::clicked, this,
            [this] { openSettingsFont("default_font_face"); });
    int familyIndex = 0;
    for (auto *button :
         { settings->serifFontButton, settings->sansFontButton, settings->monoFontButton }) {
        const int index = familyIndex++;
        connect(button, &QPushButton::clicked, this,
                [this, index] { openSettingsFont(appearance::fallbackFontKeys[index]); });
    }
    settings->stylingRequested = [this](SettingsPanel::AdvancedText feature) {
        renderStylingSamples(int(feature));
    };
    settings->stylingHidden = [this] { cancelStylingSamples(); };
    connect(darkOption, &QCheckBox::clicked, this,
            [this](bool enabled) { applyDarkMode(enabled); });
    connect(bionicOption, &QCheckBox::clicked, this,
            [this](bool enabled) { applyBoldReading(enabled); });
    connect(sleepScreenButton, &QPushButton::clicked, this, [this] { openSleepScreen(); });
    connect(sessionTimeoutButton, &QPushButton::clicked, this, [this] { openSessionTimeout(); });
    connect(tappableLinksButton, &QPushButton::clicked, this, [this] { openTappableLinks(); });
    connect(tapZonesButton, &QPushButton::clicked, this, [this] { openTapZones(); });
    connect(chapterGroupsButton, &QPushButton::clicked, this, [this] { openChapterGroups(); });
    connect(navigationBarButton, &QPushButton::clicked, this,
            [this] { openNavigationBarButtons(); });
    connect(pageNavigationButton, &QPushButton::clicked, this, [this] { openPageNavigation(); });
    settings->updateButtonControls(buttonControls(), preferences, false);
    connect(settings->buttonControlsButton, &QPushButton::clicked, this, [this, settings] {
        const auto buttons = buttonControls();
        if (buttons.isEmpty())
            return;
        settings->showButtonControls(buttons, preferences, [this](const QJsonObject &actions) {
            saveButtonControls(actions);
        });
        settingsFontPopup = settings->fontDialog;
        if (settingsFontPopup) {
            settingsFontPopup->installEventFilter(this);
            for (auto *widget : settingsFontPopup->findChildren<QWidget *>())
                textRefresh(widget);
            textRefresh(settingsFontPopup);
        }
    });
    settings->automaticOpeningChosen = [this](bool enabled) { applyAutomaticOpening(enabled); };
    connect(refreshButton, &QPushButton::clicked, this, [this] { openScreenRefresh(); });
    connect(settings->openAcknowledgments, &QPushButton::clicked, this, [this] {
        kobalt::ui::PanelTiming timing("acknowledgments");
        acknowledgments->open();
        timing.finish(acknowledgments);
        for (auto *widget : acknowledgments->findChildren<QWidget *>())
            textRefresh(widget);
    });
    connect(settings->confirmDefaults, &QPushButton::clicked, this, [this] {
        restoringDefaults = true;
        settingsUiFontFace.clear();
        settingsUiMonoFontFace.clear();
        settingsHeaderFooterFontFace.clear();
        settingsDictionaryFontFace.clear();
        settingsDictionaryFontSize = 100;
        settingsHeaderFooterWeight = 2;
        settingsTimeRemainingFormat = "detailed";
        hideTabLabelsOption->setChecked(false);
        zenModeOption->setChecked(false);
        if (wifiPowerSaverOption)
            wifiPowerSaverOption->setChecked(false);
        for (const auto *key : appearance::fallbackFontKeys)
            settingsFontFaces[key] = QString();
        showSettingsFonts();
        darkOption->setChecked(false);
        animationOption->setChecked(false);
        bionicOption->setChecked(false);
        settingsVerticalSpacing = "off";
        settingsVerticalSpacingScope = "all";
        settingsParagraphSpacing = -1;
        settingsParagraphIndent = -1;
        settingsBeautifier = appearance::normalizationPresetSettings("recommended");
        showAdvancedText();
        screenRefreshMode = kobalt::ScreenRefresh::Smart;
        showScreenRefresh();
        resetDefaults->setText(strings::get("reader.defaults_selected"));
        resetDefaults->setEnabled(false);
        settingsPages->setCurrentIndex(int(SettingsPage::Defaults));
    });
    if (settings->debugProgress)
        connect(settings->debugProgress, &QPushButton::clicked, this,
                [this] { showProgressDebug(); });
    connect(settingsPages, &QStackedWidget::currentChanged, this, [this](int) {
        fitSettings();
        refresh();
    });
}

void Reader::buildNotices()
{
    acknowledgments = new kobalt::Acknowledgments(this);
    // The navigation hides its panel edges while Acknowledgments covers the reader.
    acknowledgments->finished = [this] {
        acknowledgments->hide();
        general->raise();
        syncNavigation();
    };
    acknowledgments->hide();
    busyNotice = new kobalt::ui::ModalOverlay(this);
    busyNotice->setDimmed(true);
    busyNotice->card()->message(strings::get("reader.applying_appearance_changes"),
                                "rrUpdatingText");
    // Create the bar before the initial refresh attributes are assigned.
    busyNotice->setDelayedProgress(false, 0, 0);
    resizeTimer.setSingleShot(true);
    resizeTimer.setInterval(100);
    connect(&resizeTimer, &QTimer::timeout, this, [this] {
        if (!lifecycleHidden && !services.suspended() && !exitPending)
            resizeText();
    });
    renderNoticeTimer.setSingleShot(true);
    renderNoticeTimer.setTimerType(Qt::PreciseTimer);
    connect(&renderNoticeTimer, &QTimer::timeout, this, [this] { refreshRenderNotice(); });
    busyNotice->card()->fitToWidth(metrics::notice::cardWidth);
    busyNotice->centerCard();
    brightnessNotice = new kobalt::ui::LightSwipeNotice(this);
    brightnessNotice->setObjectName("rrBrightnessNotice");
    brightnessNotice->setAttribute(Qt::WA_TransparentForMouseEvents);
    brightnessNotice->hide();
    brightnessHide.setObjectName("rrBrightnessHideTimer");
    brightnessHide.setSingleShot(true);
    brightnessHide.setTimerType(Qt::PreciseTimer);
    brightnessHide.setInterval(metrics::lightSwipe::finalPercentageMilliseconds);
    connect(&brightnessHide, &QTimer::timeout, brightnessNotice, &QWidget::hide);
    annotationModeNotice = new ui::AnnotationModeNotice(this);
    annotationModeNotice->setObjectName("rrAnnotationModeNotice");
    annotationModeNotice->hide();
    annotationModeHide.setSingleShot(true);
    annotationModeHide.setInterval(metrics::lightSwipe::hideMilliseconds);
    connect(&annotationModeHide, &QTimer::timeout, annotationModeNotice, &QWidget::hide);
}

void Reader::connectLightGestures()
{
    lightFeedback.setObjectName("rrLightFeedbackTimer");
    lightFeedback.setSingleShot(true);
    lightFeedback.setInterval(metrics::lightSwipe::feedbackMilliseconds);
    connect(&lightFeedback, &QTimer::timeout, this, [this] {
        if (lightFeedbackLevel < 0 || !takesInput() || book->deviceTransition() || annotationMode
            || session->busy() || general->isVisible() || topControls->isVisible()
            || appearanceEdit.panelOpen() || choices->isVisible() || dictionaryPanel->isVisible()
            || searchPanel->isVisible() || linksActive() || lookupPending || syncDialog
            || kosyncDialog || finishDialog || lightPopup->isVisible())
            return;
        brightnessNotice->setLevel(gestureWarmth, lightFeedbackLevel);
        brightnessNotice->show();
        brightnessNotice->raise();
        if (!lightGestureActive)
            brightnessHide.start();
    });
    page->lightStarted = [this](bool warmth) {
        if (annotationMode || !isVisible() || services.suspended() || book->deviceTransition()
            || this->session->busy() || appearanceEdit.preview() || general->isVisible()
            || topControls->isVisible() || choices->isVisible() || dictionaryPanel->isVisible()
            || searchPanel->isVisible() || linksActive() || lookupPending || syncDialog
            || kosyncDialog || finishDialog || applyingText || lightPopup->isVisible()
            || exitPending)
            return false;
        if (warmth) {
            if (!light->initialize() || !light->hasWarmth())
                return false;
            warmthInitial = light->warmth();
            warmthAutomatic = light->automaticWarmth();
        } else {
            if (!light->beginGesture())
                return false;
            brightnessInitial = light->brightness();
        }
        gestureWarmth = warmth;
        lightGestureActive = true;
        lightFeedback.stop();
        lightFeedbackLevel = -1;
        brightnessHide.stop();
        return true;
    };
    page->lightChanged = [this](double moved) {
        lightMovement = moved;
        lightFeedback.stop();
        // Keep only the latest position. Deliver it before queued paints, on
        // the GUI thread required by the native light controller. Feedback
        // waits for a pause instead of requesting an e-ink flush per move.
        if (!lightCommandPending) {
            lightCommandPending = true;
            QCoreApplication::postEvent(this, new QEvent(lightCommandEventType()),
                                        Qt::HighEventPriority);
        }
    };
    page->lightFinished = [this](bool save) {
        // Release can arrive before the queued command. Save the final level;
        // cancellation drops it before restoring the original host state.
        if (save)
            dispatchLightGesture();
        lightCommandPending = false;
        QCoreApplication::removePostedEvents(this, lightCommandEventType());
        lightGestureActive = false;
        if (!save) {
            lightFeedback.stop();
            lightFeedbackLevel = -1;
        }
        if (!gestureWarmth)
            light->finishGesture(save);
        else if (save)
            light->saveWarmth();
        else {
            // A cancelled drag restores the warmth and the automatic setting.
            light->setWarmth(warmthInitial);
            if (warmthAutomatic)
                light->setAutomaticWarmth(true);
        }
        if (save && brightnessNotice->isVisible())
            brightnessHide.start();
        else {
            brightnessHide.stop();
            brightnessNotice->hide();
        }
    };
}

QEvent::Type Reader::lightCommandEventType()
{
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}

void Reader::dispatchLightGesture()
{
    if (!lightCommandPending)
        return;
    lightCommandPending = false;
    const double moved = lightMovement;
    int feedback = -1;
    if (gestureWarmth) {
        // Like Nickel's slider, a manual change turns automatic warmth off.
        const int minimum = light->warmthMinimum(), range = light->warmthMaximum() - minimum;
        if (range <= 0)
            return;
        const double percent = 100.0 * (warmthInitial - minimum) / range
                + kobalt::ui::BrightnessGesture::change(moved);
        const int snapped = qBound(0, qRound(percent / 10.) * 10, 100);
        const int step = qMax(1, light->warmthStep());
        const int level = qBound(minimum, minimum + qRound(snapped * range / (100.0 * step)) * step,
                                 minimum + range);
        const int value = light->setWarmth(snapped == 100 ? minimum + range : level);
        if (value < 0)
            return;
        feedback = qRound(100.0 * (value - minimum) / range);
    } else {
        const int value = light->setGestureLevel(brightnessInitial
                                                 + kobalt::ui::BrightnessGesture::change(moved));
        if (value < 0)
            return;
        feedback = value;
    }
    if (preferences.value("show_light_gesture_feedback").toBool(true)) {
        lightFeedbackLevel = feedback;
        lightFeedback.start();
    }
}

void Reader::connectPageInput()
{
    page->annotationDragStarted = [this](QPoint point, int boundary) {
        return beginAnnotationDrag(point, boundary);
    };
    page->annotationDragMoved = [this](QPoint point) { moveAnnotationDrag(point); };
    page->annotationDragFinished = [this](bool cancelled) { finishAnnotationDrag(cancelled); };
    page->annotationRequested = [this] {
        if (!features::enabled(preferences, features::Annotations) || annotationMode || !isEnabled()
            || !isVisible() || lifecycleHidden || closing || exitPending || exportRunning
            || services.suspended() || book->deviceTransition() || session->busy()
            || appearanceEdit.panelOpen() || appearanceEdit.applying() || general->isVisible()
            || choices->isVisible() || topControls->isVisible() || revealingLinks
            || dictionaryPanel->isVisible() || searchPanel->isVisible() || linksActive()
            || lookupPending || acknowledgments->isVisible() || bookmarkList->isVisible()
            || lightPopup->isVisible() || chapterGraph->isVisible() || !highlights->writable()) {
            return;
        }
        const auto *modal = QApplication::activeModalWidget();
        if (modal && modal != this) {
            return;
        }
        for (auto *widget : popups()) {
            auto *overlay = dynamic_cast<ui::ModalOverlay *>(widget);
            if (overlay && overlay->isVisible() && overlay->inputBarrier()) {
                return;
            }
        }
        enterAnnotations();
    };
    page->swiped = [this](int direction) {
        if (!annotationMode
            && !pageNavigation::swipes(pageNavigation::mode(preferences, services.pageButtons()))) {
            return;
        }
        if (dictionaryPanel->isVisible()) {
            dismissLookup();
            clearAnnotationDraft();
        }
        if (revealingLinks || searchPanel->isVisible() || linksActive() || lookupPending
            || appearanceEdit.preview() || general->isVisible() || choices->isVisible()
            || topControls->isVisible())
            return;
        turnPage(session->state().value("page_progression_rtl").toBool() ? -direction : direction,
                 page->gestureTimestamp());
    };
    page->tapped = [this](QPoint point) {
        const QPoint bodyPoint = point - QPoint(0, page->topHeight());
        if (!topControls->isVisible() && !annotationDraft->range().start.isEmpty()
            && page->annotationContains(bodyPoint)) {
            annotationTap(bodyPoint);
            return;
        }
        if (dictionaryPanel->isVisible()) {
            dismissDictionary();
            return;
        }
        if (!annotationDraft->range().start.isEmpty() && !topControls->isVisible())
            dismissLookup(true);
        const qreal x = qreal(point.x()) / qMax(1, page->width());
        if (searchPanel->isVisible() || linksActive() || lookupPending) {
            dismissLookup();
            updateLinkReturn();
            return;
        }
        if (appearanceEdit.applying())
            return;
        if (revealingLinks) {
            if (!openLink(point - QPoint(0, page->topHeight())))
                leaveRevealedLinks();
            return;
        }
        if (choosingTypography && choices->isVisible()) {
            choices->hide();
            refresh();
            return;
        }
        const bool devicePreview = services.devicePreview && services.devicePreview();
        const qreal y = qreal(point.y()) / qMax(1, page->height());
        const auto layout = pageNavigation::tapLayout(preferences);
        const auto action = tapZones::action(layout, x, y);
        const bool menuTap = action == tapZones::Action::Menu;
        if (topControls->isVisible() || choices->isVisible()) {
            // Panels can cover the middle of a device preview. Any tap on
            // the exposed page closes the panel and returns to reading.
            if (!devicePreview || menuTap || appearanceEdit.panelOpen() || chapterGraph->isVisible()
                || lightPopup->isVisible()) {
                flashAfterMenu = true;
                dismissControls();
            }
            return;
        }
        // Enabled links win when their text overlaps the bookmark shortcut.
        if (!annotationMode && point.y() >= page->topHeight()
            && point.y() < page->height() - page->bottomHeight() && openLink(bodyPoint, true))
            return;
        if (features::enabled(preferences, features::Bookmarks) && bookmarks->available()
            && page->bookmarkCorner().contains(point)) {
            toggleBookmark();
            return;
        }
        // A tap on the header strip opens the menus, like a tap on the title.
        if (!devicePreview && point.y() < page->topHeight()) {
            showControls();
            return;
        }
        if (menuTap) {
            showControls();
            return;
        }
        if (!pageNavigation::taps(pageNavigation::mode(preferences, services.pageButtons()))) {
            return;
        }
        const int direction = action == tapZones::Action::Previous ? -1 : 1;
        const bool reverse = layout == tapZones::Layout::LeftRight
                && session->state().value("page_progression_rtl").toBool();
        turnPage(reverse ? -direction : direction, page->gestureTimestamp());
    };
}

void Reader::showFontPicker()
{
    if (!appearanceEdit.panelOpen() || panels->currentIndex() != 0)
        return;
    const auto *settings =
            appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : session;
    diagnostic("font-picker-open", {}, false);
    QVector<Choice> items{ { strings::get("reader.publisher_fonts"), QString(), "rrFontChoice0",
                             strings::get("reader.recommended_fonts") } };
    pickerNativeFamilies = services.fontFamilies();
    const auto state = settings->state();
    const auto supplemental = supplementalFontFamily(state.value("language").toString());
    const auto requestedDefault =
            preferences.value("default_font_face").toString(kobalt::defaultFontFace);
    auto fonts = settings->fontCatalog();
    // The publisher row's sample uses Libron.
    for (const auto &font : fonts)
        if (font.family == QLatin1String("Libron"))
            items[0].fontFiles = font.files;
    for (int row = fonts.size() - 1; row >= 0; --row) {
        const auto &font = fonts[row];
        if ((font.origin == FontOrigin::BuiltIn
             && !pickerNativeFamilies.contains(font.family, Qt::CaseInsensitive))
            || (font.origin == FontOrigin::Interface && font.family != supplemental
                && font.family != requestedDefault
                && font.family != state.value("font_face").toString()))
            fonts.removeAt(row);
    }
    for (const auto &font :
         orderedReadingFonts(std::move(fonts), { settings->fontFace(), requestedDefault })) {
        QString section;
        switch (font.origin) {
        case kobalt::FontOrigin::Recommended:
            section = strings::get("reader.recommended_fonts");
            break;
        case kobalt::FontOrigin::BuiltIn:
            section = strings::get("reader.built_in_fonts");
            break;
        case kobalt::FontOrigin::Sideloaded:
            section = strings::get("reader.sideloaded_fonts");
            break;
        case kobalt::FontOrigin::Interface:
            section = strings::get("reader.supplemental_fonts");
            break;
        }
        items.append({ fontDisplayName(font.family), font.family,
                       QString("rrFontChoice%1").arg(items.size()), section });
        items.last().fontFiles = font.files;
    }
    QString defaultFace = preferences.value("default_font_face").toString(kobalt::defaultFontFace);
    int defaultIndex = -1;
    for (int i = 0; i < items.size(); ++i)
        if (items[i].value == defaultFace)
            defaultIndex = i;
    if (defaultIndex < 0) {
        defaultFace = kobalt::defaultFontFace;
        defaultIndex = 1;
    }
    const int embedded = state.value("embedded_font_count").toInt();
    auto serifFallback = state.value("serif_font_face").toString();
    if (serifFallback.isEmpty())
        serifFallback = kobalt::defaultFontFace;
    items[0].detail = embedded > 0
            ? (embedded == 1 ? strings::get("reader.one_embedded_font")
                             : strings::get("reader.embedded_fonts").arg(embedded))
            : strings::get("reader.no_embedded_fonts") + '\n'
                    + strings::get("reader.uses_default_font").arg(fontDisplayName(serifFallback));
    if (defaultIndex < items.size()) {
        auto &detail = items[defaultIndex].detail;
        if (!detail.isEmpty())
            detail += '\n';
        detail += strings::get("reader.default_font");
    }
    Choice slot{ QStringLiteral("Aa"), defaultFace, "rrFontDefaultSlot",
                 strings::get("reader.recommended_fonts") };
    slot.detail = strings::get("reader.set_default_font");
    slot.placeholder = true;
    // The Publisher fonts row samples Libron to fit its two-line caption.
    // Without this registration the sample can measure a fallback face.
    registerPickerFonts(QString::fromLatin1(kobalt::defaultFontFace));
    if (!services.phoneLayout) {
        // Registration reads only metadata. The slot's Aa loads its face when drawn.
        registerPickerFonts(defaultFace);
        items.insert(qMin(2, items.size()), slot);
    }
    choices->defaultRequested = [this](const QString &face, bool instructions) {
        showFontDefaultDialog(face, instructions);
    };
    choices->selected = [this](const QString &face) {
        const auto *current = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        if (!appearanceEdit.queued() && face == current->fontFace())
            return;
        diagnostic("font-preview-request", {}, false);
        mutateAppearance([this, face](ReaderSession &preview) {
            if (preview.style(preview.fontSize(), preview.spacing(), face, preview.margin()))
                choices->selectValue(face);
        });
    };
    choosingTypography = true;
    const int bottom = navigation ? navigation->contentBottom(size()) : height();
    const int pickerHeight = qMin(choices->fontPanelHeight(),
                                  bottom - (navigation ? navigation->toolbarHeight() : 0));
    choices->setDropdownGeometry(QRect(0, bottom - pickerHeight, width(), pickerHeight));
    choices->showChoices(strings::get("reader.font"), std::move(items), settings->fontFace(),
                         ChoiceScreen::Mode::Fonts);
    refresh();
}

} // namespace kobalt::reader
