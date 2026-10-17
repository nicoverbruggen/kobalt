#include "interfaces/reader_interface.h"
#include "interfaces/reader_window.h"
#include "interfaces/reader/views/opening.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "reading/header_footer.h"

namespace kobalt {
namespace {
QSize readingViewport(QSize screen, const QJsonObject &preferences) {
    namespace m = ui::metrics::reading;
    const auto chrome = headerfooter::select(preferences).layout;
    screen.rheight() -= (chrome.header() ? m::stripHeight(chrome.headerSize) : 0) +
                        (chrome.footer() ? m::stripHeight(chrome.footerSize) : 0) +
                        (chrome.progressBar ? m::progressHeight : 0);
    return screen;
}
}
const ReaderInterface &readerInterface() {
    static const ReaderInterface implementation{
        createReaderWindow,
        [](QWidget *background, const QJsonObject &preferences,
           PresentationFactory presentation) -> std::unique_ptr<ReaderOpeningView> {
            // The opening screen uses the first fonts, so choose their weight here.
            ui::setInterfaceWeight(ui::heavierTextSteps);
            return createOpening(background, preferences, std::move(presentation));
        },
        readingViewport, ui::setDensity};
    return implementation;
}
}
