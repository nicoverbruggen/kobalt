#pragma once
#include <future>
#include "app/reader/settings/interface_style.h"
#include "reading/features.h"
// Private to the reader window: its declaration and the widgets only it uses.
// Include it only from the reader's own translation units.
#include "ui/controls/buttons.h"
#include "ui/controls/options.h"
#include "ui/controls/sliders.h"
#include "ui/controls/page_scrubber.h"
#include "ui/controls/panels.h"
#include "ui/dialogs/modals.h"
#include "platform/api/keyboard_controller.h"
#include "ui/style/theme.h"
#include "app/reader_window.h"
#include "app/reader_interface.h"
#include "app/reader/views/opening.h"
#include "reading/reading_book.h"
#include "reading/appearance_settings.h"
#include "reading/reader_session.h"
#include "reading/page_navigation.h"
#include "ui/input/tap_filter.h"
#include "sync/kosync.h"
#include "sync/sync_coordinator.h"
#include "app/reader/settings/kosync_page.h"
#include "reading/bookmark_session.h"
#include "reading/highlight_session.h"
#include "reading/annotation_draft.h"
#include "storage/annotation_export.h"
#include "storage/record_writer.h"
#include "app/reader/views/reading_page.h"
#include "app/reader/views/image_viewer.h"
#include "app/reader/views/appearance_preview.h"
#include "app/reader/panels/choice_screen.h"
#include "app/reader/panels/activity_view.h"
#include "app/reader/panels/chapter_graph.h"
#include "app/reader/settings/acknowledgments.h"
#include "app/reader/panels/light_panel.h"
#include "app/reader/panels/bookmark_list.h"
#include "app/reader/panels/note_editor.h"
#include "app/reader/panels/dictionary_panel.h"
#include "app/reader/panels/link_panel.h"
#include "app/reader/panels/search_panel.h"
#include "app/reader/settings/appearance_panel.h"
#include "app/reader/settings/text_presets_panel.h"
#include "reading/text_presets.h"
#include "reading/style_sample_cache.h"
#include "storage/preview_store.h"
#include "support/save_wait.h"
#include "app/reader/panels/word_list.h"
#include "app/reader/panels/word_page.h"
#include "library/cover_thumbnails.h"
#include "storage/activity_records.h"
#include "storage/saved_words.h"
#include "appearance_edit_session.h"
#include "ui/controls/text.h"
#include "app/reader/views/navigation.h"
#include "platform/api/reader_host_services.h"
#include "platform/api/reader_orientation.h"
#include "render/screen_refresh.h"
#include "support/main_thread.h"
#include <QCheckBox>
#include <array>
#include <atomic>
#include <QElapsedTimer>
#include <QEvent>
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
#include <tuple>

class QVBoxLayout;

namespace kobalt::reader {
enum class SettingsPage;
class SettingsPanel;
class SleepGallery;
class SleepEditor;
// Global preference for the Wi-Fi power saver. Missing means on.
inline constexpr const char *wifiPowerSaverKey = "turn_off_wifi_on_open";
inline constexpr const char *disableReaderStandbyKey = "disable_reader_standby";

// Input and accessibility target only. The cached header image supplies its
// pixels and invalidates only the part whose text changed.
class ChapterHeader : public QPushButton
{
public:
    explicit ChapterHeader(QWidget *parent);
};

class MenuSessionLabel final : public ui::InterfaceLabel
{
public:
    using ui::InterfaceLabel::InterfaceLabel;
    void setCountingPages(bool counting);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool countingPages = false;
};

class Reader : public kobalt::ReaderWindow
{
public:
    ~Reader() override;
    Reader(std::unique_ptr<kobalt::ReadingBook> openedBook, kobalt::ReaderOrientation *orientation,
           kobalt::ReaderHostServices hostServices);
    bool statsUseWholeVolume() const
    {
        return !session->hasMainTextBoundary()
                || preferences.value("stats_word_scope").toString() == "volume";
    }
    void updateGraph();
    void openStatsWordScope();
    void refresh();
    void refreshRenderNotice();
    bool readyForPresentation() const override;
    void preparePresentation(std::function<void(bool)> finished) override;
    void checkpoint() override;
    bool flushBeforeStandby() override;
    bool pendingStorageSave() const override;
    // Runs the close and transition steps again when a save reports its end.
    kobalt::SaveWait saveWait{ this };
    void revokeNativeReading() override;
    void finishStorageSave(std::function<void(bool)> finished, bool nativeAllowed = true) override;
    void setOpeningSaveState(OpeningSaveState state) override;
    bool openingSaveParked() const override;
    void prepareUsb(std::function<void()> finished) override;
    void prepareShutdown(std::function<void()> finished, bool allowFailedSave = false) override;
    sleep::Snapshot powerScreenSnapshot() override;
    sleep::Snapshot presentedSleepSnapshot() const override;
    void sleepReader(bool externalScreenVisible) override;

    bool pendingNetworkSync() const override
    {
        return syncCoordinator && syncCoordinator->finalPending();
    }

    void prepareWake() override;
    void cancelWake() override;
    void sleepViewReady() override;
    void wakeReader() override;
    void resumeReadingClock() override;
    void progressTick() override;
    void refreshHostPresentation() override;

    void showPosition(const QString &position) override { goToPassage(position); }

    void pageButton(int direction) override;
    void physicalPageButton(int key, int direction) override;
    void scriptedPageButton(int direction) override;
    QJsonObject benchmarkState() const override;
    bool benchmarkCommand(int command, int argument) override;

    bool closeGuardActive() const override { return closeGuarded; }

    bool busy() const override
    {
        return resizeTimer.isActive() || session->busy() || session->preparingNeighbours()
                || stylingSamplesPending > 0;
    }

    // Automatic syncs follow the account setting, unless a performance run
    // has paused them for the whole process.
    bool kosyncAutomatic() const
    {
        return features::enabled(preferences, features::Kosync) && kosyncClient->automatic()
                && !kobalt::kosync::automaticPaused();
    }

    void setOpeningTiming(const QElapsedTimer &started, const QJsonObject &stages) override;

