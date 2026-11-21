#include "start_screen.h"
#include "app/reader/settings/interface_style.h"
#include "assets/font_catalog.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "app/library/library_view.h"
#include "library/library_store.h"
#include "canvas.h"
#include "app/reader/views/opening.h"
#include "ui/style/metrics.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace kobalt::standalone {
StartScreen::StartScreen(PresentationFactory presentation, library::Store &store,
                         std::function<qreal()> textScale, QWidget *parent)
    : QStackedWidget(parent),
      presentation(std::move(presentation)),
      store(store),
      textScale(std::move(textScale))
{
    setObjectName("androidStart");
    qApp->installEventFilter(this);
    showLibrary();
}

bool StartScreen::dismissModal()
{
    return libraryShown() && view->dismiss();
}

void StartScreen::buildLibrary()
{
    if (view)
        return;
    // The library uses the reader's controls, which are sized in device
    // pixels. Its canvas maps them onto this logical-pixel window.
    if (!canvas) {
        canvas = new WidgetCanvas(this);
        canvas->setObjectName("androidLibraryCanvas");
        canvas->setStyleSheet("QGraphicsView{border:0;}");
        canvas->viewportResized = [this] { fitLibrary(); };
        addWidget(canvas);
    }
    // Views read sizes while they are built; see applyTextSize.
    builtTextScale = ui::textScale();
    view = new library::LibraryView(store.index(),
                                    [this](const QString &name) { return store.coverPath(name); });
    // Complete the subtree before embedding it; proxy resize can disable new children.
    canvas->setContent(view);
    view->openBook = [this](const QString &key) { emit bookRequested(key); };
    view->openPassage = [this](const QString &key, const QString &position) {
        emit passageRequested(key, position);
    };
    view->setWordsDirectory(store.profileDirectory());
    view->addFolder = [this] { emit addFolderRequested(); };
    view->openFile = [this] { emit libraryOpenRequested(); };
    view->rescan = [this] { store.scan(); };
    view->removeFolder = [this](const QString &key) { emit removeFolderRequested(key); };
    view->removeBook = [this](const QString &key) { emit removeBookRequested(key); };
    view->markBook = [this](const QString &key, library::Book::Mark mark) {
        if (mark == library::Book::Mark::Unread)
            emit markUnreadRequested(key);
        else
            store.setMark(key, mark);
    };
    view->settingsChanged = [this](const library::Settings &settings) {
        store.setSettings(settings);
    };
    store.changed = [this] {
        view->setScanning(store.scanning(), store.found());
        view->refresh();
    };
    view->singleBooksAreCopies = singleBooksAreCopies;
    view->resourceFolderLabels = resourceFolderLabels;
    view->chooseResourceFolder = chooseResourceFolder;
    view->setKeyboardFactory(keyboard);
    view->setProperty("kobalt_icon_style", property("kobalt_icon_style"));
    view->setProperty("kobalt_palette", property("kobalt_palette"));
    view->setDark(dark);
    view->show();
    if (hostScaled)
        setLibraryScale(scale.pixels, scale.scale, scale.nativePixels, scale.rasterScale,
                        scale.topInset);
    else
        fitLibrary();
}

void StartScreen::applyTextSize()
{
    ui::setLargeText(textScale ? textScale() : 1);
    if (!view || ui::textScale() == builtTextScale)
        return;
    // The old view can be running one of its own callbacks, such as a menu
    // action that reports an error, so it is deleted later. Hiding it first
    // also hides its keyboard.
    const auto preferences = view->readerPreferences();
    view->hide();
    if (auto *proxy = view->graphicsProxyWidget()) {
        canvas->scene()->removeItem(proxy);
        proxy->deleteLater();
    } else {
        view->deleteLater();
    }
    view = nullptr;
    buildLibrary();
    view->setReaderPreferences(preferences);
}

void StartScreen::setResourceFolders(std::function<QStringList()> labels,
                                     std::function<void(int, bool)> choose)
{
    resourceFolderLabels = std::move(labels);
    chooseResourceFolder = std::move(choose);
    if (view) {
        view->resourceFolderLabels = resourceFolderLabels;
        view->chooseResourceFolder = chooseResourceFolder;
    }
}

void StartScreen::setKeyboardFactory(KeyboardFactory factory)
{
    keyboard = std::move(factory);
    if (view)
        view->setKeyboardFactory(keyboard);
}

library::LibraryView *StartScreen::library()
{
    buildLibrary();
    return view;
}

WidgetCanvas *StartScreen::libraryCanvas()
{
    buildLibrary();
    return canvas;
}

void StartScreen::setLibrarySystemBarBorders(bool visible)
{
    const int width = visible
            ? qMax(1, qRound(int(ui::metrics::control::border) / canvas->devicePixelRatioF()))
            : 0;
    ui::readerStyle(
            canvas,
            QString("QGraphicsView{border:0;border-top:%1px solid palette(mid);border-bottom:%1px solid palette(mid);}")
                    .arg(width));
    ui::applyReaderPalette(canvas, view->palette());
}

void StartScreen::showLibrary()
{
    // The setting can change while a book is open or the app is in the
    // background. The first library is built at the current setting too.
    applyTextSize();
    buildLibrary();
    setCurrentWidget(canvas);
    releaseOpening();
    fitLibrary();
    view->setScanning(store.scanning(), store.found());
    view->refresh();
    if (!store.available())
        view->showNotice(strings::get("library.open_failed"),
                         strings::get("error.saved_data_unavailable"));
}

void StartScreen::showLibraryError(const QString &error)
{
    showLibrary();
    view->showNotice(strings::get("library.open_failed"), error);
}

