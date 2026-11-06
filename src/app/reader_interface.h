#pragma once
#include "reader_window.h"
#include "platform/api/reader_host_services.h"
#include <QDialog>
#include <QJsonObject>
#include <functional>

namespace kobalt {
// The controller owns publication access and loading. Each UI owns the widgets
// used to show that work, and the viewport it reserves for the book.
// Storage is a failure caused by saved data that cannot be read or moved aside.
enum class OpeningFallback { Failure, Protected, Remembered, Storage };

class ReaderOpeningView
{
public:
    virtual ~ReaderOpeningView() = default;
    virtual QDialog *window() const = 0;
    virtual void setProgress(int percent) = 0;
    virtual void setCover(QImage cover) = 0;
    // Decode an existing host cover without waiting for publication opening.
    virtual void loadCover(const QString &path) = 0;
    // Run after the first visible frame, before expensive opening work starts.
    virtual void afterFirstPaint(std::function<void()> callback) = 0;
    // Replace temporary system faces after the bundled interface fonts load.
    virtual void refreshFonts() = 0;
    // The controller owns authorization and the native continuation.
    virtual void showFallback(OpeningFallback state) = 0;
};

struct ReaderInterface
{
    ReaderWindow *(*createWindow)(std::unique_ptr<ReadingBook>, ReaderOrientation *,
                                  ReaderHostServices);
    std::unique_ptr<ReaderOpeningView> (*createOpening)(QWidget *background,
                                                        const QJsonObject &preferences,
                                                        PresentationFactory);
    QSize (*readingViewport)(QSize screen, const QJsonObject &preferences);
    // Sizes the interface for a screen of this many pixels per inch. Call before
    // creating any view.
    void (*setDensity)(int ppi);
    // Applies the platform's text size setting, where 1 is standard. Call
    // before creating any view.
    void (*setLargeText)(qreal requested);
};

const ReaderInterface &readerInterface();
} // namespace kobalt