    kobalt::ActionTiming closingTiming() const override { return closingTimes; }

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
    void paintEvent(QPaintEvent *) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void observeReading();
    void prepareTransition(std::function<void()> finished, bool usb);
    void releaseUsbWrites(std::function<void()> finished);
    // Construction, in this order. Each step builds one group of widgets and
    // connects its signals.
    void saveNativeProgress();
    QString savedNativeAnchor;
    int savedNativePage = -1, savedNativePages = -1, savedNativePercent = -1;
    void buildReadingPage();
    ImageViewer *imageViewer = nullptr;
    quint64 imageRequest = 0;
    void buildAnnotations();
    // `notice` shows the On or Off indicator. Quick annotation mode passes false.
    void enterAnnotations(bool notice = true);
    void leaveAnnotations(bool notice = true);
    void showAnnotationModeNotice();
    void annotationTap(QPoint point);
    void loadAnnotationDragEdges();
    int annotationDragPageDirection(QPoint point) const;
    bool beginAnnotationDrag(QPoint point, int boundary);
    void moveAnnotationDrag(QPoint point);
    void finishAnnotationDrag(bool cancelled);
    void requestAnnotationDrag();
    void resolveAnnotationTokens(kobalt::AnnotationToken start, kobalt::AnnotationToken end,
                                 bool startPolicy, bool endPolicy, std::function<void()> after = {},
                                 std::function<void()> reject = {});
    void prepareAnnotation(bool leave = false, std::function<void()> after = {});
    void clearAnnotationDraft();
    bool annotationMenuSelectionEnabled() const;
    void syncAnnotationControls();
    void saveAnnotation(bool leave = false, std::function<void()> after = {});
    QPoint selectionLinkPoint() const;
    void showAnnotationActions();
    void updateAnnotationRects(bool showActions = false);
    void paintAnnotationDraft();
    void syncAnnotations();
    QString annotationViewportKey() const;
    void syncMarkupRequest();
    void finishAnnotationPresentation();
    void editAnnotation(const QString &id, bool navigate = true);
    void showSavedAnnotationAt(const QString &id, QRect anchor, bool navigate);
    // The color and style choices of a saved highlight. `colorStep` comes
    // from a noted highlight's Color action and offers Discard instead of the
    // highlight's other actions.
    void showSavedAppearance(const QString &id, QRect anchor, bool navigate, bool colorStep);
    void openNoteEditor(const QString &id);
    // Tells the Kobo refresh that a finger drags a selection or a handle.
    void setAnnotationDragRefresh(bool dragging);
    // Which presses the open selection menu passes on to the handles.
    void syncPopoverExclusions();
    QPointer<kobalt::NoteEditor> noteEditor;
    void showSavedAnnotation(const QString &id, QRect anchor, bool navigate = false);
    void showAnnotationMatches(QVector<QString> ids, QRect anchor, int page = 0);
    void removeAnnotation(const QString &id);
    QVector<kobalt::BookmarkRow> annotationRows() const;

    struct AnnotationAction
    {
        QString name;
        kobalt::ui::ControlIcon icon;
        QString label;
        std::function<void()> run;
    };

    void annotationActionsFor(const QVector<AnnotationAction> &actions, QRect anchor,
                              bool compactActions = true, const QString &paletteColor = {},
                              std::function<void(QString)> colorChanged = {},
                              const QString &selectedStyle = {},
                              std::function<void(QString)> styleChanged = {},
                              const QString &note = {});

    bool colorAnnotations() const { return services.colorDisplay && services.colorDisplay(); }

    QString annotationColor = "yellow", annotationSelectedColor = "yellow";
    QString annotationStyle = "shaded", annotationSelectedStyle = "shaded";
    void syncAnnotationAppearance();
    bool annotationColorChosen = false, annotationStyleChosen = false;

    bool minimalAnnotationMode() const
    {
        return preferences.value("minimal_annotation_mode").toBool(false);
    }

    bool annotationMode = false, annotationSaving = false, annotationShowActionsPending = false;
    std::unique_ptr<kobalt::AnnotationDraft> annotationDraft;
    QString annotationFrame;
    QRect annotationActionAnchor;
    quint64 savedHighlightEpoch = 0;
    QVector<QRect> savedHighlightRects;

    // One press that selects text or moves a selection handle.
    struct AnnotationDrag
    {
        // Active: the press moves the selection. AwaitingRelease: the gesture
        // ended at a page boundary before the finger lifted; the release
        // finishes it.
        enum class Phase { Idle, Active, AwaitingRelease };
        // What the press moves. None: a new selection, before its first word.
        // Anchored: the press began on blank paper and stays a page gesture.
        // Start and End: that end of the selection; the page passes them as 0
        // and 1.
        enum class Boundary { None = -1, Anchored = -2, Start = 0, End = 1 };

        Phase phase = Phase::Idle;
        Boundary boundary = Boundary::None;
        // The press began on a selection handle.
        bool handle = false;
        // A hit test, range resolution or page turn for this press is running.
        bool requestPending = false;
        // The finger lifted while a request was running.
        bool released = false;
        bool loadingEdges = false;
        // The page turn that this press asked for: -1, 1 or 0 for none.
        int turn = 0;
        kobalt::AnnotationPageEdges edges;
        QPoint point, origin;

        bool active() const { return phase == Phase::Active; }

        // The press moves one end of an existing selection.
        bool movesEnd() const { return boundary == Boundary::Start || boundary == Boundary::End; }

        void begin(Boundary pressed, QPoint at)
        {
            phase = Phase::Active;
            requestPending = released = false;
            boundary = pressed;
            handle = movesEnd();
            turn = 0;
            edges = {};
            origin = point = at;
        }

        void end()
        {
            phase = Phase::Idle;
            requestPending = false;
        }

        // The gesture is complete, but the finger may still be down.
        void endBeforeRelease()
        {
            requestPending = false;
            phase = released ? Phase::Idle : Phase::AwaitingRelease;
        }

        // True once, when the release that a completed gesture waited for arrives.
        bool takeAwaitedRelease()
        {
            if (phase != Phase::AwaitingRelease)
                return false;
            phase = Phase::Idle;
            return true;
        }

        void forgetAwaitedRelease() { takeAwaitedRelease(); }

        // A range that resolved in reverse order swaps the end being moved.
        void swapEnd()
        {
            if (movesEnd())
                boundary = boundary == Boundary::Start ? Boundary::End : Boundary::Start;
        }
    };

