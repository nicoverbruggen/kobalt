#pragma once
// Private to the reader window: its declaration and the widgets only it uses.
// Include it only from the reader's own translation units.
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/sliders.h"
#include "interfaces/reader/ui/controls/page_scrubber.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "interfaces/reader_window.h"
#include "interfaces/reader_interface.h"
#include "interfaces/reader/views/opening.h"
#include "reading/reading_book.h"
#include "reading/appearance_settings.h"
#include "reading/reader_session.h"
#include "reading/kosync.h"
#include "interfaces/reader/settings/kosync_page.h"
#include "reading/bookmark_session.h"
#include "reading/highlight_session.h"
#include "storage/annotation_export.h"
#include "interfaces/reader/views/reading_page.h"
#include "interfaces/reader/panels/choice_screen.h"
#include "interfaces/reader/panels/chapter_graph.h"
#include "interfaces/reader/settings/acknowledgments.h"
#include "interfaces/reader/panels/light_panel.h"
#include "interfaces/reader/panels/bookmark_list.h"
#include "interfaces/reader/panels/dictionary_panel.h"
#include "interfaces/reader/panels/link_panel.h"
#include "interfaces/reader/panels/search_panel.h"
#include "interfaces/reader/settings/appearance_panel.h"
#include "appearance_edit_session.h"
#include "interfaces/reader/views/navigation.h"
#include "platform/api/reader_host_services.h"
#include "platform/api/reader_orientation.h"
#include "ui/screen_refresh.h"
#include "support/main_thread.h"
#include <QCheckBox>
#include <array>
#include <atomic>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QImage>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QTimer>
#include <functional>
#include <memory>

class QVBoxLayout;

