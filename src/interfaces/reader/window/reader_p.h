#pragma once
// Private to the reader window: its declaration and the widgets only it uses.
// Include it only from the reader's own translation units.
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/sliders.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "interfaces/reader_window.h"
#include "interfaces/reader_interface.h"
#include "reading/reading_book.h"
#include "reading/reader_session.h"
#include "reading/bookmark_session.h"
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
#include <QCheckBox>
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
    void checkpoint() override;
    void prepareUsb(std::function<void()> finished) override;
    void prepareShutdown(std::function<void()> finished) override;
    void sleepReader() override;
    void prepareWake() override;
    void cancelWake() override;
    void sleepViewReady() override;
    void wakeReader() override;
    void progressTick() override;
    void refreshHostPresentation() override;
    void pageButton(int direction) override;
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
    void prepareTransition(std::function<void()> finished, int timeoutMs);
    // Construction, in this order. Each step builds one group of widgets and
    // connects its signals.
    void saveNativeProgress();
    QString savedNativeAnchor;
    int savedNativePage = -1, savedNativePages = -1;
    void buildReadingPage();
    void startClockTimer();
    void buildToolbar();
    void buildBottomControls();
    kobalt::reader::AppearanceLayout buildMenu();
    void buildOverlays();
    void buildAppearance(const kobalt::reader::AppearanceLayout &layout);
    void buildSettings();
    void showFontPicker();
    void showFontDefaultDialog(const QString &face);
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
    QTimer *clockTimer;
#ifdef KOBALT_READER_HARDWARE
    const bool liveState = QFile::exists(book->paths().directory + "/live-state");