    AnnotationDrag annotationDrag;
    ReaderMarkupRequest annotationRequest;
    quint64 savedMarkupGeneration = 0;
    QString savedMarkupEditingId;
    bool savedMarkupReady = false;
    bool savedMarkupInColor = false;
    qint64 annotationImageKey = 0;
    bool annotationPresentationCapture = false;
    std::function<void(bool)> annotationPresentationReady;
    kobalt::ui::ModalOverlay *annotationPopover = nullptr;
    void startClockTimer();
    void startDeviceObservers();
    // The sleep cover, overlay and wallpapers, and today's reading for them.
    void prepareSleepImages();
    void buildToolbar();
    void buildBottomControls();
    kobalt::reader::AppearanceLayout buildMenu();
    void buildOverlays();
    void buildAppearance(const kobalt::reader::AppearanceLayout &layout);
    // The Text and Layout controls, the text presets bar and the Header and
    // footer rows are built when a Text or Layout page first opens, or by
    // preparePanels. Until then their controls are null or empty.
    void ensureAppearance();
    kobalt::reader::AppearanceLayout appearanceLayout;
    bool appearanceBuilt = false;
    // Settings is built when it first opens. Most readings never open it,
    // and building it takes a large part of the window build on a Kobo.
    // Until then general, kosyncPage and the Settings controls are null.
    // Builds Settings into `parent`; ensureSettings moves it into the window.
    void buildSettings(QWidget *parent);
    void connectSettingsPage(SettingsPage section);
    void prepareNewWidgets(QWidget *root);
    void showZenSettings();
    void showSettingsPage(SettingsPage section);
    void ensureSettings();
    // Debug mode: the Developer checkbox and the Debug page (reader_debug.cpp).
    void buildDebugSettings();
    void showDebugSettings();
    void refreshDebugFacts();
    void askCacheEviction(bool allBooks);
    // Set when the reader closes to evict the parsed-book cache: 1 for this
    // book, 2 for every book.
    int cacheEviction = 0;
    QPointer<kobalt::ui::ModalOverlay> evictionDialog;
    // What applyTheme gives each widget: the reader palette, its text and
    // button icons. Also for widgets built after the theme.
    void themeWidget(QWidget *widget, const QPalette &colors, bool builtInTheme = false);
    // The palette applyTheme gave the widgets. The window's own palette is
    // already themed, so it is not a base for another one.
    QPalette themePalette;
    // Builds the hidden panels once the book's background work has settled
    // and nobody has touched the screen for a moment, so that their first
    // opening does not wait for them and the build meets neither the
    // whole-book layout nor a tap. Opening a panel first builds it then.
    void preparePanels();
    // Since the last press or key. Invalid before the first one.
    QElapsedTimer inputQuiet;
    bool panelsWaiting = false;
    // preparePanels built one panel in this call; the next waits its turn.
    bool builtPanelNow = false;

    bool settingsVisible() const { return general && general->isVisible(); }

