#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/sliders.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "assets/bundled_fonts.h"
#include "reader_p.h"
#include "dictionaries/manager.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "interfaces/reader/ui/style/styles.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "platform/api/kobo_refresh.h"
#include "support/diagnostics.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "storage/reader_storage.h"
#include "interfaces/reader/settings/settings_panel.h"
#include "build_info.h"
#include "reading/appearance_settings.h"
#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QScreen>
#include <QVBoxLayout>
#include <utility>

namespace kobalt::reader {
namespace strings = kobalt::strings;
using namespace kobalt::storage;
namespace styles = kobalt::ui::styles;
namespace m = kobalt::ui::metrics;
using kobalt::ui::readerStyle;
using kobalt::ui::makeButton;
using kobalt::ui::ControlIcon;
using kobalt::ui::buttonIcon;
using kobalt::ui::SettingSlider;
using kobalt::platform::textRefresh;
using kobalt::Choice;
using kobalt::ChoiceScreen;

namespace {
kobalt::ui::Button *iconButton(QWidget *parent, QBoxLayout *layout, const char *name, const QString &label,
                               ControlIcon icon, std::function<void()> action) {
    auto *button = static_cast<kobalt::ui::Button *>(makeButton(parent, layout, name, {}, std::move(action)));
    button->setAccessibleName(label);
    button->setToolTip(label);
    kobalt::ui::iconAction(button, icon, label);
    button->setFixedWidth(m::control::iconButtonWidth);
    return button;
}
}

ChapterHeader::ChapterHeader(QWidget *parent) : QPushButton(parent) {
    setObjectName("rrReadingMenu");
    readerStyle(this, "min-height:0;padding:0;");
    setUpdatesEnabled(false);
    // The page handles taps on the strip, including the bookmark corner.
    // On firmware 4 a touch on this button never became a click.
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

Reader::Reader(std::unique_ptr<kobalt::ReadingBook> openedBook, kobalt::ReaderOrientation *orientation,
               kobalt::ReaderHostServices hostServices)
    : ReaderWindow(nullptr, Qt::Window | Qt::FramelessWindowHint), services(std::move(hostServices)),
      book(std::move(openedBook)), model(book->session()), orientation(orientation) {
    Q_ASSERT(services.complete());
    setObjectName("rrReader");
    setAttribute(Qt::WA_DeleteOnClose);
    presentation = services.presentation(this);
    Q_ASSERT(presentation);
    dictionary = services.dictionary();
    if (services.downloadableDictionaries)
        dictionary = std::make_unique<kobalt::DictionaryManager>(book->paths().directory + "/dictionaries",
                                                                 std::move(dictionary));
    if (!book->dictionaryChoice().isEmpty()) {
        QString error;
        dictionary->select(book->identity(), book->dictionaryChoice(), &error);
    }
    kobalt::platform::setReaderRefreshWindow(this);
    qApp->installEventFilter(this);
    setWindowModality(Qt::ApplicationModal);
    // A damaged settings file still falls back to the defaults, as a
    // missing one does, but the diagnostics say which it was.
    const auto savedSettings = kobalt::storage::readJsonFile(book->paths().preferences());
    if (savedSettings.status != kobalt::storage::ReadStatus::Ok &&
        savedSettings.status != kobalt::storage::ReadStatus::Missing)
        kobalt::diagnostic("settings-unreadable", {{"status", kobalt::storage::readStatusName(savedSettings.status)}},
                           false);
    preferences = appearance::readerPreferences(savedSettings.object, book->layoutPreferences());
    bookmarks = std::make_unique<kobalt::BookmarkSession>(
        *model,
        [this] {
            return layoutKey();
        },
        book->paths().annotations(), book->identity(), book->fingerprint(), model->title());
    auto *nativeSaveTimer = new QTimer(this);
    nativeSaveTimer->setObjectName("rrNativeProgressTimer");
    connect(nativeSaveTimer, &QTimer::timeout, this, [this] {
        if (!closing && !lifecycleHidden && !exportRunning && !model->busy() && !progressBlocked()) {
            saveNativeProgress();
        }
    });
    nativeSaveTimer->start(30000);
    bookmarks->changed = [this] {
        refresh();
    };
    bookmarks->added = [this] {
        page->bookmarked = true;
        page->refreshContent();
        saveSoon();
    };
    bookmarks->failed = [this] {
        kobalt::qt::later(0, this, [this] {
            showBookmarkError();
        });
    };
    // Choose the text weight before the first font is used.
    kobalt::ui::setInterfaceWeight(kobalt::ui::interfaceWeightSteps(preferences));
    // Set only our window; native Nickel controls keep their own font.
    setFont(kobalt::ui::interfaceFont());
    readerStyle(this, styles::reader());
    buildReadingPage();
    startClockTimer();
    buildToolbar();
    buildBottomControls();
    const auto appearanceLayout = buildMenu();
    buildOverlays();
    buildAppearance(appearanceLayout);
    buildSettings();
    buildNotices();
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
    connect(header, &QPushButton::clicked, this, [this] {
        if (topControls->isVisible()) {
            flashAfterMenu = true;
            dismissControls();
        } else
            showControls();
    });
    // Keep the displayed page untouched while a turn renders. Refreshing
    // at request time sends the old page to the panel a second time.
    this->model->updated = [this] {
        refresh();
    };
    this->model->renderProgressChanged = [this] {
        refreshRenderNotice();
    };
    this->model->turnBoundary = [this](int direction) {
        turnPage(direction);
    };
    connect(QGuiApplication::primaryScreen(), &QScreen::geometryChanged, this, [this](const QRect &geometry) {
        if (isVisible() && !closing)
            setGeometry(geometry);
    });
    resize(QGuiApplication::primaryScreen()->size());
    connectPageInput();
    // Android can suspend the app without hiding the embedded reader widget.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this] {
        observeReading();
    });
}