#endif
    QLabel *menuClock, *menuSession;
    kobalt::ui::BatteryIndicator *menuBattery;
    kobalt::ui::BluetoothIndicator *menuBluetooth;
    kobalt::ReadingPage *page;
    ChapterHeader *header;
    AppearanceSheet *menu;
    QFrame *bottomControls;
    kobalt::ui::Panel *topControls;
    kobalt::ToolbarComposition toolbarComposition;
    std::unique_ptr<kobalt::reader::Navigation> navigation;
    QVector<QWidget *> topActions;
    kobalt::ui::Panel *general;
    std::unique_ptr<kobalt::LightControls> light;
    kobalt::LightPanel *lightPopup = nullptr;
    QCheckBox *darkOption, *smoothOption, *progressOption, *animationOption, *edgeLightOption, *denseGraphOption,
        *pinStatsOption, *heavierTextOption;
    QPushButton *autoRotateOption, *clockOption, *clockHideOption;
    QCheckBox *hideStatusOption = nullptr, *hideDeviceOptionsOption = nullptr;
    bool hideNativeStatus() const;
    QJsonObject preferences;
    QLabel *settingsError;
    kobalt::ui::ModalOverlay *busyNotice = nullptr;
    QTimer renderNoticeTimer;
    kobalt::ui::LightSwipeNotice *brightnessNotice = nullptr;
    bool raisingPopups = false;
    QPointer<kobalt::ui::ModalOverlay> debugDialog;
    QTimer brightnessHide;
    int brightnessInitial = 0, warmthInitial = 0;
    bool gestureWarmth = false, warmthAutomatic = false, warmthSupported = false;
    int pendingDarkMode = -1;
    bool applyingText = false, closing = false;
    kobalt::ChoiceScreen *choices;
    kobalt::ChapterGraph *chapterGraph = nullptr;
    bool pinStats = false, pinnedStatsOpen = false;
    kobalt::Acknowledgments *acknowledgments;
    QStackedWidget *panels;
    QLabel *progressLabel, *previewLabel;
    QPushButton *previousPage, *nextPage, *resetDefaults, *chapterButton;
    QStackedWidget *settingsPages;
    QVector<QPushButton *> rotationButtons;
    bool restoringDefaults = false, themeReady = false, darkTheme = false;
    kobalt::ui::FontButton *fontButton;
    QVector<QPushButton *> alignmentButtons, hyphenButtons, headerSizeButtons, footerSizeButtons;
    void syncChromeSizeButtons();
    kobalt::ui::SettingSlider *sizeSlider, *opticalSizeSlider, *weightSlider, *variableWeightSlider, *spacingSlider,
        *marginSlider, *verticalMarginSlider, *bookSlider;
    kobalt::reader::AppearanceEditSession appearanceEdit;
    QSize appearanceViewport;
    quint64 warmScheduledGeneration = 0;
    quint64 warmAttemptGeneration = 0;
    using AppearanceDestination = kobalt::reader::AppearanceEditSession::Destination;
    QVector<int> registeredFonts;
    QStringList pickerNativeFamilies;
    QSet<QString> registeredFontPaths;
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
    QPointer<kobalt::ui::ModalOverlay> fontDefaultDialog;
    QPushButton *linkReturn = nullptr;
    bool linkReturnAvailable = false;
    struct LinkVisit {
        QString position;
        int page;
        QString chapter;
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
    QCheckBox *linkIndicatorOption = nullptr, *hideNonlinearOption = nullptr;
    bool revealingLinks = false;
    QString revealedPosition;
    void syncLinkDiscovery();
    void showRevealedLinks();
    void leaveRevealedLinks();
    void hideRevealedLinks(bool restoreControls = true);
    void dismissLinkConfirmation();
    void dismissLinks();
    void updateLinkReturn();
    bool openLink(QPoint point);
    void goToPassage(const QString &target);
    kobalt::DictionaryPanel *dictionaryPanel = nullptr;
    std::unique_ptr<kobalt::DictionaryService> dictionary;
    QString lookupText;
    QRect lookupBounds;
    quint64 lookupEpoch = 0;
    bool lookupPending = false;
    void dismissLookup(bool keepGesture = false);
    void lookupWord(QPoint point, bool checkLink = true);
    void resizeText();
    void applyTheme(bool dark);
    void registerPickerFonts(const QString &family = {});
    QString bookFace();

    void startClock();
    void updateMenuStatus();
    void updateReadingStats();
    void configureNavigation();
    QRect panelBounds(int panelHeight) const;
    bool statsPinned() const;
    void syncNavigation();
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
    void applyHeavierText(bool enabled);
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
    // Exit Kobalt ignores taps from when the chapter list closes until a short
    // time after the reader has handled the close. A tap that waited in the
    // queue while the screen repainted still counts as part of the double tap.
    bool exitGuarded = false;
    int exitGuardGeneration = 0;
    void guardExit();
    // Full flashes clear the ghosting that partial page turns leave behind.
    // Turns since the last flash, for the Refresh the screen setting.
    int pagesSinceFlash = 0;
    kobalt::ScreenRefresh screenRefreshMode = kobalt::ScreenRefresh::Smart;
    ui::DescribedOption *bionicOption = nullptr;
    kobalt::ui::CaptionButton *refreshButton = nullptr;
    QPointer<kobalt::ui::ModalOverlay> refreshPopup;
    static QString refreshExplanation(kobalt::ScreenRefresh mode);
    static QString refreshLabel(kobalt::ScreenRefresh mode);
    void showScreenRefresh();
    void closeScreenRefresh();
    void openScreenRefresh();
    // Declared after book, so it is destroyed before the session it borrows.
    std::unique_ptr<kobalt::BookmarkSession> bookmarks;
    QString layoutKey() const;
    void syncBookmarks();
    kobalt::BookmarkList *bookmarkList = nullptr;
    QPointer<kobalt::ui::ModalOverlay> removeDialog;
    QPointer<kobalt::ui::ModalOverlay> bookmarkErrorDialog, settingsWriteErrorDialog;
    QPointer<kobalt::ui::ModalOverlay> formattingWarningDialog;
    QVector<kobalt::BookmarkRow> bookmarkRows() const;
    void openBookmarks();
    void askRemoveBookmark(const QString &position);
    void toggleBookmark();
    void showBookmarkError();
    void showFormattingWarning();
    bool flashAfterMenu = false;
    void requestFlash(const char *reason);
    QTimer *saveTimer = nullptr;
    bool turnPainted = false;
    QElapsedTimer unsavedSince;
    void showFinishChoice();
    // Like Nickel's reader, record the opening in Nickel's database once the
    // book is on screen: the position, the reading status and the time. Nickel
    // shows the most recently read book on the sleep screen and on Home.
    // Checking Nickel's progress at opening has already compared the times.
    bool recordedOpening = false;
    void recordOpening();
    void finishExit();
    void saveSoon();
    void save(bool snapshot = false);
};
}