    // The KOSync client and coordinator. Its page in Settings is wired by
    // buildKosyncPage() when Settings is built.
    void buildKosync();
    void buildKosyncPage();
    void connectKosyncSchedule(SettingsPanel &settings);
    void refreshKosyncAccount();
    void dismissKosyncEditors();
    void requestManualKosync();
    void updateKosyncToolbar();
    void synchronizeKosync(bool interactive = false);
    QByteArray kosyncDocument() const;
    double kosyncPercentage() const;
    void prepareKosyncPresentation(std::function<void(bool)> finished);
    // The Wi-Fi power saver runs as the last step before the reader takes
    // input, and at sleep after the sleep sync. At opening it applies only
    // when Kobalt would sync this book and automatic KOSync is off.
    bool wifiPowerSaverVisible() const;
    bool wifiPowerSaverApplies(bool sleeping = false) const;
    void saveWifiPower(std::function<void()> next);
    void saveWifiPowerAtSleep();
    void cancelWifiPowerAtSleep();
    void finishKosyncPresentation();
    void prepareAnnotationPresentation(std::function<void(bool)> finished);
    void showKosyncChoice();
    bool kosyncEreaderPreview() const;
    void updateKosyncFeatures();
    void syncKosyncWhileReading();
    qint64 kosyncClock() const;
    void flushKosync(std::function<void()> finished, bool enabled = true);
    void finishKosync(const QString &message, bool finishing);
    void cancelKosync();
    void restartKosync();
    void showKosyncOpening();
    void hideKosyncOpening();
    void kosyncStatus(const QString &message);
    // Declared before the client, which uses it until destroyed.
    std::unique_ptr<QNetworkAccessManager> kosyncHttp;
    std::unique_ptr<kosync::Client> kosyncClient;
    // Destroy the attempt owner before its borrowed client and HTTP transport.
    std::unique_ptr<kosync::SyncCoordinator> syncCoordinator;
    KosyncPage *kosyncPage = nullptr;
    QTimer kosyncOpeningGrace;
    ReadingTimer kosyncIntervalClock;
    std::function<void(bool)> kosyncPresentationReady;
    bool kosyncOpening = false;
    // The power saver's request at sleep. Wake deletes it, which drops a
    // Bluetooth answer that arrives after wake.
    QPointer<QObject> wifiSleepRequest;
    // The next opening-style sync follows a wake, not a book opening.
    bool kosyncWaking = false;
    QPointer<ui::ModalOverlay> kosyncDialog;
    QString kosyncMessage;
    quint64 kosyncChoiceEpoch = 0;
    bool kosyncStarted = false;
    // The position the last check saw, and when a sync last started, an
    // opening or wake skipped it or a deferred choice came back, from kosyncClock().
    QString kosyncSeenPosition;
    qint64 kosyncSubmitted = 0;
    bool kosyncPositionFailed = false;
    bool kosyncPromptDeferred = false;
    void showFontPicker();
    void showFontDefaultDialog(const QString &face, bool instructions);
    void buildNotices();
    void connectLightGestures();
    static QEvent::Type lightCommandEventType();
    void dispatchLightGesture();
    void connectPageInput();
    // The phases of refresh(), in this order.
    bool publishPendingDarkMode();
    void syncFrameState();
    void finishAppearanceApply();
    bool syncFontPicker();
    void showChapterText();
    void updateHeaderText(const QString &chapter);
    void showPageImage(const QImage &nextImage);
    void sampleReflowText();
    QRect appearancePreviewSpace() const;
    bool appearancePreviewFits() const;
    void syncAppearancePreview();
    void paintTurn();
    void syncAppearanceControls(bool pickingFont);
    ReaderSession *appearanceSession() const;
    void fitAppearance();
    void syncReadingControls();
    // After a page turn with the menu and every panel hidden, refresh() skips
    // the control syncs and marks the controls stale. They sync before they
    // show: showControls() and openPanel() sync on their own, and the event
    // filter catches every other way a control container becomes visible.
    bool controlsHidden() const;
    void syncReadingState();
    void syncStaleControls();
    bool controlsStale = false;
    void publishResize();
    void openPendingDestination();
    kobalt::ReaderHostServices services;
    std::unique_ptr<kobalt::WindowPresentation> presentation;
    std::unique_ptr<kobalt::ReadingBook> book;
    kobalt::ReaderSession *session;
    QPointer<kobalt::ReaderOrientation> orientation;
    QPointer<kobalt::ui::ModalOverlay> syncDialog, finishDialog;
    bool finishChoiceResolved = false, finishChoiceForExit = false;
    bool importingProgress = false, comparingProgress = false;
    // A progress check found the reader busy. refresh() repeats it once free.
    bool progressCheckDeferred = false;
    enum class ExitProgress { Normal, WaitingForSync, LocalOnly };
    ExitProgress exitProgress = ExitProgress::Normal;
    void showExitSyncWait();
    void clearExitSyncWait();
    bool localStorageSavePending() const;
    bool exitPending = false, exportRunning = false;
    bool lifecycleHidden = false;
    QImage sleepImage, sleepCover, sleepOverlay;
    bool sleepOverlayLoading = false;
    // The wallpapers of a Custom screen while sleeping and when powered off,
    // and the files the wallpaper folders hold, loaded off the GUI thread.
    QImage sleepWallpaper, sleepOffWallpaper;
    QStringList sleepWallpaperNames;
    quint64 sleepWallpaperRequest = 0;
    sleep::Tile activeSleepTile = sleep::Tile::Bookmark;
    // The design and text of the current sleep screen, without its image, and
    // what lies under its status so a battery change can repaint only that.
    sleep::Snapshot sleepShot;
    // Sleep and power-off share one random status until the reader wakes.
    sleep::Design pickedStatus = sleep::Design::Stock;
    bool statusPickDue = true;
    QPointer<QLabel> privateSleepFrame;
    QVector<QImage> sleepUnderlay;
    // Other openings on this local date, loaded off the GUI thread.
    // The current opening is counted from memory to avoid checkpoint races.
    std::future<qint64> todayReading;
    QString todayReadingDate;
    qint64 todayReadingFinished = -1;
    QFileInfo sleepCoverSource;
    sleep::Snapshot exitSleepSnapshot;
    QString sleepProgress, sleepRemaining;
    int sleepPercent = -1;
    BatteryStatus sleepBattery;
    quint64 sleepCoverRequest = 0;
    Qt::ScreenOrientation sleepOrientation = Qt::PrimaryOrientation;
    bool sleepNeedsImageCleanup = true, sleepFramePresented = false;
    QTimer *clockTimer = nullptr;
    // The local date, minute of the day and UTC offset distinguish repeated
    // minutes after standby or a time zone change.
    using ClockMinute = std::tuple<QDate, int, int>;
    static ClockMinute clockMinute(const QDateTime &now);
    ClockMinute timerMinute, displayedMinute;
    QString displayedClock;
    bool clockUsesDesktopFill = false;
    const bool liveState = QFile::exists(book->paths().directory + "/live-state");
    QLabel *menuClock;
    MenuSessionLabel *menuSession;
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

    // Toolbar buttons whose state follows the reader. buildToolbar() makes
    // them once; a recomposition moves and hides them and never rebuilds them.
    struct ToolbarButtons
    {
        QPushButton *light = nullptr, *typography = nullptr, *layout = nullptr, *settings = nullptr,
                    *graph = nullptr;
    };

    ToolbarButtons toolbarButtons;
    // The weight sliders with their minus and plus buttons.
    QVector<QWidget *> weightControls;
    std::unique_ptr<kobalt::reader::Navigation> navigation;
    QVector<QWidget *> topActions;
    kobalt::ui::Panel *general = nullptr;
    std::unique_ptr<kobalt::LightControls> light;
    kobalt::LightPanel *lightPopup = nullptr;

    // Settings can be applied before a section has ever been visited.
    struct SettingsCheck
    {
        bool checked() const { return widget ? widget->isChecked() : initial; }

        void set(bool value)
        {
            initial = value;
            if (widget)
                widget->setChecked(value);
        }

        void bind(QCheckBox *control)
        {
            widget = control;
            set(initial);
        }

        QCheckBox *widget = nullptr;
        bool initial = false;
    };

    SettingsCheck darkOption, animationOption, edgeLightOption, lightGestureFeedbackOption,
            automaticOpeningOption, minimalAnnotationOption, hideTabLabelsOption, zenModeOption,
            saveWordsOption, wifiPowerSaverOption, disableStandbyOption, hideNonlinearOption,
            inpageFootnotesOption, bionicOption, hideBookOnSleepOption, openingCoverOption,
            debugModeOption;
    QPushButton *autoRotateOption = nullptr, *lockRotationOption = nullptr;
    bool hideNativeStatus() const;

    // Zen mode keeps only Chapters, Search, Annotate and Bookmarks in the reading menu.
    bool zenMode() const { return preferences.value("zen_mode").toBool(false); }

    // Shows or hides Zen mode's reading stats and remembers the choice.
    void toggleZenStats();
    void setDenseGraph(bool dense);