void Reader::buildReadingPage() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    page = new kobalt::ReadingPage(this, services.cancelAnimation);
    layout->addWidget(page, 1);
    page->setFont(kobalt::ui::chromeFont());
    readerStyle(page, "font-family:'" + kobalt::ui::chromeFont().family() + "';");
    dictionaryPanel = new kobalt::DictionaryPanel(this);
    dictionaryPanel->setLanguage(model->state().value("language").toString());
    dictionaryPanel->dismissed = [this] {
        dismissLookup();
        updateLinkReturn();
    };
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
    linkPanel->navigate = [this](const QString &target) {
        goToPassage(target);
    };
    linkReturn = new kobalt::ui::BackButton(strings::get("reader.return_to_passage"), this);
    linkReturn->setObjectName("rrLinkReturn");
    readerStyle(linkReturn, styles::outlined() + "QPushButton{min-height:0;padding:0;}");
    linkReturn->setFixedSize(m::links::returnWidth, m::links::returnHeight);
    linkReturn->hide();
    connect(linkReturn, &QPushButton::clicked, this, [this] {
        if (this->model->busy() || linkHistory.isEmpty() || lifecycleHidden || closing || services.suspended()) {
            return;
        }
        dismissLookup();
        dismissControls();
        returningFromLink = true;
        this->model->goTo(linkHistory.last().position);
        refresh();
    });
    searchPanel = new kobalt::SearchPanel(this, services.keyboard);
    searchPanel->requested = [this](const QString &query) {
        const auto epoch = ++searchEpoch;
        QPointer<Reader> guard(this);
        this->model->search(query, [guard, epoch](kobalt::SearchBatch batch) {
            if (!guard || guard->searchEpoch != epoch || guard->lifecycleHidden)
                return;
            guard->searchPanel->append(batch, guard->model->pages());
        });
    };
    searchPanel->dismissed = [this] {
        closeSearch();
    };
    searchPanel->returnToStart = [this] {
        // Returning ends the search trip; the next search starts from here.
        const auto start = std::exchange(searchOriginal, QString());
        closeSearch();
        this->model->goTo(start);
    };
    searchPanel->selected = [this](kobalt::SearchHit hit) {
        closeSearch();
        pendingSearchHit = hit;
        this->model->goTo(hit.start);
    };
    page->held = [this](QPoint point) {
        lookupWord(point);
    };
    header = new ChapterHeader(page);
    page->showHeader = preferences.value("show_header").toBool(true);
    page->showFooter = preferences.value("show_footer").toBool(true);
    page->headerSize = m::reading::textSize(preferences.value("header_text_size").toString());
    page->footerSize = m::reading::textSize(preferences.value("footer_text_size").toString());
    page->headerTextPixels = m::reading::textPixels(page->headerSize, false);
    page->footerTextPixels = m::reading::textPixels(page->footerSize, true);
    page->showClock = preferences.value("show_clock").toBool(true);
    page->hideDeviceStatus = hideNativeStatus();
    page->showProgress = preferences.value("show_progress_bar").toBool(true);
    page->clockText = services.clock();
}

void Reader::startClockTimer() {
    // The clock and menu status change once a minute. Wake just after each
    // change instead of every second.
    clockTimer = new QTimer(this);
    clockTimer->setSingleShot(true);
    connect(clockTimer, &QTimer::timeout, this, [this] {
        startClock();
        if (!isVisible() || services.suspended())
            return;
        if (topControls->isVisible()) {
            updateMenuStatus();
            return;
        }
        if (!page->showClock || general->isVisible() || dictionaryPanel->isVisible() || searchPanel->isVisible() ||
            linksActive())
            return;
        const auto time = services.clock();
        if (time == page->clockText)
            return;
        page->clockText = time;
        page->cleanRefresh(false);
        page->refreshContent(true);
    });
    startClock();
}