namespace kobalt::reader {
// Input and accessibility target only. The cached header image supplies its
// pixels and invalidates only the part whose text changed.
class ChapterHeader : public QPushButton {
public:
    explicit ChapterHeader(QWidget *parent);
};

class Reader : public kobalt::ReaderWindow {
public:
    ~Reader() override;
    Reader(std::unique_ptr<kobalt::ReadingBook> openedBook, kobalt::ReaderOrientation *orientation,
           kobalt::ReaderHostServices hostServices);
    void updateGraph();
    void refresh();
    void refreshRenderNotice();
    bool readyForPresentation() const override;
    void preparePresentation(std::function<void(bool)> finished) override;
    void checkpoint() override;
    bool flushBeforeStandby() override;
    bool pendingStorageSave() const override;
    void revokeNativeReading() override;
    void finishStorageSave(std::function<void(bool)> finished, bool nativeAllowed = true) override;
    void prepareUsb(std::function<void()> finished) override;
    void prepareShutdown(std::function<void()> finished, bool allowFailedSave = false) override;
    sleep::Snapshot powerScreenSnapshot() override;
    void sleepReader() override;
    bool pendingNetworkSync() const override {
        return kosyncFinishing && kosyncFinishingElapsed.elapsed() < kosyncFinishBudget;
    }
    void prepareWake() override;
    void cancelWake() override;
    void sleepViewReady() override;
    void wakeReader() override;
    void progressTick() override;
    void refreshHostPresentation() override;
    void pageButton(int direction) override;
    bool busy() const override {
        return model->busy() || model->preparingNeighbours() || stylingSamplesPending > 0;
    }
    // Automatic syncs follow the account setting, unless a performance run
    // has paused them for the whole process.
    bool kosyncAutomatic() const {
        return kosyncClient->automatic() && !kobalt::kosync::automaticPaused();
    }
    void setOpeningTiming(const QElapsedTimer &started, const QJsonObject &stages) override;
    kobalt::ActionTiming closingTiming() const override {
        return closingTimes;
    }
    void locateText(const QString &text, std::function<void(QPoint)> found) override;
    void locateLink(const QString &label, std::function<void(QPoint)> found) override;
    QVector<QWidget *> popups() const;
    bool popup(QObject *object) const;
    void raisePopups();

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void observeReading();
    void prepareTransition(std::function<void()> finished, bool usb);
    // Construction, in this order. Each step builds one group of widgets and
    // connects its signals.
    void saveNativeProgress();
    QString savedNativeAnchor;
    int savedNativePage = -1, savedNativePages = -1;
    void buildReadingPage();
    void buildAnnotations();
    void enterAnnotations();
    void leaveAnnotations();
    void showAnnotationModeNotice();
    void finishAnnotations();
    void annotationTap(QPoint point);
    void loadAnnotationDragEdges();
    int annotationDragPageDirection(QPoint point) const;
    bool beginAnnotationDrag(QPoint point, int boundary);
    void moveAnnotationDrag(QPoint point);
    void finishAnnotationDrag(bool cancelled);
    void requestAnnotationDrag();
    void resolveAnnotationDraft(std::function<void()> after = {}, std::function<void()> reject = {});
    void resolveAnnotationTokens(kobalt::AnnotationToken start, kobalt::AnnotationToken end,
                                bool startPolicy, bool endPolicy, std::function<void()> after = {},
                                std::function<void()> reject = {});
    void prepareAnnotation(bool leave = false, std::function<void()> after = {});
    void setAnnotationBoundary(kobalt::AnnotationToken token, bool start);
    void clearAnnotationDraft();
    bool annotationMenuSelectionEnabled() const;
    void syncAnnotationControls();
    void saveAnnotation(bool leave = false, std::function<void()> after = {});
    QPoint selectionLinkPoint() const;
    void showAnnotationActions();
    void updateAnnotationRects(bool showActions = false);
    void paintAnnotationDraft();
    void syncAnnotations();
    void syncMarkupRequest();
    void finishAnnotationPresentation();
    void editAnnotation(const QString &id, bool navigate = true);
    void showSavedAnnotationAt(const QString &id, QRect anchor, bool navigate);
    void showSavedAnnotation(const QString &id, QRect anchor, bool navigate = false);
    void showAnnotationMatches(QVector<QString> ids, QRect anchor, int page = 0);
    void removeAnnotation(const QString &id);
    QVector<kobalt::BookmarkRow> annotationRows() const;
    struct AnnotationAction {
        QString name;
        kobalt::ui::ControlIcon icon;
        QString label;
        std::function<void()> run;
    };
    void annotationActionsFor(const QVector<AnnotationAction> &actions, QRect anchor, bool compactActions = true,
                              const QString &paletteColor = {}, std::function<void(QString)> colorChanged = {},
                              const QString &selectedStyle = {}, std::function<void(QString)> styleChanged = {});
    bool colorAnnotations() const {
        return services.colorDisplay && services.colorDisplay();
    }
    QString annotationColor = "yellow", annotationSelectedColor = "yellow";
    QString annotationStyle = "shaded", annotationSelectedStyle = "shaded";
    void syncAnnotationAppearance();
    bool annotationColorChosen = false, annotationStyleChosen = false;
    bool minimalAnnotationMode() const {
        return preferences.value("minimal_annotation_mode").toBool(false);
    }
    bool annotationMode = false, annotationSaving = false, annotationShowActionsPending = false,
         annotationAdjusting = false, annotationDirty = false;
    QString annotationStart, annotationEnd, annotationEditing, annotationFrame, annotationViewportFrame;
    kobalt::AnnotationToken annotationStartWord, annotationEndWord;
    bool annotationStartPolicy = false, annotationEndPolicy = false;
    QString annotationChosenStart, annotationChosenEnd;
    QRect annotationActionAnchor;
    quint64 annotationSaveRevision = 0;
    quint64 annotationEpoch = 0, savedHighlightEpoch = 0;
    QVector<QRect> draftHighlightRects, savedHighlightRects;
    kobalt::AnnotationGeometry draftHighlightGeometry;
    bool annotationDragging = false, annotationDragPending = false, annotationDragReleased = false;
    bool annotationHandleDragging = false, annotationDragAwaitingRelease = false, annotationDragLoadingEdges = false;
    int annotationDragTurn = 0;
    kobalt::AnnotationPageEdges annotationDragEdges;
    int annotationDragBoundary = -1;
    QPoint annotationDragPoint, annotationDragOrigin;
    ReaderMarkupRequest annotationRequest;
    qint64 annotationImageKey = 0;
    bool annotationPresentationCapture = false;
    std::function<void(bool)> annotationPresentationReady;
    kobalt::ui::ModalOverlay *annotationPopover = nullptr;
    void startClockTimer();
    void buildToolbar();
    void buildBottomControls();
    kobalt::reader::AppearanceLayout buildMenu();
    void buildOverlays();
    void buildAppearance(const kobalt::reader::AppearanceLayout &layout);
    void buildSettings();
    void buildKosync();
    void requestManualKosync();
    void updateKosyncToolbar();
    void synchronizeKosync(bool interactive = false);
    QByteArray kosyncDocument() const;
    void prepareKosyncPresentation(std::function<void(bool)> finished);
    void finishKosyncPresentation();
    void prepareAnnotationPresentation(std::function<void(bool)> finished);
    void showKosyncChoice();
    void startKosyncExchange(bool finishing);
    void continueKosyncWhenReady(std::function<void()> ready);
    void prepareKosyncNetwork(bool interactive, std::function<void()> ready);
    void flushKosync(std::function<void()> finished);
    void finishKosync(const QString &message);
    void cancelKosync();
    void showKosyncOpening();
    void hideKosyncOpening();
    void kosyncStatus(const QString &message);
    std::unique_ptr<kosync::Client> kosyncClient;
    std::unique_ptr<SyncNetwork> kosyncNetwork;
    KosyncPage *kosyncPage = nullptr;
    static constexpr int kosyncFinishBudget = 2000;
    QTimer kosyncTimer, kosyncDeadline, kosyncOpeningGrace, kosyncReaderRetry;
    QElapsedTimer kosyncFinishingElapsed;
    std::function<void()> kosyncFinished;
    std::function<void(bool)> kosyncPresentationReady;
    std::function<void()> kosyncReaderReady;
    bool kosyncFinishing = false, kosyncOpening = false, kosyncFlushWaiting = false;
    bool kosyncConnecting = false, kosyncAuthenticating = false;
    QPointer<ui::ModalOverlay> kosyncDialog;
    kosync::Reconciler kosyncProgress;
    QString kosyncMessage;
    quint64 kosyncEpoch = 0;
    bool kosyncStarted = false;
    bool kosyncPositionFailed = false;
    bool kosyncPromptDeferred = false;
    void showFontPicker();
    void showFontDefaultDialog(const QString &face, bool instructions);
    void buildNotices();
    void connectLightGestures();
    void connectPageInput();
    // The phases of refresh(), in this order.
    bool publishPendingDarkMode();
    void syncFrameState();
    void finishAppearanceApply();
    bool syncFontPicker();
    void showChapterText();
    void showPageImage(const QImage &nextImage);
    void paintTurn();
    void syncAppearanceControls(bool pickingFont);
    void fitAppearance();
    void syncReadingControls();
    void publishResize();
    void openPendingDestination();
    kobalt::ReaderHostServices services;
    std::unique_ptr<kobalt::WindowPresentation> presentation;
    std::unique_ptr<kobalt::ReadingBook> book;
    kobalt::ReaderSession *model;
    QPointer<kobalt::ReaderOrientation> orientation;
    QPointer<kobalt::ui::ModalOverlay> syncDialog, finishDialog;
    bool finishChoiceResolved = false;
    bool importingProgress = false, comparingProgress = false;
    bool exitPending = false, exportRunning = false;
    bool lifecycleHidden = false;
    QImage sleepImage, sleepCover;
    QFileInfo sleepCoverSource;
    sleep::Snapshot exitSleepSnapshot;
    QString sleepProgress, sleepRemaining;
    int sleepPercent = -1;
    BatteryStatus sleepBattery;
    quint64 sleepCoverRequest = 0;
    Qt::ScreenOrientation sleepOrientation = Qt::PrimaryOrientation;
    QTimer *clockTimer = nullptr;
#ifdef KOBALT_READER_HARDWARE
    const bool liveState = QFile::exists(book->paths().directory + "/live-state");
#endif
    QLabel *menuClock, *menuSession;
    kobalt::ui::BatteryIndicator *menuBattery;
    kobalt::ui::BluetoothIndicator *menuBluetooth;
    kobalt::ui::WifiIndicator *menuWifi;
    kobalt::ReadingPage *page;
    ChapterHeader *header;
    AppearanceSheet *menu;
    QFrame *bottomControls;
    kobalt::ui::Panel *topControls = nullptr;
    kobalt::ui::Panel *pinnedBookmarks = nullptr;
    QVector<kobalt::ui::CellButton *> pinnedButtons;
    kobalt::ToolbarComposition toolbarComposition;
    std::unique_ptr<kobalt::reader::Navigation> navigation;
    QVector<QWidget *> topActions;
    kobalt::ui::Panel *general = nullptr;
    std::unique_ptr<kobalt::LightControls> light;
    kobalt::LightPanel *lightPopup = nullptr;
    QCheckBox *darkOption, *inheritTextLayoutOption, *animationOption, *edgeLightOption, *lightGestureFeedbackOption, *denseGraphOption;
    QCheckBox *automaticOpeningOption = nullptr, *minimalAnnotationOption = nullptr, *hideTabLabelsOption = nullptr;
    QPushButton *autoRotateOption;
    bool hideNativeStatus() const;
    QJsonObject preferences;
    QLabel *settingsError;
    kobalt::ui::ModalOverlay *busyNotice = nullptr;
    QTimer renderNoticeTimer;
    kobalt::ui::LightSwipeNotice *brightnessNotice = nullptr;
    kobalt::ui::AnnotationModeNotice *annotationModeNotice = nullptr;
    QTimer annotationModeHide;
    bool raisingPopups = false;
    QPointer<kobalt::ui::ModalOverlay> debugDialog;
    QTimer brightnessHide;
    int brightnessInitial = 0, warmthInitial = 0;
    bool gestureWarmth = false, warmthAutomatic = false, warmthSupported = false;
    int pendingDarkMode = -1;
    bool applyingText = false, closing = false;
    kobalt::ChoiceScreen *choices = nullptr;
    kobalt::ChapterGraph *chapterGraph = nullptr;
    bool seekOpen = false;
    void openSeek();
    kobalt::Acknowledgments *acknowledgments;
    QStackedWidget *panels;
    QLabel *previewLabel;
    kobalt::ui::PageScrubber *pageScrubber = nullptr;
    QPushButton *resetDefaults, *chapterButton;
    QStackedWidget *settingsPages;
    QVector<QPushButton *> rotationButtons;
    bool restoringDefaults = false, themeReady = false, darkTheme = false;
    kobalt::ui::FontButton *fontButton;
    QVector<QPushButton *> alignmentButtons, hyphenButtons;
    // Header and footer. The preset is saved per book; the Custom layout is
    // shared by every book that selects it.
    kobalt::headerfooter::Preset chromePreset = kobalt::headerfooter::Preset::Default;
    kobalt::ui::DropdownButton *headerFooterRow = nullptr;
    kobalt::reader::SlotMap *slotMap = nullptr;
    QVector<kobalt::reader::PresetCard *> presetCards;
    kobalt::reader::StripSizeControl *headerSizeControl = nullptr, *footerSizeControl = nullptr;
    QVector<QPushButton *> progressBarButtons;
    // Header and footer and the field list fill the reading area like
    // Chapters, over the Layout panel, which stays open beneath them.
    kobalt::reader::HeaderFooterView *headerFooterView = nullptr;
    kobalt::reader::FieldChooser *fieldChooser = nullptr;
    QWidget *chromeView() const;
    void openHeaderFooterView();
    void closeChromeViews();
    kobalt::headerfooter::Values chromeValues();
    kobalt::headerfooter::Layout customChrome() const;
    // Custom has a saved layout that differs from Kobalt's.
    bool customModified() const;
    // Before an edit replaces a modified Custom layout with a preset's, ask.
    void confirmCustomReplace(const std::function<void()> &proceed, const std::function<void()> &cancel);
    // Reads the preset and layout from the preferences and shows them.
    void loadChrome();
    // Saves the preset, and the Custom layout when given, then shows them.
    bool saveChrome(kobalt::headerfooter::Preset preset, const kobalt::headerfooter::Layout *custom = nullptr,
                    const kobalt::headerfooter::Layout *text = nullptr);
    void syncChromeControls();
    void openFieldChooser(int slot);
    void closeFieldChooser();
    // True when a slot shows something that changes with the clock.
    bool chromeTicks() const;
    kobalt::ui::SettingSlider *sizeSlider, *opticalSizeSlider, *weightSlider, *variableWeightSlider, *spacingSlider,
        *marginSlider, *verticalMarginSlider, *bookSlider;
    kobalt::reader::AppearanceEditSession appearanceEdit;
    QSize appearanceViewport;
    quint64 warmScheduledGeneration = 0;
    quint64 warmAttemptGeneration = 0;
    using AppearanceDestination = kobalt::reader::AppearanceEditSession::Destination;
    QStringList pickerNativeFamilies;
    bool resizePending = false;
    bool choosingTypography = false, dismissingFontTouch = false, dismissingFontMouse = false;
    QString navigationFontFace;
    kobalt::SearchPanel *searchPanel = nullptr;
    quint64 searchEpoch = 0;
    // Where the current search trip started. Empty until a search opens.
    QString searchOriginal;
    int searchOriginalPage = 0;
    kobalt::SearchHit pendingSearchHit;
    void updateOrientationBlock();
    bool rotationControlsBlocked() const;
    void closeSearch();
    void openSearch();
    kobalt::LinkPanel *linkPanel = nullptr;
    QPointer<kobalt::ui::ModalOverlay> linkDialog;
    QPointer<kobalt::ui::ModalOverlay> fontDefaultDialog, settingsFontPopup;
    QJsonObject settingsFontFaces;
    QString settingsDefaultFontFace;
    QString settingsUiFontFace, settingsUiMonoFontFace, settingsHeaderFooterFontFace;
    QString settingsDictionaryFontFace;
    int settingsDictionaryFontSize = 100;
    int settingsHeaderFooterWeight = 2;
    void applyInterfaceFonts();
    void showSettingsFonts();
    void openSettingsFont(const QString &key);
    QPushButton *linkReturn = nullptr;
    bool linkReturnAvailable = false;
    struct LinkVisit {
        QString position;
        int page;
        QString chapter;
        bool pinnedShortcut = false;
    };
    QVector<LinkVisit> linkHistory;
    LinkVisit pendingPassage{};
    quint64 linkEpoch = 0;
    bool linkPending = false, returningFromLink = false;
    bool linksActive() const;
    QPushButton *linkIndicator = nullptr;
    kobalt::ui::Panel *linkDiscovery = nullptr;
    QLabel *linkDiscoveryIcon = nullptr, *linkDiscoveryHint = nullptr;
    QPushButton *linkDiscoveryToggle = nullptr;
    QLabel *linkDiscoveryExit = nullptr;
    QCheckBox *linkIndicatorOption = nullptr, *hideNonlinearOption = nullptr, *avoidWastefulStylingOption = nullptr;
    bool revealingLinks = false;
    QString revealedPosition;
    void syncLinkDiscovery();
    void showRevealedLinks();
    void leaveRevealedLinks();
    void hideRevealedLinks(bool restoreControls = true);
    void dismissLinkConfirmation();
    void dismissLinks();
    void updateLinkReturn();
    bool openLink(QPoint point, bool direct = false);
    void goToPassage(const QString &target, bool pinnedShortcut = false);
    kobalt::DictionaryPanel *dictionaryPanel = nullptr;
    std::unique_ptr<kobalt::DictionaryService> dictionary;
    QString lookupText;
    QRect lookupBounds;
    quint64 lookupEpoch = 0;
    bool lookupPending = false;
    void dismissLookup(bool keepGesture = false);
    void dismissDictionary();
    void lookupWord(QPoint point);
    void lookupSelection();
    void resizeText();
    void applyTheme(bool dark);
    void registerPickerFonts(const QString &family = {});
    QString bookFace();