    QJsonObject preferences;
    QLabel *settingsError = nullptr;
    kobalt::ui::ModalOverlay *busyNotice = nullptr;
    QTimer renderNoticeTimer, resizeTimer;
    kobalt::ui::LightSwipeNotice *brightnessNotice = nullptr;
    kobalt::ui::AnnotationModeNotice *annotationModeNotice = nullptr;
    QTimer annotationModeHide;
    // Reopens the selection menu after a handle adjustment; see reopenAnnotationActions.
    QTimer annotationReopen;
    void reopenAnnotationActions();
    kobalt::ui::ButtonFeedback *wordNotice = nullptr;
    void showWordNotice(const QString &text);
    void placeWordNotice();
    bool raisingPopups = false;
    QPointer<kobalt::ui::ModalOverlay> debugDialog;
    QTimer brightnessHide;
    QTimer lightFeedback;
    double lightMovement = 0;
    int lightFeedbackLevel = -1;
    bool lightCommandPending = false, lightGestureActive = false;
    int brightnessInitial = 0, warmthInitial = 0;
    bool gestureWarmth = false, warmthAutomatic = false, warmthSupported = false;
    int pendingDarkMode = -1, savingDarkMode = -1;
    bool applyingText = false, closing = false;
    kobalt::ChoiceScreen *choices = nullptr;
    kobalt::ChapterGraph *chapterGraph = nullptr;
    bool seekOpen = false;

    bool navigateAvailable() const { return features::enabled(preferences, features::Navigate); }

    void openSeek();
    kobalt::Acknowledgments *acknowledgments = nullptr;

    bool acknowledgmentsVisible() const { return acknowledgments && acknowledgments->isVisible(); }

    QStackedWidget *panels;
    QLabel *previewLabel;
    kobalt::reader::AppearancePreview *appearancePreview = nullptr;
    kobalt::reader::AppearancePreview::Axis appearancePreviewAxis =
            kobalt::reader::AppearancePreview::Axis::None;
    // The page drawn from the bookmark, for the frame with this image key.
    QImage previewPage;
    qint64 previewPageKey = 0, previewPageRequested = 0;
    ReaderSession::PreviewPage previewPageKind = ReaderSession::PreviewPage::Layout;
    kobalt::ui::PageScrubber *pageScrubber = nullptr;
    QPushButton *resetDefaults = nullptr, *chapterButton;
    QStackedWidget *settingsPages = nullptr;
    QVector<QPushButton *> rotationButtons;
    bool restoringDefaults = false, themeReady = false, darkTheme = false;
    // Settings > Defaults > Restore default presets was confirmed. It applies
    // when Settings closes.
    bool restoringPresets = false;
    // openSettings() selects the first page itself and fits once at its end,
    // so a page change while it runs needs no fit of its own.
    bool openingSettings = false;
    // Settings pages that have shown in this window. Their rows measure
    // correctly before Settings shows again.
    QSet<const QWidget *> settingsPagesShown;
    kobalt::ui::FontButton *fontButton = nullptr;
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
    // Made the first time a header or footer slot is chosen.
    kobalt::reader::FieldChooser *fieldChooser = nullptr;
    kobalt::reader::FieldChooser *ensureFieldChooser();
    std::function<void(const std::function<void(kobalt::headerfooter::Layout &)> &)>
            fieldChooserEdit;
    QWidget *chromeView() const;
    void openHeaderFooterView();
    void closeChromeViews();
    enum class ChromeFields { Reading, SlotMap, Chooser };
    kobalt::headerfooter::Values chromeValues(ChromeFields fields);
    kobalt::headerfooter::Layout customChrome() const;
    // Custom has a saved layout that differs from Kobalt's.
    bool customModified() const;
    // Before an edit replaces a modified Custom layout with a preset's, ask.
    void confirmCustomReplace(const std::function<void()> &proceed,
                              const std::function<void()> &cancel);
    // Reads the preset and layout from the preferences and shows them.
    void loadChrome();
    // Saves the preset, and the Custom layout when given, then shows them.
    void saveChrome(kobalt::headerfooter::Preset preset,
                    const kobalt::headerfooter::Layout *custom = nullptr,
                    const kobalt::headerfooter::Layout *text = nullptr,
                    std::function<void()> saved = {});
    void syncChromeControls();
    void openFieldChooser(int slot);
    void closeFieldChooser();
    // True when a slot shows something that changes with the clock.
    bool chromeTicks() const;
    // The Book status field refreshes every 5 seconds, and only while it
    // shows in a header or footer of the reading page. Without it nothing
    // wakes the reader for it.
    bool bookStatusShown() const;
    bool menuShowsPagination() const;
    void syncStatusTimer();
    QTimer *statusTimer = nullptr;
    // How long the whole book took after the session's layout start that
    // statusLayout names, or -1 until it shows.
    quint64 statusLayout = 0;
    qint64 statusWholeBookMilliseconds = -1;
    bool clockNeeded() const;
    bool paceNeeded() const;
    kobalt::ui::SettingSlider *sizeSlider = nullptr, *opticalSizeSlider = nullptr,
                              *weightSlider = nullptr, *variableWeightSlider = nullptr,
                              *spacingSlider = nullptr, *marginSlider = nullptr,
                              *verticalMarginSlider = nullptr, *bookSlider = nullptr;
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
    void openSearch(const QString &query = {});
    kobalt::LinkPanel *linkPanel = nullptr;
    QPointer<kobalt::ui::ModalOverlay> linkDialog;
    QPointer<kobalt::ui::ModalOverlay> fontDefaultDialog, settingsSheet;
    QJsonObject settingsFontFaces;
    QString settingsDefaultFontFace;
    InterfaceStyle themeDraft, customTheme;
    QString settingsMonoFace, settingsStripFace;
    int settingsStripWeight = 2;
    InterfaceStyle themeStyle(const QString &theme) const;
    void loadThemeSettings();
    void markThemeCustom();
    QString settingsDictionaryFontFace;
    QString settingsTimeRemainingFormat, settingsPercentageFormat, settingsSecondaryNumbers;

    QString secondaryNumberStyle() const
    {
        return preferences.value(secondaryNumberKey).toString(secondaryNumberStyles().first());
    }
    QString settingsRenderingMode;
    int settingsDictionaryFontSize = 100;
    QString interfaceTheme() const;
    void applyThemeFonts();
    void applyInterfaceFonts();
    void showSettingsFonts();
    void showSettingsFonts(SettingsPage section);
    void openSettingsFont(const QString &key);
    QPushButton *linkReturn = nullptr;
    bool linkReturnAvailable = false;
    QString imageSuggestionPosition;
    bool imageSuggestionDismissed = false;

