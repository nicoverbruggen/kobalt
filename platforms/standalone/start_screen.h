#pragma once
#include "app/reader_interface.h"
#include "platform/api/keyboard_controller.h"
#include <QStackedWidget>
#include <functional>

namespace kobalt::library {
class LibraryView;
class Store;
} // namespace kobalt::library

namespace kobalt::standalone {
class WidgetCanvas;

// Qt-only library and book-opening UI. The host supplies document and folder
// picking, file access and exit.
class StartScreen final : public QStackedWidget
{
    Q_OBJECT
public:
    // `textScale` reports the platform's text size setting, where 1 is
    // standard. The library takes it when it is built and whenever it is shown.
    StartScreen(PresentationFactory presentation, library::Store &store,
                std::function<qreal()> textScale = {}, QWidget *parent = nullptr);
    ~StartScreen() override;
    void showOpening(const QString &bookKey = {});
    void setProgress(int percent);
    void setCover(QImage cover);
    // Closes the library's topmost layer. False at the root library.
    bool dismissModal();
    void showLibrary();
    bool libraryShown() const;
    // Applies the platform's text size to the interface. A library built at
    // another size is built again, because views read sizes when they are
    // built. The rebuilt library starts on its first page, outside search.
    void applyTextSize();
    void setKeyboardFactory(KeyboardFactory factory);
    // Follows the reader's dark mode setting.
    void setDark(bool dark);
    library::LibraryView *library();
    WidgetCanvas *libraryCanvas();

    WidgetCanvas *openingCanvas() const { return openingSurface; }

    // Separate the library from Android's system bars; the reader stays fullscreen.
    void setLibrarySystemBarBorders(bool visible);
    // By default the library fills its canvas at the display's pixel ratio,
    // as on Android. A host that scales the reader itself, like the desktop
    // app, gives the library the same reader size and scale.
    // `topInset` keeps the library below a band the host shows instead, such
    // as a simulated status bar.
    void setLibraryScale(QSize pixels, qreal scale, bool nativePixels, qreal rasterScale,
                         int topInset = 0);
    // An opening error for a book chosen in the library, shown over the library.
    void showLibraryError(const QString &error);
    // On Android a book opened from the library on its own is Kobalt's copy,
    // which removal deletes. The confirmation says so.
    void setSingleBooksAreCopies(bool value);
signals:
    void exitRequested();
    // From the library: open one of its books, add a folder, or open and add
    // a single book.
    void bookRequested(QString key);
    void addFolderRequested();
    void libraryOpenRequested();
    void removeBookRequested(QString key);
    void markUnreadRequested(QString key);
    void removeFolderRequested(QString key);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    std::unique_ptr<ReaderOpeningView> opening;
    PresentationFactory presentation;
    library::Store &store;
    WidgetCanvas *canvas = nullptr, *openingSurface = nullptr;
    library::LibraryView *view = nullptr;
    std::function<qreal()> textScale;
    KeyboardFactory keyboard;
    // The text scale the library was built with.
    qreal builtTextScale = 1;
    bool dark = false, hostScaled = false, singleBooksAreCopies = false;

    struct Scale
    {
        QSize pixels;
        qreal scale = 1, rasterScale = 1;
        bool nativePixels = false;
        int topInset = 0;
    } scale;

    void buildLibrary();
    void fitLibrary();
};
} // namespace kobalt::standalone
