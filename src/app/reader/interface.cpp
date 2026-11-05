#include "app/reader_interface.h"
#include "app/reader_window.h"
#include "app/reader/views/opening.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "reading/header_footer.h"
#include "reading/reading_density.h"

namespace kobalt {
namespace {
QSize readingViewport(QSize screen, const QJsonObject &preferences)
{
    namespace reading = ui::metrics::reading;
    const auto chrome =
            headerfooter::select(preferences, isPhoneScreen(screen, ui::density())).layout;
    screen.rheight() -= (chrome.header() ? reading::stripHeight(chrome.headerSize) : 0)
            + (chrome.footer() ? reading::stripHeight(chrome.footerSize) : 0)
            + (chrome.progressBar ? reading::progressHeight : 0);
    return screen;
}
} // namespace

const ReaderInterface &readerInterface()
{
    static const ReaderInterface implementation{
        createReaderWindow,
        [](QWidget *background, const QJsonObject &preferences,
           PresentationFactory presentation) -> std::unique_ptr<ReaderOpeningView> {
            // The opening screen uses the first fonts, so choose their weight here.
            ui::setInterfaceWeight(ui::heavierTextSteps);
            return createOpening(background, preferences, std::move(presentation));
        },
        readingViewport, ui::setDensity, ui::setLargeText
    };
    return implementation;
}
} // namespace kobalt