    struct LinkVisit
    {
        QString position;
        int page;
        QString chapter;
        bool pinnedShortcut = false;
    };

    QVector<LinkVisit> linkHistory;
    LinkVisit pendingPassage{};
    std::shared_ptr<std::atomic<quint64>> linkEpoch = std::make_shared<std::atomic<quint64>>(0);
    bool linkPending = false, returningFromLink = false;
    bool linksActive() const;
    // What covers the page. Each caller adds the terms that only it needs.
    // A typography panel, Settings or a choice screen.
    bool sheetOpen() const;
    // A prompt about the reading position: sync, KOSync or finishing the book.
    bool positionPromptOpen() const;
    // A tool over the page: the chapter graph, the light popup, the
    // dictionary, search, acknowledgments, a link or a pending lookup.
    bool readingToolOpen() const;
    kobalt::ui::Panel *linkDiscovery = nullptr;
    QLabel *linkDiscoveryHint = nullptr, *linkDiscoveryExit = nullptr;
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
    void lookupSelection();
    // My Words. Null with the JSON diagnostic backend, which has no database.
    std::unique_ptr<kobalt::SavedWords> savedWords;
    // Activity: the reading history behind Stats. The store reads on the
    // profile writer thread. Keep the view's widgets across openings.
    std::unique_ptr<kobalt::ActivityRecordStore> activityRecords;
    std::unique_ptr<kobalt::library::CoverThumbnails> activityCovers;
    QPointer<kobalt::ActivityView> activityView;

    bool activityOpen() const { return activityView && !activityView->isHidden(); }

    quint64 activityOpening = 0, activityBookRequest = 0;
    // Shown the first time Activity opens: the history covers this device only.
    QPointer<ui::ModalOverlay> activityWelcome;
    void showActivityWelcome();
    void hideActivityBook(const QString &book, bool hidden);
    void deleteActivityBook(const QString &book, const QString &title);
    QDate activityToday() const;
    void openActivity();
    void closeActivity();
    void loadActivity();
    void finishActivityEdit(bool ok);
    void mapActivityBook(const QString &book);
    void loadActivityCovers(const QVector<kobalt::activity::BookTotal> &books, QSize size);
    kobalt::WordList *wordList = nullptr, *allWords = nullptr;
    kobalt::WordPage *wordPage = nullptr;
    QString lookupContext, lookupPosition;
    // The latest lookup as the sheet's save cell would save it.
    kobalt::SavedWord lookupSnapshot;
    quint64 savedLookupEpoch = 0;
    void buildWords();
    // The annotation key that saved words share with bookmarks and highlights.
    QString wordsBook() const;
    QString wordsSource() const;
    // Saves a lookup that found a definition and shows the sheet's save cell.
    void recordLookup(const QString &query, const kobalt::DictionaryResult &result);
    void showWords();
    void showAllWords();
    void reloadWords();
    void openWord(const kobalt::SavedWord &word);
    void resizeText();
    // The host's cutout and corner bands for the current size.
    kobalt::SafeArea safeArea() const;
    // The book's text area: the window without the safe area, header and footer.
    QSize bodySize() const;
    void applyTheme(bool dark);
    void registerPickerFonts(const QString &family = {});
    QString bookFace();

    void startClock();
    void startClock(const QDateTime &now);
    QDateTime localDateTime() const;
    QString clockText();
    void updateMenuStatus(bool refreshDevice = false);
    void updateMenuSession();
    void updateBatteryIndicators(const BatteryStatus &battery);
    void updateReadingStats();
    void configureNavigation();
    void activateNavigation(kobalt::NavigationDestination destination);
    QRect panelBounds(int panelHeight) const;
    void syncNavigation();
    void syncPinnedBookmarks();
    void syncStatusBoundary();
    bool statusBoundaryPending = false;
    void showControls();
    // returnToPage is false when another view replaces the menu, such as Search.
    void dismissControls(bool returnToPage = true);
    void toggleLight();
    void fitSettings(bool opening = false);
    void openSettings();
    void applyEdgeLight();
    void applyDarkMode(bool enabled);
    void applyBoldReading(bool enabled);
    void prepareSleepCover();
    void prepareSleepOverlay();
    void prepareSleepWallpapers();
    void updateSleepOverlayOption();
    void updateSleepWallpaperOption();
    // The Custom editor, or the arrange page of a design whose status moves.
    void openSleepEditor(sleep::Design design);
    void closeSleepEditor();
    void showSleepWallpaperHelp();
    // Each status design's place for the open book.
    QHash<int, sleep::Place> sleepPlaces() const;
    // `poweredOff` selects the power-off look of a Custom screen.
    sleep::Snapshot captureSleepScreen(bool poweredOff = false);
    sleep::Design sleepStatusPick(bool poweredOff);
    // The clean backdrop of `look` at `size` in `shot`: its image, whether the
    // page is dimmed and where the cover sits. Leaves the image null when the
    // backdrop has nothing to show, such as a coverless book without a page.
    void sleepBackdrop(const sleep::Look &look, QSize size, sleep::Snapshot &shot,
                       bool poweredOff, bool pageReady = true);
    sleep::Snapshot privateSleepSnapshot(bool poweredOff = false);
    sleep::Details sleepDetails(bool poweredOff = false);
    void loadTodayReading();
    void updateSleepBattery(const BatteryStatus &battery);
    void showSleepScreen();
    void openSleepScreen();
    void loadGalleryCover();
    void showSleepOverlayHelp();
    void editSleepGreeting();
    // Text presets: the bar and cards of the Text panel, All presets, the
    // save and rename dialog, and their part of Settings > Defaults.
    void buildTextPresets();
    void syncTextPresets();
    ReaderSession *textPresetSettings();
    kobalt::reader::TextPresetView textPresetView(const kobalt::textpresets::Preset &preset);
    void applyTextPreset(const QString &id);
    void saveTextPresets(QJsonObject next, const QVector<kobalt::textpresets::Preset> &presets,
                         std::function<void()> saved = {});
    void updateTextPreset(const QString &id);
    void saveNewTextPreset();
    void editTextPresetName(const QString &id);
    // The gallery page whose previews are rendering or shown, as the sample
    // keys of its previews, and the flag that stops its renders when another
    // page or a close replaces it.
    QVector<QByteArray> textPresetPreviewKeys;
    std::shared_ptr<std::atomic_bool> textPresetPreviewRender;
    // The preset's sample on the fixed tile canvas. It depends only on the
    // preset, so its tile is the same on every device and in every book.
    kobalt::TextSampleRequest textPresetSample(const kobalt::textpresets::Preset &preset);