void StartScreen::setSingleBooksAreCopies(bool value)
{
    singleBooksAreCopies = value;
    if (view)
        view->singleBooksAreCopies = value;
}

bool StartScreen::libraryShown() const
{
    return canvas && currentWidget() == canvas;
}

void StartScreen::applyLibraryTheme(const QJsonObject &preferences, const FontSources &sources)
{
    view->setReaderPreferences(preferences);
    const auto style = reader::savedInterfaceStyle(preferences);
    QString mainFace, monoFace;
    const auto mono = preferences.value("ui_mono_font_face").toString();
    if (!style.mainFace.isEmpty() || !mono.isEmpty()) {
        for (const auto &font : discoverReadingFonts(sources)) {
            // A family too large to stay in memory is not an interface font.
            if ((font.family != style.mainFace && font.family != mono) || !fitsInterfaceFont(font))
                continue;
            for (const auto &file : font.files)
                ui::registerPrivateInterfaceFont(file, font.family);
            if (font.family == style.mainFace)
                mainFace = font.family;
            if (font.family == mono)
                monoFace = font.family;
        }
    }
    ui::keepPrivateInterfaceFamilies({ mainFace, monoFace });
    ui::setInterfaceFontFamily(mainFace);
    ui::setInterfaceMonoFontFamily(monoFace);
    setProperty("kobalt_icon_style", int(style.icons));
    view->setProperty("kobalt_icon_style", int(style.icons));
    view->setFont(ui::interfaceFont());
    setDark(preferences.value("dark_mode").toBool(), style.palette);
}

void StartScreen::setDark(bool value, const QString &paletteId)
{
    dark = value;
    setProperty("kobalt_palette", paletteId);
    setProperty("reader_dark", dark);
    ui::applyReaderPalette(this, ui::readerPalette(palette(), dark, paletteId));
    if (view) {
        view->setProperty("kobalt_palette", paletteId);
        view->setDark(dark);
    }
    if (canvas) {
        canvas->setProperty("reader_dark", dark);
        ui::applyReaderPalette(canvas, view->palette());
    }
}

void StartScreen::fitLibrary()
{
    if (canvas && !hostScaled)
        canvas->fitContent();
}

void StartScreen::setLibraryScale(QSize pixels, qreal factor, bool nativePixels, qreal rasterScale,
                                  int topInset)
{
    hostScaled = true;
    scale = { pixels, factor, rasterScale, nativePixels, topInset };
    if (!view)
        return;
    view->setGeometry(0, topInset, pixels.width(), pixels.height() - topInset);
    canvas->setNativePixels(nativePixels);
    canvas->setRasterScale(rasterScale);
    canvas->setTransform(QTransform::fromScale(factor, factor));
    canvas->setSceneRect(QRectF(QPointF(), pixels));
}

void StartScreen::resizeEvent(QResizeEvent *event)
{
    QStackedWidget::resizeEvent(event);
    fitLibrary();
    if (openingSurface)
        openingSurface->fitContent();
}

bool StartScreen::eventFilter(QObject *object, QEvent *event)
{
    if (view && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        auto *widget = qobject_cast<QWidget *>(object);
        if (widget && (widget == view || view->isAncestorOf(widget)) && !libraryRepairPending) {
            libraryRepairPending = true;
            QTimer::singleShot(0, this, [this] {
                libraryRepairPending = false;
                if (!view)
                    return;
                // As in the reader host, proxy resize can disable painting on
                // newly created children. Preserve deliberate update disables.
                for (auto *child : view->findChildren<QWidget *>())
                    if (!child->updatesEnabled()
                        && !child->testAttribute(Qt::WA_ForceUpdatesDisabled)
                        && child->parentWidget() && child->parentWidget()->updatesEnabled())
                        child->setUpdatesEnabled(true);
            });
        }
    }
    return QStackedWidget::eventFilter(object, event);
}

StartScreen::~StartScreen()
{
    qApp->removeEventFilter(this);
    store.changed = {};
}

void StartScreen::showOpening(const QString &bookKey, bool showCover)
{
    releaseOpening();
    openingSurface = new WidgetCanvas(this);
    openingSurface->setObjectName("androidOpeningCanvas");
    // A parentless QDialog can become a native Android window before proxy
    // embedding, applying Android's pixel ratio to the scene dimensions again.
    auto *content = new QWidget;
    content->setProperty("reader_dark", dark);
    auto *body = new QVBoxLayout(content);
    body->setContentsMargins(0, 0, 0, 0);
    opening = createEmbeddedOpening(content, presentation, showCover);
    opening->loadCover(store.coverPath(store.index().books.value(bookKey).cover));
    connect(opening->window(), &QDialog::rejected, this, &StartScreen::exitRequested);
    body->addWidget(opening->window());
    openingSurface->setContent(content);
    openingSurface->viewportResized = [this] { openingSurface->fitContent(); };
    addWidget(openingSurface);
    setCurrentWidget(openingSurface);
    openingSurface->fitContent();
}

void StartScreen::releaseOpening()
{
    if (!openingSurface)
        return;
    removeWidget(openingSurface);
    opening.reset();
    delete openingSurface;
    openingSurface = nullptr;
}

void StartScreen::setCover(QImage cover)
{
    if (opening && currentWidget() == openingSurface)
        opening->setCover(std::move(cover));
}

void StartScreen::setReopening()
{
    if (opening && currentWidget() == openingSurface)
        opening->setReopening();
}

void StartScreen::setProgress(int percent)
{
    if (opening && currentWidget() == openingSurface)
        opening->setProgress(percent);
}
} // namespace kobalt::standalone