    void startClock();
    void updateMenuStatus();
    void updateReadingStats();
    void configureNavigation();
    void activateNavigation(kobalt::NavigationDestination destination);
    QRect panelBounds(int panelHeight) const;
    void syncNavigation();
    void syncPinnedBookmarks();
    void syncStatusBoundary();
    bool statusBoundaryPending = false;
    void showControls();
    void dismissControls();
    void toggleLight();
    void fitSettings();
    void openSettings();
    void applyEdgeLight();
    void applyDarkMode(bool enabled);
    void applyBoldReading(bool enabled);
    void prepareSleepCover();
    sleep::Snapshot captureSleepScreen();
    void updateSleepBattery();
    void showSleepScreen();
    void openSleepScreen();
    void openSleepTile();
    void openSessionTimeout();
    struct SleepOption {
        QString value, label, explanation;
    };
    // Sleep and session choices share one popup.
    void openSleepChoice(const QString &name, const QString &optionPrefix, const QString &heading,
                         const QVector<SleepOption> &options, int current, std::function<void(int)> chosen,
                         const QString &description = {});
    bool saveSleepPreferences(const QJsonObject &next);
    void closeSleepScreen();
    void showTapZones();
    void openTapZones();
    void closeTapZones();
    void showPageNavigation();
    void openPageNavigation();
    void closePageNavigation();
    void showTappableLinks();
    void openTappableLinks();
    void closeTappableLinks();
    void showMenuView();
    void openMenuView();
    void closeMenuView();
    void showNavigationBarButtons();
    void openNavigationBarButtons();
    void closeNavigationBarButtons();
    void applyAutomaticOpening(bool enabled);
    void applySettings();
    bool writePreferences(const QJsonObject &next);
    void showSettingsWriteError();
    void selectPanel(int index);
    void openPanel(int index);
    void schedulePreviewWarm();
    void mutateAppearance(std::function<void(ReaderSession &)> operation);
    void finishPreview();
    void resumeAppearanceApply();
    void showChapters();
    void toggleGraph();
    void openGraph();
#ifndef KOBALT_READER_HARDWARE
    void showProgressDebug();
#endif
    bool progressBlocked() const;
    void dismissProgressChoice();
    void showProgressChoice();
    void buildProgressChoice(const kobalt::ReadingProgress &shown, int direction, bool located);
    void turnPage(int direction);
    bool pageList(int direction);
    // Runs from a tap or swipe until its page is painted; invalid otherwise.
    QElapsedTimer turnStarted;
    // Exit Kobalt ignores the second tap of a double tap on a panel's close
    // control, which sits where Exit Kobalt is. A tap that waited in the queue
    // while the screen repainted still counts as part of the double tap, but
    // only if it was pressed within the guard time of the first. Closing
    // Settings can hold the GUI longer than that on a device.
    bool exitGuarded = false;
    int exitGuardGeneration = 0;
    // Source timestamps of the newest press and of the press that armed the
    // exit guard. Zero when the platform gives no timestamp.
    quint32 lastPressTime = 0, exitGuardPressTime = 0;
    void guardExit();
    bool exitGuardElapsed() const;
    // Full flashes clear the ghosting that partial page turns leave behind.
    // Turns since the last flash, for the Refresh the screen setting.
    int pagesSinceFlash = 0;
    kobalt::ScreenRefresh screenRefreshMode = kobalt::ScreenRefresh::Smart;
    ui::DescribedOption *bionicOption = nullptr;
    kobalt::ui::CaptionButton *refreshButton = nullptr;
    QPointer<kobalt::ui::ModalOverlay> refreshPopup;
    kobalt::ui::CaptionButton *sleepScreenButton = nullptr, *sessionTimeoutButton = nullptr, *sleepTileButton = nullptr;
    QPointer<kobalt::ui::ModalOverlay> sleepPopup;
    kobalt::ui::CaptionButton *pageNavigationButton = nullptr, *tapZonesButton = nullptr,
                              *navigationBarButton = nullptr;
    QPointer<kobalt::ui::ModalOverlay> pageNavigationPopup, tapZonesPopup, navigationBarPopup, menuViewPopup,
                                     tappableLinksPopup;
    kobalt::ui::CaptionButton *menuViewButton = nullptr, *tappableLinksButton = nullptr;
    int settingsColumnSpacing = defaultColumnSpacingPercent;
    int settingsWordSpacing = 100;
    int settingsParagraphSpacing = -1;
    int settingsParagraphIndent = -1;
    QJsonObject settingsBeautifier;
    int settingsTracking = 0;
    QString settingsWidowsOrphans = "publisher";
    QString settingsVerticalSpacing = "off", settingsVerticalSpacingScope = "all";
    // `changed` is the styling setting that just changed, as a
    // SettingsPanel::AdvancedText value, or -1. Its samples already show
    // every option, so only the others are drawn again.
    void showAdvancedText(int changed = -1);
    void renderStylingSamples(int requested);
    // The selected option of a SettingsPanel::AdvancedText setting, in sheet order.
    int stylingChoice(int feature) const;
    // The latest sample render of each setting. Setting one stops its render.
    std::array<std::shared_ptr<std::atomic_bool>, 6> stylingSampleRenders;
    // Includes queued renders until their results reach the GUI thread.
    int stylingSamplesPending = 0;
    void cancelStylingSamples();
    static QString refreshExplanation(kobalt::ScreenRefresh mode);
    static QString refreshLabel(kobalt::ScreenRefresh mode);
    void showScreenRefresh();
    void closeScreenRefresh();
    void openScreenRefresh();
    // Declared after book, so it is destroyed before the session it borrows.
    std::unique_ptr<kobalt::BookmarkSession> bookmarks;
    std::unique_ptr<kobalt::HighlightSession> highlights;
    QString layoutKey() const;
    void syncBookmarks();
    kobalt::BookmarkList *bookmarkList = nullptr;
    QPointer<kobalt::ui::ModalOverlay> removeDialog;
    QPointer<kobalt::ui::ModalOverlay> marksActions;
    void openMarksActions();
    void closeMarksActions();
    void askExportMarks(AnnotationExportFormat format);
    QByteArray marksExportArchive(AnnotationExportFormat format);
    void confirmMarksExport(AnnotationExportFormat format, bool replace, const QString &path, std::function<void()> accepted);
    void saveMarksExport(AnnotationExportFormat format, const QString &path, bool replaceApproved);
    void chooseMarksExport(AnnotationExportFormat format);
    void finishMarksExport(AnnotationExportFormat format, bool saved, const QString &location = {});
    QPointer<QDialog> annotationExportPicker;
    QPointer<kobalt::ui::ModalOverlay> customReplaceDialog;
    QPointer<kobalt::ui::ModalOverlay> annotationErrorDialog, settingsWriteErrorDialog;
    QPointer<kobalt::ui::ModalOverlay> formattingWarningDialog;
    QVector<kobalt::BookmarkRow> bookmarkRows() const;
    void openBookmarks();
    void askRemoveBookmark(const QString &position);
    void toggleBookmark();
    void showBookmarkError();
    void showHighlightError();
    void showAnnotationError(const QString &message, const QString &title, const QString &name);
    void showStartupNotice();
    bool showNotice(const ReaderNotice &notice) override;
    void showFormattingWarning();
    bool flashAfterMenu = false, flashAfterPagePreview = false;
    void requestFlash(const char *reason);
    bool turnPainted = false;
    // Reading activity queues a due batch without a periodic wake timer.
    bool lowBattery = false;
    bool localSaveDue = false, standbyFlushQueued = false;
    void runIdleSave(bool beforeStandby);
    void showFinishChoice();
    // Record the local opening after the first page paints. Native status is
    // attempted once during preparePresentation, before this window takes input.
    bool recordedOpening = false;
    // Stages of the opening and of the close, for "reader-shown" and "book-close".
    QJsonObject openingStages;
    QElapsedTimer openingStarted, building, showing;
    bool shownOnce = false;
    // Local opening saves are queued after the first page paints.
    bool pagePainted = false;
    kobalt::ActionTiming closingTimes;
    void reportShown();
    void findLink(QVector<kobalt::PageLink> links, int index, QString label, std::function<void(QPoint)> found);
    void locateOnPage(const kobalt::SearchHit &hit, std::function<void(QPoint)> found, int attempt);
    void recordOpening();
    void finishExit();
    void saveExit(quint64 request);
    void finishExitNative(quint64 request, const QString &location, const QString &error);
    void finishExitSaved(quint64 request);
    void saveTransition(quint64 request, bool usb, const QString &location, const QString &error,
                        bool mapped, std::function<void()> finished, int failures = 0);
    void finishStorageData(std::function<void(bool)> finished, bool nativeAllowed, int failures = 0);
    bool allowFailedTransition = false;
    void transitionSaveError(std::function<void()> retry, std::function<void()> discard);
    void failExit(quint64 request, const QString &error = {});
    void cancelExit();
    void showSaving(int progress);
    void hideSaving();
    bool closeSaveRetried = false, closeWithoutSaving = false;
    quint64 closeRequest = 0;
    QElapsedTimer closeStage;
    int closeSaveProgress = 0;
    std::unique_ptr<SavingView> savingView;
    QPointer<kobalt::ui::ChoiceModal> closeSaveError;
    std::unique_ptr<storage::RecordWriter> reportWriter, preferencesWriter;
    void saveSoon();
    void save(bool snapshot = false, bool background = false, bool persist = true);
    // True while a tap can turn the page. The main thread then must not write
    // storage or wait for another thread; see support/main_thread.h.
    bool takesInput() const;
    // Declared last, so it is unregistered before the members it reads.
    mainthread::Interaction interaction{[this] { return takesInput(); }};
};
}