    struct TextPresetPreview
    {
        QString id;
        kobalt::TextSampleRequest request;
        // Everything that decides the sample's pixels.
        QByteArray tile;
    };

    QVector<TextPresetPreview>
    textPresetPreviews(const QVector<kobalt::textpresets::Preset> &presets, const QStringList &ids);
    // Draws the sample on the engine worker, keeps it and calls `finished`
    // with it, or with a null image when it failed.
    void drawTextPresetTile(const TextPresetPreview &preview,
                            std::function<void(const QImage &)> finished);
    // Draws the missing tiles of the user's presets.
    void drawTextPresetTiles(const QVector<kobalt::textpresets::Preset> &presets);
    kobalt::storage::PreviewStore &textPresetTiles();
    std::unique_ptr<kobalt::storage::PreviewStore> textPresetTileStore;
    // The presets, the one in force and whether the text differs from it.
    void textPresetChoice(QVector<kobalt::textpresets::Preset> *presets, QString *selected,
                          bool *modified);
    void setTextPresetGalleryPresets(kobalt::reader::TextPresetGallery *gallery,
                                     const QVector<kobalt::textpresets::Preset> &presets,
                                     const QString &selected);
    QRect textPresetGalleryBounds() const;
    void showTextPresetActions(const QString &id);
    void confirmDeleteTextPreset(const QString &id);
    void openTextPresetGallery();
    void renderTextPresetGallery();
    void closeTextPresetGallery();
    void showTextPresetSettings();
    void openNewBookText();
    sleep::Snapshot sleepPreviewSample(bool poweredOff = false);
    void openSessionTimeout();

    struct SleepOption
    {
        QString value, label, explanation;
        bool enabled = true;
    };

    // Sleep and session choices share one popup.
    void openSleepChoice(const QString &name, const QString &optionPrefix, const QString &heading,
                         const QVector<SleepOption> &options, int current,
                         std::function<void(int)> chosen, const QString &description = {});
    void saveSleepPreferences(const QJsonObject &next, storage::RecordWriter::Done done = {});
    void closeSleepScreen();

    // One row of a choice pop-up.
    struct ChoiceOption
    {
        QString name, label, description;
        bool enabled = true;
    };

    // The setting choices of Settings share one pop-up; see reader_dialogs.cpp.
    void openChoicePopup(QPointer<kobalt::ui::ModalOverlay> &slot, const QString &name,
                         const QString &title, const QString &hint,
                         const QVector<ChoiceOption> &options, int current,
                         std::function<void(int)> chosen, bool focus = true);
    void closePopup(QPointer<kobalt::ui::ModalOverlay> &slot);
    void savePreferences(const QJsonObject &next, storage::RecordWriter::Done done = {});
    void showTapZones();
    void openTapZones();
    void showPageNavigation();
    void openPageNavigation();
    void showTappableLinks();
    void openTappableLinks();
    void showAnnotationInteraction();
    void openAnnotationInteraction();
    // Quick annotation mode enters annotation mode for each held selection
    // and leaves it when that selection ends.
    bool quickAnnotations() const;
    bool usesAnnotationToggle() const;
    void settleQuickAnnotations();
    void showMenuView();
    void openMenuView();
    void showChapterGroups();
    void openChapterGroups();
    void showNavigationBarButtons();
    void openNavigationBarButtons();
    void applyAutomaticOpening(bool enabled);
    void applySettings();
    storage::RecordWriter::Write preferenceWrite(const QJsonObject &next) const;
    void recordPreferenceChanges(const QJsonObject &next);
    using PreferencesWritten = std::function<void(const QJsonObject &, bool)>;
    void writePreferences(const QJsonObject &next, PreferencesWritten done);
    void finishSettingsWrite(const QJsonObject &next, bool saved);
    void finishPreviewWrite(const QJsonObject &next, bool saved);
    pageButtons::Buttons buttonControls() const;
    void saveButtonControls(const QJsonObject &actions);
    void saveMarksSort(const QString &order);
    struct MarksSortWrites
    {
        QJsonValue saved;
        int pending = 0;
    };
    std::shared_ptr<MarksSortWrites> marksSortWrites;
    bool savingPreferences = false, savingButtonControls = false, savingDenseGraph = false;
    // Counts preferences writes. An asynchronous save that fails undoes its
    // change only when no later write started: a later write already carries
    // the change, and its own result decides.
    quint64 zenStatsWrite = 0;