void Reader::buildToolbar() {
    topControls = new kobalt::ui::Panel(this, Qt::BottomEdge);
    topControls->setObjectName("rrControlsTop");
    topControls->setFrameStyle(QFrame::Box);
    auto *toolbar = new QVBoxLayout(topControls);
    toolbarComposition.panel = topControls;
    toolbarComposition.rows = toolbar;
    toolbar->setContentsMargins(m::panel::toolbarMargins);
    toolbar->setSpacing(m::panel::statusGap);
    auto *status = new QGridLayout;
    status->setContentsMargins(m::panel::statusMargins);
    toolbar->addLayout(status);
    menuClock = new QLabel(topControls);
    menuClock->setObjectName("rrMenuClock");
    menuClock->setFixedHeight(m::panel::statusHeight);
    menuClock->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    readerStyle(menuClock, styles::font(m::panel::statusText));
    status->addWidget(menuClock, 0, 0);
    menuSession = new QLabel(topControls);
    menuSession->setObjectName("rrMenuSession");
    menuSession->setFixedHeight(m::panel::statusHeight);
    menuSession->setAlignment(Qt::AlignCenter);
    readerStyle(menuSession, styles::brand(m::panel::sessionText));
    status->addWidget(menuSession, 0, 1);
    status->setColumnStretch(0, 1);
    status->setColumnStretch(2, 1);
    menuBattery = new kobalt::ui::BatteryIndicator(topControls);
    menuBattery->setObjectName("rrMenuBattery");
    readerStyle(menuBattery, styles::font(m::panel::batteryText));
    status->addWidget(menuBattery, 0, 2, Qt::AlignRight);
    menuBluetooth = new kobalt::ui::BluetoothIndicator(topControls);
    menuBluetooth->setObjectName("rrMenuBluetooth");
    toolbarComposition.bluetooth = menuBluetooth;
    auto *top = new QHBoxLayout;
    toolbar->addLayout(top);
    top->setSpacing(m::panel::toolbarGap);
    updateMenuStatus();
    auto *back = new kobalt::ui::BackButton(strings::get("reader.exit"), topControls);
    back->setContentsMargins(0, 0, m::panel::exitLabelExtraRight, 0);
    toolbarComposition.exitAction = back;
    back->setObjectName("rrReturn");
    top->addWidget(back);
    connect(back, &QPushButton::clicked, this, [this] {
        // The chapter list's close cell sits where Exit Kobalt is. A second
        // tap that lands after the list closed must not leave the reader.
        if (exitGuarded)
            return;
        close();
    });
    readerStyle(back, "QPushButton#rrReturn{" + styles::brand(m::about::exitText) + "}");
    back->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    top->addStretch(1);
    auto *lightButton = iconButton(topControls, top, "rrLight", strings::get("reader.brightness_and_warmth"),
                                   ControlIcon::Light, [this] {
                                       toggleLight();
                                   });
    lightButton->setCheckable(true);
    lightButton->setEnabled(services.lightAvailable());
    light = services.light(lightButton);
    Q_ASSERT(light);
    warmthSupported = services.lightAvailable() && light->initialize() && light->hasWarmth();
    applyEdgeLight();
    lightPopup = new kobalt::LightPanel(*light, this);
    lightPopup->setEdgeGesturesEnabled(page->brightnessEdge);
    lightPopup->changed = [lightButton, this] {
        lightButton->setChecked(lightPopup->isVisible());
        if (navigation && lightPopup->isVisible())
            navigation->stackPanel(lightPopup);
        syncNavigation();
    };
    lightPopup->placement = [this](int h) {
        return panelBounds(h);
    };
    auto *typeButton = iconButton(topControls, top, "rrTypography", strings::get("reader.typography"),
                                  ControlIcon::Typography, [this] {
                                      selectPanel(0);
                                  });
    typeButton->setFixedWidth(m::control::typographyButtonWidth);
    typeButton->setAccessibleName(strings::get("reader.typography"));
    typeButton->setToolTip(strings::get("reader.typography"));
    readerStyle(typeButton, styles::font(m::text::typography));
    typeButton->setCheckable(true);
    iconButton(topControls, top, "rrLayout", strings::get("reader.page_layout"), ControlIcon::Layout, [this] {
        selectPanel(1);
    })->setCheckable(true);
    iconButton(topControls, top, "rrGraph", strings::get("reader.chapter_lengths"), ControlIcon::Graph, [this] {
        toggleGraph();
    })->setCheckable(true);
    toolbarComposition.bookmarks =
        iconButton(topControls, top, "rrBookmarks", strings::get("reader.bookmarks"), ControlIcon::Bookmark, [this] {
            openBookmarks();
        });
    toolbarComposition.bookmarks->setEnabled(bookmarks->available());
    toolbarComposition.formattingWarning =
        iconButton(topControls, top, "rrFormattingWarning", strings::get("reader.formatting_warning"),
                   ControlIcon::FileWarning, [this] {
                       showFormattingWarning();
                   });
    toolbarComposition.formattingWarning->setVisible(model->formattingWarning());

    toolbarComposition.settings =
        iconButton(topControls, top, "rrSettings", strings::get("reader.reader_settings"), ControlIcon::More, [this] {
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
    for (auto *button : topControls->findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (button == back)
            continue;
        if (button->objectName() != "rrSettings" && button->objectName() != "rrBookmarks" &&
            button->objectName() != "rrFormattingWarning")
            topActions.append(button);
        if (auto *action = dynamic_cast<kobalt::ui::Button *>(button))
            action->setPaintMode(kobalt::ui::ButtonPaintMode::ToolbarSquare);
        button->setFixedSize(m::control::toolbarButtonSize, m::control::toolbarButtonSize);
        readerStyle(button, "QPushButton{border:none;padding:0;min-height:0;}" +
                                (button == typeButton ? styles::font(m::text::typography) : QString()));
    }
}

void Reader::buildBottomControls() {
    bottomControls = new kobalt::ui::Panel(this, Qt::TopEdge);
    bottomControls->setObjectName("rrControlsBottom");
    bottomControls->setFrameStyle(QFrame::Box);
    auto *bottom = new QVBoxLayout(bottomControls);
    bottom->setContentsMargins(m::panel::footerMargins);
    bottom->setSpacing(m::panel::footerGap);
    bottom->addWidget(linkReturn, 0, Qt::AlignLeft);
    progressLabel = new QLabel(bottomControls);
    progressLabel->setObjectName("rrControlsProgress");
    readerStyle(progressLabel, styles::font(m::text::secondary));
    progressLabel->setAlignment(Qt::AlignCenter);
    bottom->addWidget(progressLabel);
    auto *pageNavigationRow = new QHBoxLayout;
    pageNavigationRow->setSpacing(m::panel::footerGap);
    bottom->addLayout(pageNavigationRow);
    previousPage = iconButton(bottomControls, pageNavigationRow, "rrPreviousPage", strings::get("common.previous_page"),
                              ControlIcon::Previous, [this] {
                                  turnPage(-1);
                              });
    bookSlider = new SettingSlider(bottomControls, 0, 0);
    bookSlider->setObjectName("rrBookPosition");
    bookSlider->setAccessibleName(strings::get("reader.book_position"));
    pageNavigationRow->addWidget(bookSlider, 1);
    connect(bookSlider, &QSlider::valueChanged, this, [this](int value) {
        progressLabel->setText(strings::get("reader.menu_page_of").arg(value + 1).arg(this->model->pages()));
    });
    bookSlider->committed = [this](int value) {
        if (!this->model->busy())
            this->model->go(value);
        refresh();
    };
    nextPage = iconButton(bottomControls, pageNavigationRow, "rrNextPage", strings::get("common.next_page"),
                          ControlIcon::Next, [this] {
                              turnPage(1);
                          });
    auto *footer = new QHBoxLayout;
    bottom->addLayout(footer);
    auto *chapters = new kobalt::ui::ElidingButton(strings::get("reader.contents"), bottomControls);
    chapters->setObjectName("rrChapters");
    footer->addWidget(chapters, 1);
    iconButton(bottomControls, footer, "rrSearchOpen", strings::get("reader.search_book"), ControlIcon::Search, [this] {
        openSearch();
    });
    connect(chapters, &QPushButton::clicked, this, [this] {
        showChapters();
    });
    chapterButton = chapters;
    chapterButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    readerStyle(chapters, "text-align:left;");
    buttonIcon(chapters, ControlIcon::Chapters);
    chapters->setIconSize(QSize(m::control::chapterIconSize, m::control::chapterIconSize));
}

kobalt::reader::AppearanceLayout Reader::buildMenu() {
    menu = new AppearanceSheet(this);
    panels = menu->pages;
    previewLabel = menu->message;
    return menu->controls();
}

void Reader::buildOverlays() {
    // Link discovery takes its styles in syncLinkDiscovery: they follow the
    // place it occupies and the reading strip's text size.
    linkIndicator = new kobalt::ui::Button({}, this);
    linkIndicator->setObjectName("rrLinkIndicator");
    linkIndicator->hide();
    linkDiscovery = new kobalt::ui::Panel(this);
    linkDiscovery->setObjectName("rrLinkDiscovery");
    linkDiscovery->setEdgeWidth(m::chrome::heavyRule);
    auto *linkRow = new QHBoxLayout(linkDiscovery);
    linkRow->setSpacing(m::links::hintGap);
    linkDiscoveryIcon = new QLabel(linkDiscovery);
    // The link glyph has more space around it than the undo icon in the return
    // row: a larger drawing in the same column matches its size.
    linkDiscoveryIcon->setFixedSize(m::chrome::cellIcon, m::control::iconSize);
    linkDiscoveryIcon->setAlignment(Qt::AlignCenter);
    linkRow->addWidget(linkDiscoveryIcon);
    linkDiscoveryHint = new QLabel(strings::get("reader.long_press_link"), linkDiscovery);
    linkDiscoveryHint->setObjectName("rrLinkDiscoveryHint");
    linkRow->addWidget(linkDiscoveryHint, 1);
    linkDiscoveryToggle = makeButton(linkDiscovery, linkRow, "rrRevealLinks", {}, [this] {
        showRevealedLinks();
    });
    linkDiscoveryExit = new QLabel(strings::get("reader.tap_elsewhere_to_exit").toUpper(), linkDiscovery);
    linkDiscoveryExit->setObjectName("rrLinkDiscoveryExit");
    linkRow->addWidget(linkDiscoveryExit);
    linkDiscovery->hide();
    connect(linkIndicator, &QPushButton::clicked, this, [this] {
        showControls();
        showRevealedLinks();
    });
    chapterGraph = new kobalt::ChapterGraph(this);
    chapterGraph->changed = [this] {
        save();
    };
    chapterGraph->setDense(preferences.value("dense_stats_chart").toBool(false));
    pinStats = preferences.value("pin_stats").toBool(false);
    choices = new ChoiceScreen(this);
    choices->hide();
    bookmarkList = new kobalt::BookmarkList(this);
    bookmarkList->selected = [this](const QString &position) {
        goToPassage(position);
    };
    bookmarkList->back = [this] {
        bookmarkList->hide();
        guardExit();
        showControls();
        refresh();
    };
    bookmarkList->removeRequested = [this](const QString &position) {
        askRemoveBookmark(position);
    };
    choices->changed = [this] {
        if (choosingTypography && choices->isVisible())
            syncNavigation();
        save();
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

void Reader::buildAppearance(const kobalt::reader::AppearanceLayout &layout) {
    auto *typography = layout.typography, *pageLayout = layout.pageLayout;
    auto *typeRows = layout.typeRows, *pageRows = layout.pageRows;
    kobalt::reader::AppearancePanelInputs appearanceInputs;
    const auto chromeSize = [this](bool footer) {
        const QString key = footer ? "footer_text_size" : "header_text_size";
        const QString visibility = footer ? "show_footer" : "show_header";
        return preferences.value(visibility).toBool(true) ? m::reading::textSize(preferences.value(key).toString())
                                                          : QString("off");
    };
    appearanceInputs.headerSize = chromeSize(false);
    appearanceInputs.footerSize = chromeSize(true);
    appearanceInputs.sideButtons = services.pageButtons();
    appearanceInputs.reverseLandscapeButtons = services.reverseLandscapeButtons();
    appearanceInputs.automaticRotationAvailable = orientation->automaticAvailable();
    appearanceInputs.showClock = page->showClock;
    appearanceInputs.systemRotation = services.systemRotation;
    kobalt::reader::AppearancePanel appearance({typography, pageLayout, typeRows, pageRows}, appearanceInputs);
    fontButton = appearance.fontButton;
    sizeSlider = appearance.sizeSlider;
    opticalSizeSlider = appearance.opticalSizeSlider;
    weightSlider = appearance.weightSlider;
    variableWeightSlider = appearance.variableWeightSlider;
    spacingSlider = appearance.spacingSlider;
    marginSlider = appearance.marginSlider;
    verticalMarginSlider = appearance.verticalMarginSlider;
    alignmentButtons = appearance.alignmentButtons;
    hyphenButtons = appearance.hyphenButtons;
    rotationButtons = appearance.rotationButtons;
    autoRotateOption = appearance.autoRotateOption;
    clockOption = appearance.clockOption;
    clockHideOption = appearance.clockHideOption;
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
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        if (appearanceEdit.queued() || pixels != settings->fontSize())
            mutateAppearance([pixels](ReaderSession &preview) {
                preview.style(pixels, preview.spacing());
            });
    };
    connect(sizeSlider, &QSlider::valueChanged, this, [this](int value) {
        findChild<QLabel *>("rrTextSizeValue")
            ->setText(strings::get("reader.text_size_value").arg(QString::number(value / 2.0, 'f', value % 2)));
    });
    opticalSizeSlider->committed = [this](int value) {
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        if (appearanceEdit.queued() || value != settings->opticalSize())
            mutateAppearance([value](ReaderSession &preview) {
                preview.setOpticalSize(value);
            });
    };
    connect(opticalSizeSlider, &QSlider::valueChanged, this, [this](int value) {
        findChild<QLabel *>("rrOpticalSizeValue")
            ->setText(strings::get("reader.text_size_value").arg(QString::number(value)));
    });
    weightSlider->committed = [this](int value) {
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        if (appearanceEdit.queued() || value != settings->thickness())
            mutateAppearance([value](ReaderSession &preview) {
                preview.setThickness(value);
            });
    };
    variableWeightSlider->committed = [this](int value) {
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        if (appearanceEdit.queued() || value != settings->weight())
            mutateAppearance([value](ReaderSession &preview) {
                preview.setWeight(value);
            });
    };
    connect(variableWeightSlider, &QSlider::valueChanged, this, [this](int value) {
        findChild<QLabel *>("rrVariableFontWeightValue")->setText(QString::number(value));
    });
    spacingSlider->committed = [this](int value) {
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        if (appearanceEdit.queued() || value != settings->spacing())
            mutateAppearance([value](ReaderSession &preview) {
                preview.style(preview.fontSize(), value);
            });
    };
    marginSlider->committed = [this](int value) {
        const int pixels = kobalt::readingPixels(value);
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        if (appearanceEdit.queued() || pixels != settings->margin())
            mutateAppearance([pixels](ReaderSession &preview) {
                preview.style(preview.fontSize(), preview.spacing(), preview.fontFace(), pixels);
            });
    };
    verticalMarginSlider->committed = [this](int value) {
        const int pixels = kobalt::readingPixels(value);
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        if (appearanceEdit.queued() || pixels != settings->topMargin() || pixels != settings->bottomMargin())
            mutateAppearance([pixels](ReaderSession &preview) {
                preview.setVerticalMargins(pixels);
            });
    };
    for (auto *button : alignmentButtons) {
        const QString value = button->property("alignment").toString();
        connect(button, &QPushButton::clicked, this, [this, value] {
            const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
            if (appearanceEdit.queued() || value != settings->alignment())
                mutateAppearance([value](ReaderSession &preview) {
                    preview.style(preview.fontSize(), preview.spacing(), preview.fontFace(), preview.margin(), value);
                });
        });
    }
    for (auto *button : hyphenButtons) {
        const QString value = button->property("hyphenation").toString();
        connect(button, &QPushButton::clicked, this, [this, value] {
            const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
            if (appearanceEdit.queued() || value != settings->hyphenation())
                mutateAppearance([value](ReaderSession &preview) {
                    preview.setHyphenation(value);
                });
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
                               {{"target_orientation", value},
                                {"current_orientation", int(orientation->manual())},
                                {"width", width()},
                                {"height", height()},
                                {"main_busy", model->busy()},
                                {"preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy()},
                                {"updates_enabled", updatesEnabled()},
                                {"resize_pending", resizePending}},
                               false);
            this->orientation->configure(kobalt::qt::object({{"orientation", value}, {"auto_rotate", false}}));
            kobalt::diagnostic("reader-orientation-dispatched",
                               {{"target_orientation", value},
                                {"width", width()},
                                {"height", height()},
                                {"main_busy", model->busy()},
                                {"preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy()},
                                {"updates_enabled", updatesEnabled()},
                                {"resize_pending", resizePending}},
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
            appearanceEdit.setError(this->orientation->setAutomatic(true)
                                        ? QString()
                                        : strings::get("reader.the_orientation_sensor_is_unavailable"));
            refresh();
        });
    headerSizeButtons = appearance.headerSizeButtons;
    footerSizeButtons = appearance.footerSizeButtons;
    auto connectChromeSize = [this](bool footer) {
        const QString key = footer ? "footer_text_size" : "header_text_size";
        const QString visibility = footer ? "show_footer" : "show_header";
        for (auto *button : footer ? footerSizeButtons : headerSizeButtons) {
            connect(button, &QPushButton::clicked, this, [this, button, key, footer, visibility] {
                if (model->temporaryTypography()) {
                    syncChromeSizeButtons();
                    return;
                }
                const QString value = button->property("text_size").toString();
                auto next = preferences;
                next[visibility] = value != "off";
                if (value != "off") {
                    next[key] = value;
                }
                const bool changed = next != preferences;
                if (!writePreferences(next)) {
                    syncChromeSizeButtons();
                    appearanceEdit.setError(strings::get("reader.could_not_save_reader_settings"));
                    refresh();
                    return;
                }
                preferences = next;
                if (footer) {
                    page->showFooter = value != "off";
                    page->footerSize = m::reading::textSize(next.value(key).toString());
                    page->footerTextPixels = m::reading::textPixels(page->footerSize, true);
                } else {
                    page->showHeader = value != "off";
                    page->headerSize = m::reading::textSize(next.value(key).toString());
                    page->headerTextPixels = m::reading::textPixels(page->headerSize, false);
                }
                const QSize viewport(width(), height() - page->topHeight() - page->bottomHeight());
                if (changed) {
                    mutateAppearance([viewport](ReaderSession &preview) {
                        preview.resize(viewport);
                    });
                }
                header->setGeometry(0, 0, width(), page->topHeight());
                header->setVisible(page->topHeight() > 0);
                syncChromeSizeButtons();
                appearanceEdit.clearError();
                refresh();
            });
        }
    };
    connectChromeSize(false);
    connectChromeSize(true);
    typeRows->setSpacing(m::control::rowGap);
    pageRows->setSpacing(m::control::rowGap);
    typeRows->addStretch();
    pageRows->addStretch();
}

void Reader::buildSettings() {
    kobalt::reader::SettingsPanelInputs settingsInputs;
    const auto release = QStringLiteral(KOBALT_VERSION);
    settingsInputs.release = release.mid(release.startsWith('v') ? 1 : 0);
    settingsInputs.commit = QString::fromLatin1(kobaltCommit) + (KOBALT_COMMIT_MODIFIED ? "*" : "");
    settingsInputs.commitVisible = kobaltCommit[0] != '\0';
    settingsInputs.qtMajor = QT_VERSION >> 16;
    settingsInputs.desktopUi = services.desktopUi;
    settingsInputs.downloadableDictionaries = services.downloadableDictionaries;
    settingsInputs.animationsSupported = services.animationsAvailable();
    settingsInputs.lightSupported = services.lightAvailable();
    settingsInputs.warmthSupported = warmthSupported;
#ifndef KOBALT_READER_HARDWARE
    settingsInputs.debugProgressAvailable = true;
#endif
    auto *settings = new kobalt::reader::SettingsPanel(this, settingsInputs);
    general = settings;
    settings->showDictionaries(services.downloadableDictionaries &&
                               !(services.devicePreview && services.devicePreview()));
    if (services.downloadableDictionaries) {
        auto *manager = static_cast<kobalt::DictionaryManager *>(dictionary.get());
        auto *dictionarySettings = settings->dictionaries;
        dictionarySettings->setLanguage(model->state().value("language").toString());
        manager->setLanguage(model->state().value("language").toString());
        const auto catalog = kobalt::DictionaryManager::catalog();
        dictionarySettings->setCatalog(catalog);
        auto updateDictionaries = [this, manager, dictionarySettings, catalog, previous = QSet<QString>{},
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
            dictionarySettings->setTransfer(transfer.id, transfer.received, transfer.total, transfer.installing);
            dictionarySettings->setState(installed, manager->busy(), manager->status());
            dictionaryPanel->setChoices(manager->choices());
            if (general->isVisible()) {
                fitSettings();
                refresh();
            }
        };
        dictionarySettings->installRequested = [manager](const QString &id) {
            manager->download(id);
        };
        dictionarySettings->removeRequested = [manager, dictionarySettings, catalog](const QString &id) {
            QString error;
            if (!manager->remove(id, &error)) {
                QSet<QString> installed;
                for (const auto &entry : catalog)
                    if (manager->installed(entry.id))
                        installed.insert(entry.id);
                dictionarySettings->setState(installed, manager->busy(), error);
            }
        };
        dictionarySettings->cancelRequested = [manager] {
            manager->cancel();
        };
        connect(manager, &kobalt::DictionaryManager::changed, this, updateDictionaries);
        updateDictionaries();
    }
    hideStatusOption = settings->hideStatusOption;
    hideDeviceOptionsOption = settings->hideDeviceOptionsOption;
    if (services.desktopUi) {
        hideStatusOption->setChecked(preferences.value("native_hide_status_bar").toBool(true));
        hideDeviceOptionsOption->setChecked(preferences.value("native_hide_device_options").toBool(true));
    }
    settingsPages = settings->settingsPages;
    darkOption = settings->darkOption;
    smoothOption = settings->smoothOption;
    progressOption = settings->progressOption;
    animationOption = settings->animationOption;
    edgeLightOption = settings->edgeLightOption;
    denseGraphOption = settings->denseGraphOption;
    pinStatsOption = settings->pinStatsOption;
    heavierTextOption = settings->heavierTextOption;
    linkIndicatorOption = settings->linkIndicatorOption;
    hideNonlinearOption = settings->hideNonlinearOption;
    bionicOption = settings->bionicOption;
    refreshButton = settings->refreshButton;
    resetDefaults = settings->resetDefaults;
    settingsError = settings->settingsError;
    connect(darkOption, &QCheckBox::clicked, this, [this](bool enabled) {
        applyDarkMode(enabled);
    });
    connect(bionicOption, &QCheckBox::clicked, this, [this](bool enabled) {
        applyBoldReading(enabled);
    });
    connect(heavierTextOption, &QCheckBox::clicked, this, [this](bool enabled) {
        applyHeavierText(enabled);
    });
    connect(refreshButton, &QPushButton::clicked, this, [this] {
        openScreenRefresh();
    });
    connect(settings->openAcknowledgments, &QPushButton::clicked, this, [this] {
        acknowledgments->open();
        for (auto *widget : acknowledgments->findChildren<QWidget *>())
            textRefresh(widget);
    });
    connect(settings->confirmDefaults, &QPushButton::clicked, this, [this] {
        restoringDefaults = true;
        clockOption->setChecked(true);
        darkOption->setChecked(false);
        smoothOption->setChecked(true);
        progressOption->setChecked(true);
        animationOption->setChecked(false);
        bionicOption->setChecked(false);
        screenRefreshMode = kobalt::ScreenRefresh::Smart;
        showScreenRefresh();
        resetDefaults->setText(strings::get("reader.defaults_selected"));
        resetDefaults->setEnabled(false);
        settingsPages->setCurrentIndex(AdvancedSettings);
    });
#ifndef KOBALT_READER_HARDWARE
    connect(settings->debugProgress, &QPushButton::clicked, this, [this] {
        showProgressDebug();
    });
#endif
    connect(settingsPages, &QStackedWidget::currentChanged, this, [this](int) {
        fitSettings();
        refresh();
    });
}

void Reader::buildNotices() {
    acknowledgments = new kobalt::Acknowledgments(this);
    // The navigation hides its panel edges while Acknowledgments covers the reader.
    acknowledgments->finished = [this] {
        acknowledgments->hide();
        general->raise();
        syncNavigation();
    };
    acknowledgments->hide();
    busyNotice = new kobalt::ui::ModalOverlay(this);
    busyNotice->card()->message(strings::get("reader.applying_appearance_changes"), "rrUpdatingText");
    // Create the bar before the initial refresh attributes are assigned.
    busyNotice->setDelayedProgress(false, 0, 0);
    renderNoticeTimer.setSingleShot(true);
    renderNoticeTimer.setTimerType(Qt::PreciseTimer);
    connect(&renderNoticeTimer, &QTimer::timeout, this, [this] {
        refreshRenderNotice();
    });
    busyNotice->card()->fitToWidth(m::notice::cardWidth);
    busyNotice->centerCard();
    brightnessNotice = new kobalt::ui::LightSwipeNotice(this);
    brightnessNotice->setObjectName("rrBrightnessNotice");
    brightnessNotice->setAttribute(Qt::WA_TransparentForMouseEvents);
    brightnessNotice->hide();
    brightnessHide.setSingleShot(true);
    brightnessHide.setInterval(m::lightSwipe::hideMilliseconds);
    connect(&brightnessHide, &QTimer::timeout, brightnessNotice, &QWidget::hide);
}

void Reader::connectLightGestures() {
    page->lightStarted = [this](bool warmth) {
        if (!isVisible() || services.suspended() || book->deviceTransition() || this->model->busy() ||
            appearanceEdit.preview() || general->isVisible() || topControls->isVisible() || choices->isVisible() ||
            dictionaryPanel->isVisible() || searchPanel->isVisible() || linksActive() || lookupPending || syncDialog ||
            finishDialog || applyingText || lightPopup->isVisible() || exitPending)
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
        brightnessHide.stop();
        return true;
    };
    page->lightChanged = [this](double moved) {
        if (gestureWarmth) {
            // Like Nickel's slider, a manual change turns automatic warmth off.
            const int minimum = light->warmthMinimum(), range = light->warmthMaximum() - minimum;
            if (range <= 0) {
                return;
            }
            const double percent =
                100.0 * (warmthInitial - minimum) / range + kobalt::ui::BrightnessGesture::change(moved);
            const int snapped = qBound(0, qRound(percent / 10.) * 10, 100);
            const int step = qMax(1, light->warmthStep());
            const int level =
                qBound(minimum, minimum + qRound(snapped * range / (100.0 * step)) * step, minimum + range);
            const int value = light->setWarmth(snapped == 100 ? minimum + range : level);
            if (value < 0)
                return;
            brightnessNotice->setLevel(true, qRound(100.0 * (value - minimum) / range));
        } else {
            const int value = light->setGestureLevel(brightnessInitial + kobalt::ui::BrightnessGesture::change(moved));
            if (value < 0)
                return;
            brightnessNotice->setLevel(false, value);
        }
        brightnessNotice->show();
        brightnessNotice->raise();
    };
    page->lightFinished = [this](bool save) {
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
        if (save)
            brightnessHide.start();
        else {
            brightnessHide.stop();
            brightnessNotice->hide();
        }
    };
}

void Reader::connectPageInput() {
    page->swiped = [this](int direction) {
        if (revealingLinks || dictionaryPanel->isVisible() || searchPanel->isVisible() || linksActive() ||
            lookupPending || appearanceEdit.preview() || general->isVisible() || choices->isVisible() ||
            topControls->isVisible())
            return;
        turnPage(model->state().value("page_progression_rtl").toBool() ? -direction : direction);
    };
    page->tapped = [this](QPoint point) {
        const qreal x = qreal(point.x()) / qMax(1, page->width());
        if (dictionaryPanel->isVisible() || searchPanel->isVisible() || linksActive() || lookupPending) {
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
        const bool menuTap = x > .33 && x < .67 && (!devicePreview || (y > .33 && y < .67));
        if (topControls->isVisible() || choices->isVisible()) {
            if (!devicePreview || menuTap) {
                flashAfterMenu = true;
                dismissControls();
            }
            return;
        }
        if (bookmarks->available() && page->bookmarkCorner().contains(point)) {
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
        if (devicePreview && x > .33 && x < .67)
            return;
        const int direction = x <= .33 ? -1 : 1;
        turnPage(model->state().value("page_progression_rtl").toBool() ? -direction : direction);
    };
}

void Reader::showFontPicker() {
    if (!appearanceEdit.panelOpen() || panels->currentIndex() != 0)
        return;
    const auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
    choices->fontPageRequested = [this](const QStringList &families) {
        for (const auto &family : families)
            registerPickerFonts(family);
    };
    QVector<Choice> items{
        {strings::get("reader.publisher_fonts"), QString(), "rrFontChoice0", strings::get("reader.recommended_fonts")}};
    pickerNativeFamilies = services.fontFamilies();
    const auto state = settings->state();
    const auto supplemental = supplementalFontFamily(state.value("language").toString());
    const auto requestedDefault = preferences.value("default_font_face").toString(kobalt::defaultFontFace);
    for (const auto &font : settings->fontCatalog()) {
        QString section;
        switch (font.origin) {
        case kobalt::FontOrigin::Recommended:
            section = strings::get("reader.recommended_fonts");
            break;
        case kobalt::FontOrigin::BuiltIn:
            if (!pickerNativeFamilies.contains(font.family, Qt::CaseInsensitive))
                continue;
            section = strings::get("reader.built_in_fonts");
            break;
        case kobalt::FontOrigin::Sideloaded:
            section = strings::get("reader.sideloaded_fonts");
            break;
        case kobalt::FontOrigin::Interface:
            if (font.family != supplemental && font.family != requestedDefault &&
                font.family != state.value("font_face").toString())
                continue;
            section = strings::get("reader.supplemental_fonts");
            break;
        }
        items.append({fontDisplayName(font.family), font.family, QString("rrFontChoice%1").arg(items.size()), section});
    }
    for (int i = 1; i < items.size(); ++i) {
        if (items[i].value == QStringLiteral("Libron")) {
            items.insert(1, items.takeAt(i));
            break;
        }
    }
    QString defaultFace = preferences.value("default_font_face").toString(kobalt::defaultFontFace);
    int defaultIndex = -1;
    for (int i = 1; i < items.size(); ++i)
        if (items[i].value == defaultFace)
            defaultIndex = i;
    if (defaultIndex < 0)
        defaultFace = kobalt::defaultFontFace;
    const int embedded = state.value("embedded_font_count").toInt();
    items[0].detail = embedded > 0
                          ? (embedded == 1 ? strings::get("reader.one_embedded_font")
                                           : strings::get("reader.embedded_fonts").arg(embedded))
                          : strings::get("reader.no_embedded_fonts") + '\n' +
                                strings::get("reader.uses_default_font").arg(fontDisplayName(kobalt::defaultFontFace));
    if (items.size() > 1)
        items[1].detail = (defaultFace == kobalt::defaultFontFace ? strings::get("reader.default_font") : QString());
    if (defaultIndex > 1)
        items[defaultIndex].detail = strings::get("reader.default_font");
    Choice slot{QStringLiteral("Aa"), defaultFace, "rrFontDefaultSlot", strings::get("reader.recommended_fonts")};
    slot.detail = strings::get("reader.set_default_font");
    slot.placeholder = true;
    items.insert(qMin(2, items.size()), slot);
    choices->defaultRequested = [this](const QString &face) {
        showFontDefaultDialog(face);
    };
    choices->selected = [this](const QString &face) {
        const auto *current = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        if (!appearanceEdit.queued() && face == current->fontFace())
            return;
        mutateAppearance([this, face](ReaderSession &preview) {
            if (preview.style(preview.fontSize(), preview.spacing(), face, preview.margin()))
                choices->selectValue(face);
        });
    };
    choosingTypography = true;
    const int bottom = navigation ? navigation->contentBottom(size()) : height();
    const int pickerHeight = qMin(choices->fontPanelHeight(), bottom - (navigation ? navigation->toolbarHeight() : 0));
    choices->setDropdownGeometry(QRect(0, bottom - pickerHeight, width(), pickerHeight));
    choices->showChoices(strings::get("reader.font"), std::move(items), settings->fontFace(),
                         ChoiceScreen::Mode::Fonts);
    refresh();
}

}