    void showSettingsWriteError();
    void selectPanel(int index);
    void openPanel(int index);
    void schedulePreviewWarm();
    void mutateAppearance(std::function<void(ReaderSession &)> operation);
    void finishPreview();
    void resumeAppearanceApply();
    void openImagePreview(QPoint point, const QString &resource = {});
    void showChapters(ContentsList mode = ContentsList::Chapters);
    void syncChapterBoundaries();
    quint64 chapterActionRequest = 0;
    void showChapterActions(int index);
    void toggleGraph();
    void openGraph();
    void showProgressDebug();
    bool progressBlocked() const;
    void dismissProgressChoice();
    void showProgressChoice();
    void buildProgressChoice(const kobalt::ReadingProgress &shown, int direction, bool located);
    void pageButton(int direction, quint32 sourceTime, bool userInput = true, int physicalKey = 0);
    void turnPage(int direction, quint32 sourceTime = 0, bool userInput = true,
                  ui::InputClock clock = ui::InputClock::Pointer);
    ui::TapFilter pageTurnFilter;
    QElapsedTimer pageTurnClock;
    bool pageList(int direction);
    // Runs from a tap or swipe until its page is painted; invalid otherwise.
    QElapsedTimer turnStarted;
    // How long the last turn took from the accepted tap to its page image,
    // for the header and footer. Negative before the first turn.
    qint64 pageTurnMilliseconds = -1;
    // The Settings button ignores the second tap of a double tap on a panel's
    // close cell, which sits where the Settings button is. A tap that waited
    // in the queue while the screen repainted still counts as part of the
    // double tap, but only if it was pressed within the guard time of the
    // first. Closing Settings can hold the GUI longer than that on a device.
    bool closeGuarded = false;
    int closeGuardGeneration = 0;
    // Source timestamps of the newest press and of the press that armed the
    // close guard. Zero when the platform gives no timestamp.
    quint32 lastPressTime = 0, closeGuardPressTime = 0;
    void guardCloseTap();
    bool closeGuardElapsed() const;
    // Full flashes clear the ghosting that partial page turns leave behind.
    // Turns since the last flash, for the Refresh the screen setting.
    int pagesSinceFlash = 0;
    kobalt::ScreenRefresh screenRefreshMode = kobalt::ScreenRefresh::Smart;
    kobalt::ui::CaptionButton *refreshButton = nullptr;
    QPointer<kobalt::ui::ModalOverlay> refreshPopup, wordScopePopup;
    kobalt::ui::CaptionButton *sleepScreenButton = nullptr, *sessionTimeoutButton = nullptr;
    QPointer<kobalt::ui::ModalOverlay> sleepPopup;
    QPointer<kobalt::reader::SleepGallery> sleepGallery;
    QPointer<kobalt::reader::SleepEditor> sleepEditor;
    QPointer<kobalt::ui::ChoiceModal> greetingEditor;
    std::unique_ptr<kobalt::KeyboardController> greetingKeyboard;
    kobalt::reader::TextPresetSection *textPresetSection = nullptr;
    QPointer<kobalt::reader::TextPresetGallery> textPresetGallery;
    QPointer<kobalt::ui::ChoiceModal> textPresetEditor;
    QPointer<kobalt::ui::ModalOverlay> textPresetActions;
    std::unique_ptr<kobalt::KeyboardController> textPresetKeyboard;
    // The presets the All grid shows: the saved ones, or for Text for new
    // books also the book defaults and the last book's text.
    QVector<kobalt::textpresets::Preset> textPresetGalleryEntries;
    bool textPresetChoosingNewBook = false;
    kobalt::ui::CaptionButton *pageNavigationButton = nullptr, *tapZonesButton = nullptr,
                              *navigationBarButton = nullptr, *chapterGroupsButton = nullptr;
    QPointer<kobalt::ui::ModalOverlay> pageNavigationPopup, tapZonesPopup, navigationBarPopup,
            menuViewPopup, tappableLinksPopup, chapterGroupsPopup, annotationInteractionPopup;
    kobalt::ui::CaptionButton *menuViewButton = nullptr, *tappableLinksButton = nullptr,
                              *annotationInteractionButton = nullptr;
    bool settingsLandscapeColumns = false, settingsChapterSpread = false;
    int settingsColumnSpacing = defaultColumnSpacingPercent;
    int settingsWordSpacing = 100;
    int settingsParagraphSpacing = -1;
    int settingsParagraphIndent = -1;
    QJsonObject settingsBeautifier;
    int settingsTracking = 0;
    bool settingsHangingPunctuation = false;
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
    // Rendered samples of this book, kept until their inputs change, the
    // size limit removes them or the book closes with this reader.
    kobalt::StyleSampleCache stylingSampleCache;
    void cancelStylingSamples();
    static QString refreshExplanation(kobalt::ScreenRefresh mode);
    static QString refreshLabel(kobalt::ScreenRefresh mode);
    void showScreenRefresh();
    void openScreenRefresh();
    mutable kobalt::BookmarkLayoutKeyCache bookmarkLayout;
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
    void finishMarksExport(AnnotationExportFormat format, bool saved, const QString &location = {});
    std::unique_ptr<AnnotationExporter> annotationExporter;
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
    // Turning Zen mode on or off replaces the whole menu. The first frame
    // after Settings closes clears the panel.
    bool flashAfterZenSwitch = false;
    // Whether the last painted frame showed Zen mode's Chapters or Bookmarks
    // view, or Settings over the Zen menu.
    bool zenViewPainted = false, zenSettingsPainted = false;
    int navigationActive = -1;
    bool navigationPanelPainted = false;
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
    void findLink(QVector<kobalt::PageLink> links, int index, QString label,
                  std::function<void(QPoint)> found);
    void locateOnPage(const kobalt::SearchHit &hit, std::function<void(QPoint)> found, int attempt);
    void recordOpening();
    void finishExit();
    void saveExit(quint64 request);
    void finishExitNative(quint64 request, const QString &location, const QString &error);
    void finishExitSaved(quint64 request);
    void saveTransition(quint64 request, const QString &location, const QString &error,
                        bool mapped, std::function<void()> finished, int failures = 0);
    void finishStorageData(std::function<void(bool)> finished, bool nativeAllowed, int failures = 0,
                           bool retriedAnnotations = false);
    OpeningSaveState openingSaveState = OpeningSaveState::Inactive;
    std::function<void(int)> retryDisposal;
    void handleDisposalFailure();
    void resumeDisposalSave(int failures);
    bool allowFailedTransition = false;
    void transitionSaveError(std::function<void()> retry, std::function<void()> discard);
    void failExit(quint64 request, const QString &error = {},
                  SaveErrorView::Cause cause = SaveErrorView::Cause::Failed);
    enum class SleepSave { Idle, Waiting, Retry, Failed };
    SleepSave sleepSave = SleepSave::Idle;
    quint64 sleepSaveRequest = 0, wakePositionEpoch = 0;
    QString sleepSavePosition;
    void recoverSleepSave();
    void clearPendingExit();
    void cancelExit();
    bool closeSaveRetried = false, closeWithoutSaving = false, releasingForUsb = false;
    quint64 closeRequest = 0;
    QElapsedTimer closeStage;
    QPointer<kobalt::ui::ChoiceModal> closeSaveError;
    std::unique_ptr<storage::RecordWriter> reportWriter;
    storage::RecordWriter preferencesWriter{ storage::RecordWriter::Delivery::All };
    void saveSoon();
    bool save(bool snapshot = false, bool background = false, bool persist = true);
    // True while a tap can turn the page. The main thread then must not write
    // storage or wait for another thread; see support/main_thread.h.
    bool takesInput() const;
    // Declared last, so it is unregistered before the members it reads.
    mainthread::Interaction interaction{ [this] { return takesInput(); } };
};
} // namespace kobalt::reader
