#include "support/diagnostics.h"
#include "reading/reading_density.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/controls/rows.h"
#include "ui/controls/options.h"
#include "ui/controls/sliders.h"
#include "ui/controls/page_scrubber.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/dialogs/modals.h"
#include "ui/dialogs/action_menu.h"
#include "ui/style/theme.h"
#include "assets/assets.h"
#include "assets/font_catalog.h"
#include "reader_inputs.h"
#include "app/reader/views/opening.h"
#include "app/reader/views/appearance_preview.h"
#include "app/reader/settings/settings_panel.h"
#include "host_presentation.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "ui/style/fonts.h"
#include "app/reader/window/font_registration.h"
#include <QFontInfo>
#include <QFontDatabase>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include "storage/reader_storage.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/controls/paged_text.h"
#include "app/reader/panels/dictionary_panel.h"
#include "app/reader/settings/dictionaries_page.h"
#include "app/reader/panels/link_panel.h"
#include "platforms/kobo/common/browser_handoff.h"
#include "platforms/kobo/display/keyboard_refresh.h"
#include "app/reader/panels/chapter_graph.h"
#include "app/reader/panels/choice_screen.h"
#include "app/library/library_view.h"
#include <QScrollBar>
#include <QApplication>
#include <QVariant>
#include <QMouseEvent>
#include <QPointer>
#include <QPaintEvent>
#include <QPainter>
#include <QAbstractTextDocumentLayout>
#include <QTextLayout>
#include <QTextBlock>
#include <QtMath>
#include <QKeyEvent>
#include <QProgressBar>
#include <QTouchEvent>
#include <QWindow>
#include <private/qeventpoint_p.h>
#include <QRawFont>
#include <cstdio>
#include <cstdlib>
#include "check.h"
#include "ui_private_text.h"

class PaintCountingButton : public kobalt::ui::Button
{
public:
    using Button::Button;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ++paints;
        Button::paintEvent(event);
    }
};

class PaintCountingCaptionButton : public kobalt::ui::CaptionButton
{
public:
    using CaptionButton::CaptionButton;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ++paints;
        CaptionButton::paintEvent(event);
    }
};

class PaintCountingOption : public kobalt::ui::DescribedOption
{
public:
    using DescribedOption::DescribedOption;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ++paints;
        DescribedOption::paintEvent(event);
    }
};

class PaintCountingListRow : public kobalt::ui::ListRow
{
public:
    using ListRow::ListRow;
    int paints = 0;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ++paints;
        ListRow::paintEvent(event);
    }
};

class StyleCountingObserver : public QObject
{
public:
    int changes = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::StyleChange)
            ++changes;
        return false;
    }
};

class PaintCountingObserver : public QObject
{
public:
    int paints = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            ++paints;
        return false;
    }
};

class PaintCountingPanel : public kobalt::ui::Panel
{
public:
    QRegion painted;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        painted += event->region();
        Panel::paintEvent(event);
    }
};

// Checks the production controls in pixels: geometry, borders, themes, labels, sliders, dictionary rendering and paging.
// Releases dismissed content while preserving fonts, temporary hides and fresh reopening.
void dismissedContent()
{
    using namespace kobalt;
    QWidget host;
    host.resize(1072, 1448);
    host.show();
    QImage image(600, 800, QImage::Format_RGB32);
    image.fill(Qt::white);
    reader::AppearancePreview preview(&host);
    preview.resize(400, 600);
    {
        reader::AppearancePreview::Content content;
        content.page = image.size();
        content.bodyHeight = image.height();
        content.body = image;
        content.header = image;
        content.footer = image;
        preview.setContent(content);
    }
    preview.show();
    preview.grab();
    require(!image.isDetached(), "Appearance preview did not retain its test image");
    preview.hide();
    preview.clear();
    require(image.isDetached(), "Dismissed appearance preview retained its image");

    reader::SettingsPanel settings(&host, {});
    settings.show();
    {
        StyleSample sample;
        sample.spreads = { { image, image } };
        sample.crops = { image };
        sample.details = { image };
        settings.setStylingSample(reader::SettingsPanel::AdvancedText::WordSpacing, sample);
    }
    require(!image.isDetached(), "Styling did not retain its test sample");
    settings.hide();
    require(image.isDetached(), "Closed Settings retained Styling sample images");

    ui::PrivateRichText rich;
    const auto font = ui::interfaceFont(28);
    rich.setFont(font);
    const QString html =
            QStringLiteral("<p>A long definition with several lines of text.</p>").repeated(80);
    rich.setHtml(html);
    rich.layout(300, 200);
    require(rich.pages() > 1 && !rich.lineRectangles().isEmpty(), "Rich text fixture is empty");
    rich.clear();
    require(rich.lineRectangles().isEmpty() && rich.height() == 0 && rich.font() == font,
            "Rich text clear retained layout or discarded its font");
    rich.setHtml(html);
    rich.layout(300, 200);
    require(rich.pages() > 1, "Cleared rich text cannot lay out new content");

    DictionaryPanel dictionary(&host);
    DictionaryResult result;
    result.received = result.restored = true;
    result.html = html;
    dictionary.open("word", result, QRect(100, 100, 30, 30));
    auto *definition =
            static_cast<ui::PagedText *>(dictionary.findChild<QWidget *>("rrDefinition"));
    require(definition && definition->pages() > 1, "Dictionary fixture is not paginated");
    const int definitionPages = definition->pages();
    definition->setPage(1);
    host.hide();
    host.show();
    require(definition->pages() == definitionPages && definition->page() == 1,
            "Temporary reader hide discarded its dictionary");
    dictionary.dismiss();
    require(definition->pages() == 1 && definition->page() == 0,
            "Dismissed dictionary retained its definition");
    dictionary.open("word", result, QRect(100, 100, 30, 30));
    require(definition->pages() == definitionPages, "Dictionary failed to reopen the same content");
    host.hide();
    dictionary.dismiss();
    host.show();
    require(definition->pages() == 1, "Dictionary dismissal while the reader hid retained text");

    LinkPanel link(&host);
    LinkDetails details;
    details.kind = LinkKind::Footnote;
    details.html = html;
    link.open(details, QRect(100, 100, 30, 30));
    auto *note = static_cast<ui::PagedText *>(link.findChild<QWidget *>("rrLinkText"));
    require(note && note->pages() > 1, "Footnote fixture is not paginated");
    const int notePages = note->pages();
    host.hide();
    host.show();
    require(note->pages() == notePages, "Temporary reader hide discarded its footnote");
    link.dismiss();
    require(note->pages() == 1 && note->page() == 0, "Dismissed footnote retained its text");
    link.open(details, QRect(100, 100, 30, 30));
    require(note->pages() == notePages, "Footnote failed to reopen the same content");
}

int main(int argc, char **argv)
{
#ifdef Q_OS_LINUX
    // Noto Sans Math may be installed system-wide. This fixture must begin
    // without it in Qt, then prove the private face is registered on demand.
    // The configuration has no font folders. It keeps fontconfig's rule that
    // names sans-serif as the default family: without a default family, Qt's
    // fixed system font has no family, and rich text with <pre> crashes.
    QTemporaryDir fontEnvironment;
    require(fontEnvironment.isValid(), "Cannot isolate the component font environment");
    const auto fontConfig = fontEnvironment.filePath("fonts.conf");
    QFile config(fontConfig);
    require(config.open(QIODevice::WriteOnly), "Cannot create the component font configuration");
    require(config.write(
                    "<fontconfig><match target=\"pattern\">"
                    "<test qual=\"all\" name=\"family\" compare=\"not_eq\"><string>sans-serif</string></test>"
                    "<test qual=\"all\" name=\"family\" compare=\"not_eq\"><string>serif</string></test>"
                    "<test qual=\"all\" name=\"family\" compare=\"not_eq\"><string>monospace</string></test>"
                    "<edit name=\"family\" mode=\"append_last\"><string>sans-serif</string></edit>"
                    "</match></fontconfig>\n")
                    > 0,
            "Cannot write the component font configuration");
    config.close();
    qputenv("FONTCONFIG_FILE", fontConfig.toUtf8());
#endif
    QApplication app(argc, argv);
    QTemporaryDir diagnosticDirectory;
    require(diagnosticDirectory.isValid(), "Cannot create font diagnostic directory");
    kobalt::initializeDiagnostics(diagnosticDirectory.path());
    {
        // The loading view is built before the whole bundle is verified. A
        // font used before then must not stop the faces registering later.
        const auto early = kobalt::ui::interfaceFont();
        Q_UNUSED(early);
        kobalt::AssetBundle opening(kobalt::test::readerAssets().root(),
                                    QStringLiteral(KOBALT_VERSION));
        require(kobalt::ui::initializeOpeningFonts(opening),
                "Cannot load the opening view's fonts");
        require(QFontInfo(kobalt::ui::interfaceFont()).family() == "IBM Plex Sans"
                        && QFontInfo(kobalt::ui::brandFont(32)).family() == "Instrument Serif",
                "Opening view does not draw in the interface fonts");
    }
    require(kobalt::test::readerAssets().available(),
            "Component assets failed manifest verification");
    bool facesChanged = true;
    require(kobalt::ui::initializeInterfaceFonts(kobalt::test::readerAssets(), &facesChanged),
            "Cannot load verified UI font bytes");
    require(!facesChanged,
            "Full font initialization replaced the opening faces and repaints the loading view");
    require(QFontInfo(kobalt::ui::interfaceFont()).family() == "IBM Plex Sans"
                    && QFontInfo(kobalt::ui::chromeFont()).family() == "IBM Plex Mono"
                    && QFontInfo(kobalt::ui::brandFont(32)).family() == "Instrument Serif",
            "Component font substitution");
    dismissedContent();
    {
        int loads = 0;
        kobalt::setDiagnosticObserver([&](const QJsonObject &event) {
            if (event.value("event").toString() == "font-native-fallback-load")
                ++loads;
        });
        QLineEdit editor;
        editor.setFont(kobalt::ui::interfaceFont(24));
        kobalt::ui::prepareNativeEditor(&editor);
        editor.setText("Latin text");
        editor.resize(400, 80);
        QPixmap image(editor.size());
        editor.render(&image);
        require(loads == 0, "Latin editor loaded script fallback fonts");
        editor.setText(QString::fromUtf8("مرحبا"));
        editor.render(&image);
        require(loads == 1, "Arabic editor did not load exactly its needed fallback");
        editor.render(&image);
        require(loads == 1, "Unchanged editor registered its fallback again");
        require(kobalt::ui::initializeInterfaceFonts(kobalt::test::readerAssets()),
                "Cannot reset editor fonts");
        editor.render(&image);
        require(loads == 2, "Existing editor lost its fallback after asset reinitialization");
        kobalt::setDiagnosticObserver({});
    }
    checkPrivateInterfaceText(kobalt::test::readerAssets());
    {
        using namespace kobalt::ui;
        const auto &assets = kobalt::test::readerAssets();
        const QStringList files{ assets.file("fonts/IBMPlexSans-Regular.ttf") };
        renderPrivateText(QStringLiteral("Font Preview"), chromeFont(32));
        const auto retained = privateInterfaceFontBytes();
        const FontPreviewScale scale{ 30, 34, 8, 192, 5 };
        const auto regular = renderFontPreviewBitmap(files, "IBM Plex Sans", "Font Preview",
                                                     QSize(400, 100), scale, 0, false, true, false);
        const auto heavy = renderFontPreviewBitmap(files, "IBM Plex Sans", "Font Preview",
                                                   QSize(400, 100), scale, 10, false, true, false);
        require(!regular.isNull() && regular.format() == QImage::Format_Indexed8
                        && regular != heavy,
                "Bitmap preview lost the selected font or weight");
        require(privateInterfaceFontBytes() == retained,
                "Isolated preview populated the GUI font cache");
        require(renderFontPreviewBitmap(files, "IBM Plex Sans", "Too large", QSize(4096, 4096),
                                        scale, 0, false, true, false)
                        .isNull(),
                "Preview accepted an unbounded bitmap");
        // A script font names itself in Latin letters it lacks. The preview
        // must use the bundled fallback, as the interface renderer does,
        // and must not cache an empty bitmap.
        const QStringList arabic{ assets.file("fonts/NotoNaskhArabic-Regular.ttf") };
        const auto inked = [](const QImage &image) {
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.scanLine(y)[x] >= 128)
                        return true;
            return false;
        };
        const auto fallbacks = privateInterfaceFontFiles();
        require(fallbacks.contains("NotoSerif-Regular.ttf"),
                "Preview workers lost the bundled fallback files");
        const auto named =
                renderFontPreviewBitmap(arabic, "Noto Naskh Arabic", "Noto Naskh Arabic",
                                        QSize(400, 100), scale, 0, false, true, false, fallbacks);
        require(!named.isNull() && inked(named),
                "Script font preview drew its Latin name without a fallback");
        require(renderFontPreviewBitmap(arabic, "Noto Naskh Arabic", "Noto Naskh Arabic",
                                        QSize(400, 100), scale, 0, false, true, false)
                        .isNull(),
                "Preview without fallbacks hid missing glyphs");
        require(privateInterfaceFontBytes() == retained,
                "Preview fallbacks populated the GUI font cache");
        // Labels render their samples while painting, so a page shows every
        // sample in its first paint. Count renders through the diagnostics.
        int renders = 0;
        kobalt::setDiagnosticObserver([&](const QJsonObject &event) {
            if (event.value("event").toString() == "font-preview-bitmap")
                ++renders;
        });
        SampleLabel label;
        label.optical = true;
        label.resize(500, 160);
        label.setFont(chromeFont(32));
        label.setText("Font Preview");
        const auto plain = label.grab().toImage();
        label.setPreviewSource(fontPreviewSource("IBM Plex Sans", files));
        const auto first = label.grab().toImage();
        require(renders == 1 && first != plain, "Font sample did not render in its first paint");
        require(fontPreviewCacheBytes() >= label.width() * label.height() + 1024,
                "Bitmap cache accounting omitted the coverage palette");
        require(label.grab().toImage() == first && renders == 1, "A cached sample rendered again");
        label.setAppearance({ 8, "off" });
        require(label.grab().toImage() != first && renders == 2, "Weight reused a stale bitmap");
        // More distinct labels than fit in the cache must evict old masks.
        for (int i = 0; i < 70; ++i) {
            label.setText(QString("Preview %1").arg(i));
            label.grab();
            require(fontPreviewCacheBytes() <= fontPreviewBitmapBudget,
                    "Bitmap cache exceeded its budget");
        }
        require(privateInterfaceFontBytes() == retained, "Browsing bitmaps retained font files");
        // A font that cannot render keeps the ordinary label and does not
        // reopen its files on every paint.
        label.setText("Font Preview");
        label.setAppearance({});
        label.setPreviewSource(fontPreviewSource("Broken", { assets.file("fonts/missing.ttf") }));
        const int before = renders;
        const auto broken = label.grab().toImage();
        label.grab();
        require(renders == before + 1, "A failed sample rendered on every paint");
        label.setPreviewSource({});
        require(broken == label.grab().toImage(),
                "A failed sample did not keep the ordinary label");
        kobalt::setDiagnosticObserver({});
    }

    {
        using namespace kobalt::ui;
        const int savedWeight = interfaceWeight();
        const int savedDensity = density();
        for (int ppi : { 212, 300 }) {
            setDensity(ppi);
            for (bool dark : { false, true })
                for (const auto &font :
                     { interfaceFont(px(40)), chromeFont(px(40)), brandFont(px(40)) }) {
                    const auto render = [&](int weight, bool sample, int sampleWeight = 0) {
                        setInterfaceWeight(weight);
                        QImage image(px(600), px(100), QImage::Format_RGB32);
                        image.fill(dark ? Qt::black : Qt::white);
                        QPainter painter(&image);
                        painter.setFont(font);
                        painter.setPen(dark ? Qt::white : Qt::black);
                        if (sample)
                            drawOpticalText(painter, image.rect(), "Exit Kobalt", false,
                                            { sampleWeight, QStringLiteral("off") });
                        else
                            drawOpticalText(painter, image.rect(), "Exit Kobalt");
                        painter.end();
                        return image;
                    };
                    const auto regular = render(0, false);
                    const auto heavier = render(heavierTextSteps, false);
                    const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
                    if (!output.isEmpty()) {
                        require(QDir().mkpath(output),
                                "Cannot create interface text capture directory");
                        require(heavier.save(output
                                             + QString("/text-%1-%2-%3.png")
                                                       .arg(font.family())
                                                       .arg(ppi)
                                                       .arg(dark ? "dark" : "light")),
                                "Cannot save interface text capture");
                    }
                    require(regular != heavier,
                            "Optical UI labels ignored the default interface weight");
                    require(heavier == render(heavierTextSteps, true, heavierTextSteps),
                            "Optical UI labels used a different weight from interface labels");
                    for (int weight : { 0, 4 })
                        require(render(0, true, weight) == render(heavierTextSteps, true, weight),
                                "Font samples inherited the interface weight instead of their explicit weight");
                }
        }
        setDensity(savedDensity);
        setInterfaceWeight(savedWeight);
    }
    {
        using namespace kobalt::ui;
        QWidget host;
        Button button("Theme", &host);
        LightSwipeNotice notice(&host);
        auto *caption = notice.findChild<QLabel *>("rrLightSwipeCaption");
        require(caption, "Swipe notice has no caption");
        // Replay the same widgets in both directions. Platform gray roles must
        // not leak into either QSS or custom painting after a theme change.
        for (bool dark : { false, true, false, true }) {
            host.setProperty("reader_dark", dark);
            const auto &theme = themeColors(dark);
            QPalette base;
            for (const auto group : { QPalette::Active, QPalette::Inactive, QPalette::Disabled })
                for (const auto role :
                     { QPalette::Light, QPalette::Dark, QPalette::Mid, QPalette::Midlight })
                    base.setColor(group, role, Qt::magenta);
            const auto colors = readerPalette(base, dark);
            applyReaderPalette(&host, colors);
            readerStyle(&button, "color:palette(window-text);background:palette(window);");
            applyReaderPalette(&button, colors);
            for (const auto &entry : { qMakePair(QPalette::Window, theme.paper),
                                       qMakePair(QPalette::WindowText, theme.ink),
                                       qMakePair(QPalette::Highlight, theme.ink),
                                       qMakePair(QPalette::HighlightedText, theme.paper),
                                       qMakePair(QPalette::Mid, theme.disabledInk),
                                       qMakePair(QPalette::Midlight, theme.pressedPaper),
                                       qMakePair(QPalette::Light, theme.divider),
                                       qMakePair(QPalette::Dark, theme.outline) })
                for (const auto group :
                     { QPalette::Active, QPalette::Inactive, QPalette::Disabled })
                    require(button.palette().color(group, entry.first) == entry.second,
                            "Styled widget retained a platform color instead of the fixed theme");
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
            require(button.palette().color(QPalette::PlaceholderText) == theme.secondaryInk,
                    "Placeholder text retained the platform color");
#endif
            for (const auto &entry :
                 { qMakePair("window-text", theme.ink), qMakePair("window", theme.paper),
                   qMakePair("mid", theme.disabledInk), qMakePair("midlight", theme.pressedPaper),
                   qMakePair("light", theme.divider), qMakePair("dark", theme.outline),
                   qMakePair("placeholder-text", theme.secondaryInk) }) {
                QLabel sample(&host);
                readerStyle(&sample, QString("color:palette(%1);").arg(entry.first));
                require(sample.styleSheet().contains("color:" + entry.second.name() + ';'),
                        "Stylesheet and theme colors disagree");
            }
            applyReaderPalette(caption, colors);
            require(caption->styleSheet().contains("color:" + theme.swipeCaptionInk.name() + ';'),
                    "Theme replay left the swipe caption in its previous theme");
            require(theme.swipeCaptionInk == QColor(dark ? "#aaaaaa" : "#666666")
                            && theme.lightHintPaper == QColor(dark ? "#222222" : "#e6e6e6")
                            && theme.sleepPaper == QColor(dark ? "#202020" : "#ffffff")
                            && theme.overviewPaper == QColor(dark ? "#242424" : "#f1f1f1")
                            && theme.openingTrack == QColor(dark ? "#2c2c2c" : "#d3d3d3")
                            && theme.modalFade.alpha() == 90
                            && theme.modalPattern
                                    == QColor(dark ? 170 : 85, dark ? 170 : 85, dark ? 170 : 85,
                                              150)
                            && theme.sleepDim == QColor(0, 0, 0, 40),
                    "Theme flattened an intentional surface or caption color");
        }
        // Reader sheets are cached, so the same source must follow a new
        // interface font as well as a new theme.
        QLabel sample(&host);
        for (const auto &family : { QString(), QStringLiteral("Kobalt Test Face"), QString() }) {
            setInterfaceFontFamily(family);
            readerStyle(&sample, "font-family:\"@ui-font-family@\";");
            require(sample.styleSheet().contains("font-family:\"" + interfaceFont().family()
                                                 + "\";"),
                    "A cached reader sheet kept the previous interface font");
        }
    }
    {
        QWidget host;
        host.resize(1264, 1680);
        kobalt::ui::LightSwipeNotice notice(&host);
        notice.centerIn(host.size());
        notice.show();
        require(notice.testAttribute(Qt::WA_TransparentForMouseEvents),
                "Light indicator intercepts swipe input");
        auto *value = notice.findChild<QLabel *>("rrLightSwipeValue");
        auto *caption = notice.findChild<QLabel *>("rrLightSwipeCaption");
        for (bool warm : { false, true })
            for (int percent : { 0, 30, 50, 80, 100 }) {
                notice.setLevel(warm, percent);
                require(value->text() == QString::number(percent) + '%',
                        "Light indicator percentage differs from the service");
                require(caption->text()
                                == kobalt::strings::get(warm ? "light_panel.warmth"
                                                             : "light_panel.brightness")
                                           .toUpper(),
                        "Light indicator names the wrong control");
                require(notice.rect().contains(value->geometry())
                                && notice.rect().contains(caption->geometry()),
                        "Light indicator labels exceed the card");
                const auto image = notice.grab().toImage();
                const int middleX = image.width() / 2, middleY = image.height() / 2;
                const auto ink = notice.palette().color(QPalette::WindowText);
                for (int offset = 0; offset < int(kobalt::ui::metrics::chrome::heavyRule); ++offset)
                    require(image.pixel(offset, middleY) == ink.rgba()
                                    && image.pixel(image.width() - 1 - offset, middleY)
                                            == ink.rgba()
                                    && image.pixel(middleX, offset) == ink.rgba()
                                    && image.pixel(middleX, image.height() - 1 - offset)
                                            == ink.rgba(),
                            "Light indicator is missing its heavy border on an edge");
            }
        notice.setLevel(false, -1);
        require(value->text() == "0%", "Light indicator does not clamp low values");
        notice.setLevel(true, 120);
        require(value->text() == "100%", "Light indicator does not clamp high values");
        notice.centerIn(QSize(1680, 1264));
        require(notice.geometry().center() == QRect(0, 0, 1680, 1264).center(),
                "Light indicator is off center in landscape");
    }
    {
        QWidget host;
        host.resize(600, 800);
        kobalt::ui::PagerBar pager(&host, "test");
        for (int count : { 0, 1 }) {
            pager.setPage(0, count, {}, true);
            require(!pager.previous->isEnabled() && !pager.next->isEnabled(),
                    "Empty or single-page list enabled a turn");
            require(pager.turnTarget(-1) == 0 && pager.turnTarget(1) == 0,
                    "Empty or single-page list changed page");
        }
        pager.setPage(0, 3, {}, true);
        require(pager.previous->isEnabled() && pager.turnTarget(-1) == 2,
                "List previous did not wrap");
        pager.setPage(2, 3, {}, true);
        require(pager.next->isEnabled() && pager.turnTarget(1) == 0, "List next did not wrap");
        pager.setPage(2, 3);
        require(!pager.next->isEnabled() && pager.turnTarget(1) == 2,
                "Bounded content pager wrapped");
        auto *hint = host.findChild<QLabel *>("testContinuationHint");
        require(hint, "Paged list has no continuation hint");
        pager.setPage(0, 3);
        pager.setBlankArea({ 0, 500, 600, 100 });
        hint->ensurePolished();
        require(!hint->isHidden() && hint->font().italic(),
                "Paged list lost its italic continuation hint");
        require(hint->geometry() == QRect(0, 500, 600, 100),
                "Continuation hint escaped the blank area");
        require(hint->testAttribute(Qt::WA_TransparentForMouseEvents),
                "Continuation hint intercepts list taps");
        pager.setBlankArea({ 0, 500, 600, 1 });
        require(hint->isHidden(), "Continuation hint appeared in a short gap");
        pager.setBlankArea({ 0, 500, 1, 100 });
        require(hint->isHidden(), "Continuation hint appeared in a narrow gap");
        pager.setPage(2, 3, {}, true);
        pager.setBlankArea({ 0, 500, 600, 100 });
        require(hint->isHidden(), "Last list page promised more content");
        pager.setPage(0, 1);
        pager.setBlankArea({ 0, 500, 600, 100 });
        require(hint->isHidden(), "Single-page list promised more content");
        pager.setPage(0, 3);
        require(hint->isHidden(), "New list content reused a stale blank area");
    }
    {
        using namespace kobalt::ui;
        const int savedDensity = density();
        for (int ppi : { 212, 300 })
            for (bool dark : { false, true }) {
                setDensity(ppi);
                QWidget first, second;
                const auto setTheme = [](QWidget &host, bool dark) {
                    host.setProperty("reader_dark", dark);
                    auto colors = host.palette();
                    colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
                    colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
                    host.setPalette(colors);
                };
                setTheme(first, dark);
                setTheme(second, !dark);
                {
                    QWidget controls;
                    setTheme(controls, dark);
                    applyReaderPalette(&controls, readerPalette(controls.palette(), dark));
                    controls.resize(px(700), px(180));
                    controls.show();
                    {
                        DropdownButton profile(&controls);
                        profile.resize(px(600), px(90));
                        profile.setText("Kobalt");
                        profile.setForward(true);
                        readerStyle(&profile, styles::font(metrics::text::secondary));
                        applyReaderPalette(&profile, controls.palette());
                        profile.show();
                        app.processEvents();
                        const auto image = profile.grab().toImage();
                        const auto area = image.rect().adjusted(px(8), px(8), -px(100), -px(8));
                        QRect ink;
                        for (int y = area.top(); y <= area.bottom(); ++y)
                            for (int x = area.left(); x <= area.right(); ++x)
                                if (dark ? qGray(image.pixel(x, y)) > 192
                                         : qGray(image.pixel(x, y)) < 64)
                                    ink |= QRect(x, y, 1, 1);
                        require(!ink.isEmpty()
                                        && qAbs(ink.left() - int(metrics::choices::fontButtonLeft))
                                                <= px(4),
                                "Forward profile label is not left-aligned");
                        require(qAbs(ink.center().y() - image.rect().center().y()) <= 1,
                                "Forward profile label is not vertically centered");
                    }
                    QVBoxLayout rows(&controls);
                    SegmentedRow segments(&controls, &rows, "contrastSegments", "Header");
                    auto *small = segments.add("contrastSmall", "Small");
                    auto *large = segments.add("contrastLarge", "Large");
                    app.processEvents();
                    for (const bool enabled : { true, false })
                        for (const bool checked : { false, true })
                            for (const bool pressed : { false, true }) {
                                small->setEnabled(enabled);
                                (checked ? small : large)->setChecked(true);
                                small->setDown(pressed);
                                applyReaderPalette(small, readerPalette(controls.palette(), dark));
                                const auto image = small->grab().toImage();
                                const auto color = !enabled
                                        ? QColor("#999999")
                                        : controls.palette().color(checked || pressed
                                                                           ? QPalette::Window
                                                                           : QPalette::WindowText);
                                const auto area =
                                        image.rect().adjusted(px(8), px(8), -px(8), -px(8));
                                int pixels = 0;
                                for (int y = area.top(); y <= area.bottom(); ++y)
                                    for (int x = area.left(); x <= area.right(); ++x)
                                        pixels += image.pixelColor(x, y) == color;
                                require(pixels > 10,
                                        "Segment label disappeared after palette propagation");
                            }
                }
                PaintCountingListRow row(&first);
                row.setCheckable(true);
                row.resize(px(480), metrics::list::twoLines);
                row.setTitle("A long title with words to elide across the row");
                row.setDetail("A detail");
                row.setAccessory(ListRow::Caption{ "123456789", px(60) });
                first.resize(row.size());
                first.show();
                row.show();
                app.processEvents();
                row.clearFocus();
                row.setAttribute(Qt::WA_UnderMouse, false);
                const auto normalRow = row.grab().toImage();
                row.setAttribute(Qt::WA_UnderMouse, true);
                require(row.grab().toImage() == normalRow, "List rows change appearance on hover");
                row.setDown(true);
                require(row.grab().toImage() != normalRow, "List rows lack press feedback");
                row.setDown(false);
                require(row.grab().toImage() == normalRow,
                        "List rows stay highlighted after release");
                int clicks = 0;
                QObject::connect(&row, &QPushButton::clicked, &first, [&] { ++clicks; });
                StyleCountingObserver observer;
                for (auto *label : { row.titleLabel(), row.detailLabel(), row.captionLabel(),
                                     row.leadingLabel(), row.trailingLabel() })
                    label->installEventFilter(&observer);
                const auto labelInk = [&](QLabel *label, QColor color) {
                    require(label->property("reader_style")
                                    .toString()
                                    .endsWith("color:" + color.name() + ';'),
                            "List label styles lag behind presentation state");
                };
                labelInk(row.titleLabel(), first.palette().color(QPalette::WindowText));
                row.blockSignals(true);
                row.setChecked(true);
                labelInk(row.titleLabel(), first.palette().color(QPalette::Window));
                row.setChecked(false);
                row.blockSignals(false);
                labelInk(row.titleLabel(), first.palette().color(QPalette::WindowText));
                row.blockSignals(true);
                const QPoint point = row.rect().center();
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), row.mapToGlobal(point),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                                    row.mapToGlobal(point), Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(&row, &press);
                QApplication::sendEvent(&row, &release);
                require(row.isChecked(), "Signal-blocked row input lost its native checked state");
                labelInk(row.titleLabel(), first.palette().color(QPalette::Window));
                row.setChecked(false);
                row.blockSignals(false);
                row.setChecked(true);
                labelInk(row.titleLabel(), first.palette().color(QPalette::Window));
                // Applying the window theme to existing children must keep
                // the selected row's contrasting text, including in Qt5.
                for (auto *label : { row.titleLabel(), row.detailLabel(), row.captionLabel(),
                                     row.leadingLabel(), row.trailingLabel() })
                    applyReaderPalette(label, first.palette());
                require(row.titleLabel()->palette().color(QPalette::WindowText)
                                == first.palette().color(QPalette::Window),
                        "Applying the theme erased the selected row label color");
                row.setEnabled(false);
                labelInk(row.titleLabel(), QColor("#999999"));
                row.setEnabled(true);
                row.setChecked(false);
                labelInk(row.detailLabel(), noteColor(&row));
                row.setTitlePixels(px(34));
                require(row.titleLabel()->font().pixelSize() == px(34),
                        "Owned title pixels do not reach the row label");
                app.processEvents();
                observer.changes = 0;
                row.paints = 0;
                const auto geometry = row.titleLabel()->geometry();
                row.setAccessory(ListRow::Caption{ "123456789", px(60) });
                row.setTitlePixels(px(34));
                row.setTitle(row.text());
                row.setDetail("A detail");
                row.setCaption("");
                row.setLeading("");
                app.processEvents();
                require(row.paints == 0 && observer.changes == 0
                                && row.titleLabel()->geometry() == geometry,
                        "Unchanged row accessory or title pixels repeats presentation work");
                const int previousWidth = row.titleLabel()->width();
                auto trailingFont = row.trailingLabel()->font();
                trailingFont.setLetterSpacing(QFont::AbsoluteSpacing, px(10));
                row.trailingLabel()->setFont(trailingFont);
                require(row.titleLabel()->width() < previousWidth,
                        "Child font change leaves stale trailing reservation");
                row.setAccessory(ListRow::Chevron{});
                require(row.accessoryValue().kind() == ListRow::Accessory::Kind::Chevron
                                && !row.accessoryValue().caption()
                                && row.trailingLabel()->isHidden()
                                && row.trailingLabel()->text().isEmpty(),
                        "Chevron retains a stale trailing caption");
                row.setAccessory(ListRow::Caption{ "", -1 });
                require(row.accessoryValue().caption()->reservedWidth == 0,
                        "Caption reservation does not clamp negative widths");
                row.setAccessory(ListRow::None{});
                require(row.accessoryValue().kind() == ListRow::Accessory::Kind::None
                                && !row.accessoryValue().caption()
                                && row.titleLabel()->width() > previousWidth,
                        "Empty accessory keeps trailing space reserved");
                // Rows draw with their parent's colors, even if their own palette is explicit.
                row.setPalette(first.palette());
                auto changed = first.palette();
                changed.setColor(QPalette::WindowText, QColor("#123456"));
                first.setPalette(changed);
                labelInk(row.titleLabel(), QColor("#123456"));
                row.setParent(&second);
                labelInk(row.titleLabel(), second.palette().color(QPalette::WindowText));
                labelInk(row.detailLabel(), noteColor(&row));
                changed.setColor(QPalette::WindowText, QColor("#654321"));
                first.setPalette(changed);
                labelInk(row.titleLabel(), second.palette().color(QPalette::WindowText));
                second.resize(row.size());
                second.show();
                row.show();
                app.processEvents();
                observer.changes = 0;
                const QString style = row.titleLabel()->styleSheet();
                row.grab();
                row.grab();
                require(observer.changes == 0 && row.titleLabel()->styleSheet() == style,
                        "List painting mutates label styles");
                require(clicks == 0, "Row presentation updates emit user intent");
                auto *removed = new ListRow(&first);
                removed->setCheckable(true);
                removed->resize(px(240), px(100));
                removed->show();
                QPointer<ListRow> alive(removed);
                QObject::connect(removed, &QPushButton::clicked, &first,
                                 [removed] { delete removed; });
                const QPoint target = removed->rect().center();
                QMouseEvent down(QEvent::MouseButtonPress, QPointF(target),
                                 removed->mapToGlobal(target), Qt::LeftButton, Qt::LeftButton,
                                 Qt::NoModifier);
                QMouseEvent up(QEvent::MouseButtonRelease, QPointF(target),
                               removed->mapToGlobal(target), Qt::LeftButton, Qt::NoButton,
                               Qt::NoModifier);
                QApplication::sendEvent(removed, &down);
                QApplication::sendEvent(removed, &up);
                require(!alive, "List row cannot be deleted during its click callback");
            }
        setDensity(savedDensity);
    }
    for (bool dark : { false, true }) {
        namespace metrics = kobalt::ui::metrics;
        QWidget host;
        host.resize(640, 600);
        host.setProperty("reader_dark", dark);
        auto palette = host.palette();
        palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        host.setPalette(palette);
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        host.show();
        kobalt::ui::ModalOverlay loading(&host);
        loading.card()->message("Applying appearance changes", "rrUpdatingText");
        loading.fitCard();
        for (qint64 elapsed : { qint64(0), qint64(999), qint64(1000) }) {
            loading.setDelayedProgress(true, elapsed, 37, "Applying appearance changes...");
            auto *bar = dynamic_cast<kobalt::ui::ProgressBar *>(
                    loading.findChild<QWidget *>("rrApplyingProgress"));
            require(bar && bar->value() == 37, "Loading bar does not use reported progress");
            require(loading.isVisible() == (elapsed >= 1000),
                    "Appearance notice appears before one second or fails to appear afterwards");
            require(!bar->isHidden() == (elapsed >= 1000),
                    "Appearance bar does not follow the one-second threshold");
            require(loading.card()->findChildren<QLabel *>().size() == 1
                            && loading.findChild<QLabel *>("rrUpdatingText")->text()
                                    == "Applying appearance changes...",
                    "Relayout notice must contain only its reason label");
            if (elapsed >= 1000)
                require(loading.card()->rect().contains(bar->geometry()),
                        "Loading bar exceeds the modal card");
        }
        for (const auto &message : { "Enabling Bold Reading...", "Disabling Bold Reading...",
                                     "Updating page layout..." }) {
            loading.setDelayedProgress(true, 1100, 0, message);
            require(loading.findChild<QLabel *>("rrUpdatingText")->text() == message
                            && loading.card()->findChildren<QLabel *>().size() == 1
                            && dynamic_cast<kobalt::ui::ProgressBar *>(
                                       loading.findChild<QWidget *>("rrApplyingProgress"))
                                            ->value()
                                    == 0,
                    "Replacing a relayout reason adds a second label or retains old progress");
        }
        loading.setDelayedProgress(false, 2500, 100);
        require(!loading.isVisible()
                        && dynamic_cast<kobalt::ui::ProgressBar *>(
                                   loading.findChild<QWidget *>("rrApplyingProgress"))
                                   ->isHidden(),
                "Completed loading notice remains visible");
        loading.setDelayedProgress(true, 0, 0);
        require(!loading.isVisible()
                        && dynamic_cast<kobalt::ui::ProgressBar *>(
                                   loading.findChild<QWidget *>("rrApplyingProgress"))
                                   ->isHidden(),
                "A new render inherits the previous loading notice");
        kobalt::ui::ListRow row(&host);
        row.setTitle("A very long chapter name that must fit in a narrow row");
        require(row.sizeHint().height() == metrics::list::oneLine,
                "One-line row has a different height");
        row.setDetail("A passage from the book");
        require(row.sizeHint().height() == metrics::list::twoLines,
                "Two-line row has a different height");
        row.setCaption("12 MAY 2026");
        require(row.sizeHint().height() == metrics::list::threeLines,
                "Three-line row has a different height");
        auto *remove = row.addAction("remove", "Remove bookmark");
        row.setLeading("01");
        row.setAccessory(kobalt::ui::ListRow::Caption{ "123", 60 });
        row.resize(480, row.sizeHint().height());
        row.show();
        QApplication::processEvents();
        require(row.titleLabel()->text() != row.text()
                        && row.titleLabel()->text().contains(QChar(0x2026)),
                "Row title is not elided");
        require(remove->width() == metrics::chrome::cellWidth && remove->height() == row.height()
                        && remove->geometry().right() == row.width() - 1,
                "Trailing action does not fill its cell");
        require(row.titleLabel()->geometry().right() < row.trailingLabel()->x()
                        && row.trailingLabel()->geometry().right() < remove->x(),
                "Row text overlaps its trailing cell");
        row.setCheckable(true);
        row.setChecked(true);
        QApplication::processEvents();
        require(qGray(row.grab().toImage().pixel(3, 3)) == (dark ? 255 : 0),
                "Current row is not inverted");
        row.setDown(true);
        require(qGray(row.grab().toImage().pixel(3, 3)) == (dark ? 255 : 0),
                "Pressed current row loses its inversion");
        row.setDown(false);
        row.setChecked(false);
        row.setDown(true);
        require(qGray(row.grab().toImage().pixel(3, 3)) == (dark ? 51 : 221),
                "Pressed row has a different highlight");
        row.setDown(false);
        row.setEnabled(false);
        require(qGray(row.grab().toImage().pixel(3, 3)) == (dark ? 0 : 255),
                "Disabled row highlights");
        kobalt::ui::ListHeader header(&host);
        header.resize(480, metrics::list::headerHeight);
        header.setCaptions("Contents · 16 chapters", "Page");
        require(header.leftLabel()->text() == QStringLiteral("Contents · 16 chapters")
                        && header.rightLabel()->text() == "Page"
                        && header.leftLabel()->font().capitalization() == QFont::AllUppercase
                        && header.leftLabel()->geometry().right() < header.rightLabel()->x(),
                "List header loses captions or overlaps columns");
    }
    {
        QWidget reader;
        reader.show();
        kobalt::BrowserHandoff handoff;
        bool suspended = false;
        handoff.begin(&reader, [&] { return !suspended; });
        QWidget wireless;
        wireless.setObjectName("N3Dialog");
        wireless.show();
        QApplication::processEvents();
        require(reader.isVisible(), "Unrelated native dialog hid the reader");
        wireless.hide();
        QApplication::processEvents();
        QWidget dialog;
        dialog.setObjectName("N3Dialog");
        QWidget browser(&dialog);
        browser.setObjectName("N3BrowserView");
        dialog.show();
        browser.show();
        QApplication::processEvents();
        require(reader.isHidden(), "Native browser did not hide the modal reader");
        require(kobalt::BrowserHandoff::activeFor(&reader),
                "Browser handoff did not block the visibility check from waking the reader");
        dialog.hide();
        QApplication::processEvents();
        require(reader.isVisible(), "Closing the native browser did not restore the reader");
        require(!kobalt::BrowserHandoff::activeFor(&reader),
                "Closed browser kept the reader wake blocked");
        handoff.begin(&reader, [&] { return !suspended; });
        dialog.show();
        browser.show();
        QApplication::processEvents();
        suspended = true;
        dialog.hide();
        QApplication::processEvents();
        require(reader.isHidden() && kobalt::BrowserHandoff::activeFor(&reader),
                "Sleeping the browser released its reader handoff");
        suspended = false;
        dialog.show();
        QApplication::processEvents();
        require(reader.isHidden(), "Waking the browser restored the reader above it");
        dialog.hide();
        QApplication::processEvents();
        require(reader.isVisible() && !kobalt::BrowserHandoff::activeFor(&reader),
                "Closing the resumed browser did not restore the reader");
        auto *removed = new QWidget;
        removed->setObjectName("N3Dialog");
        auto *removedBrowser = new QWidget(removed);
        removedBrowser->setObjectName("N3BrowserView");
        handoff.begin(&reader, [&] { return !suspended; });
        removed->show();
        QApplication::processEvents();
        suspended = true;
        delete removed;
        QApplication::processEvents();
        require(reader.isHidden() && !kobalt::BrowserHandoff::activeFor(&reader),
                "Destroyed browser left a suspended reader held indefinitely");
    }
    const int openingDensity = kobalt::ui::density();

    struct OpeningScreen
    {
        int width, height, ppi;
    };

    // Reader widgets use physical pixels, including the desktop canvas.
    for (const auto &screen : { OpeningScreen{ 420, 500, 150 }, OpeningScreen{ 840, 1000, 300 },
                                OpeningScreen{ 800, 600, 212 }, OpeningScreen{ 1072, 1448, 300 } })
        for (bool dark : { false, true }) {
            kobalt::ui::setDensity(screen.ppi);
            QJsonObject preferences;
            preferences["dark_mode"] = dark;
            QWidget background;
            background.resize(screen.width, screen.height);
            background.show();
            auto view = kobalt::createOpening(&background, preferences,
                                              kobalt::test::makeHostPresentation);
            auto *window = view->window();
            window->resize(screen.width, screen.height);
            window->show();
            app.processEvents();
            auto *progress = window->findChild<QProgressBar *>("rrOpeningProgress");
            auto *label = window->findChild<QLabel *>("rrOpeningLabel");
            auto *hint = window->findChild<QLabel *>("rrOpeningHint");
            require(progress && progress->isVisible() && label, "normal opening has progress");
            require(!window->findChild<QWidget *>("rrOpeningFailure"),
                    "normal opening builds no failure card");
            view->showFallback(kobalt::OpeningFallback::Protected);
            app.processEvents();
            require(progress->isVisible() && progress->value() == 100
                            && !window->findChild<QWidget *>("rrOpeningFailure"),
                    "handoff keeps a completed progress bar without confirmation");
            require(label->text() == kobalt::strings::get("opening.native"), "handoff caption");
            view->showFallback(kobalt::OpeningFallback::Remembered);
            app.processEvents();
            require(hint->text() == kobalt::strings::get("opening.remembered").toUpper()
                            && !window->findChild<QWidget *>("rrOpeningFailure"),
                    "remembered failure uses neutral automatic handoff");
            view->showFallback(kobalt::OpeningFallback::Failure);
            app.processEvents();
            auto *failure = window->findChild<QWidget *>("rrOpeningFailure");
            auto *retry = window->findChild<QPushButton *>("rrOpeningNative");
            auto *back = window->findChild<QPushButton *>("rrOpeningBack");
            require(failure && retry && back, "failure builds its card");
            auto *failureTitle = failure->findChild<QLabel *>("rrModalTitle");
            auto *failureBody = failure->findChild<QLabel *>("rrModalDescription");
            require(retry->isVisible() && back->isVisible() && failureBody->isVisible(),
                    "failure has two actions and explanation");
            require(failureTitle->text() == kobalt::strings::get("opening.failed"),
                    "failure caption");
            require(failureTitle->font().pixelSize() == int(kobalt::ui::metrics::chrome::labelText),
                    "failure uses the shared title size");
            for (auto *widget :
                 { static_cast<QWidget *>(failureTitle), static_cast<QWidget *>(failureBody),
                   static_cast<QWidget *>(retry), static_cast<QWidget *>(back) })
                require(QFontInfo(widget->font()).family() == kobalt::ui::interfaceFontFamily(),
                        "failure uses the shared interface font");
            require(failureTitle->geometry().bottom() < failureBody->geometry().top()
                            && failureBody->geometry().bottom() < retry->geometry().top(),
                    "failure content does not overlap");
            require(failure->rect().contains(
                            failure->findChild<QWidget *>("rrModalCard")->geometry()),
                    "failure card fits");
            int accepted = 0, rejected = 0;
            QObject::connect(window, &QDialog::accepted, [&] {
                ++accepted;
                require(!window->isVisible(), "native continuation must not have a modal splash");
            });
            QObject::connect(window, &QDialog::rejected, [&] { ++rejected; });
            retry->click();
            require(accepted == 1 && rejected == 0, "native action accepts");
            window->show();
            back->click();
            require(rejected == 1, "back action rejects");
        }
    kobalt::ui::setDensity(openingDensity);

    {
        using namespace kobalt::ui;
        QWidget host;
        host.setFont(interfaceFont());
        readerStyle(&host, styles::reader());
        BackButton back("Exit Kobalt", &host);
        back.setObjectName("rrReturn");
        back.setContentsMargins(0, 0, metrics::panel::exitLabelExtraRight, 0);
        // A late font change must not retain the width measured at construction.
        for (int pixels : { 40, 52 }) {
            readerStyle(&back, "QPushButton#rrReturn{" + styles::brand(pixels) + "}");
            back.ensurePolished();
            back.resize(back.sizeHint().width(), 112);
            host.resize(600, 200);
            host.show();
            app.processEvents();
            const int available = back.width() - metrics::navigation::labelLeft
                    - metrics::navigation::labelRight - back.contentsMargins().right();
            require(back.fontMetrics().elidedText(back.text(), Qt::ElideRight, available)
                            == back.text(),
                    "Back button size hint truncates the Exit Kobalt caption");
        }
        if (app.arguments().contains("--exit-button-only"))
            return 0;
    }
    {
        using namespace kobalt;
        strings::showKeys(true);
        QWidget host;
        ChapterGraph graph(&host);
        require(graph.findChild<QLabel *>("rrGraphChapterCaption")->text() == "stats.chapter",
                "Chapter caption does not expose its catalog key");
        graph.setProgress("Title", 4, 10, 12, 25, 100);
        // Nickel's count: chapter page 4 of 10 is 40%.
        require(graph.findChild<QLabel *>("rrGraphChapterPercent")->text() == "common.percent [40]",
                "Key mode lost the formatting argument");
        ui::PageNavigation navigation(&host, "keyTest");
        require(navigation.back->text() == "common.back", "Navigation bypasses the string catalog");
        strings::showKeys(false);
    }
    {
        using namespace kobalt::ui;
        BatteryIndicator battery(nullptr);
        readerStyle(&battery, "background:white;color:black;");
        battery.show();
        int previous = -1;
        for (int percent : { 0, 10, 25, 50, 75, 90, 100 }) {
            battery.setStatus(percent, false);
            app.processEvents();
            const auto image = battery.grab().toImage();
            int ink = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = image.width() - metrics::panel::batteryIconSize; x < image.width();
                     ++x)
                    ink += qGray(image.pixel(x, y)) < 128;
            require(ink > previous, "Battery fill does not track small percentage changes");
            previous = ink;
        }
    }
    {
        using namespace kobalt::ui;
        // Interface text is unhinted on every path, including style sheet
        // fonts that resolve against an interface font.
        for (const auto &font : { interfaceFont(24), chromeFont(24), captionFont(), brandFont(24) })
            require(font.hintingPreference() == QFont::PreferNoHinting,
                    "Interface font asks for hinting");
        // Nickel's style sheet makes check boxes italic, and a text link is
        // underlined. Painting resolves a font against its widget's, so the
        // interface fonts set these themselves and stay upright and plain.
        QFont decorated;
        decorated.setItalic(true);
        decorated.setUnderline(true);
        for (const auto &font : { interfaceFont(24), chromeFont(24), captionFont() }) {
            const auto painted = font.resolve(decorated);
            require(!painted.italic() && !painted.underline(),
                    "Interface font took a widget's italic or underline");
        }
        QWidget parent;
        parent.setFont(interfaceFont());
        QLabel styled(&parent);
        styled.setStyleSheet("font-size:20px;font-weight:600;");
        styled.ensurePolished();
        require(styled.font().hintingPreference() == QFont::PreferNoHinting,
                "Style sheet font lost the interface hinting preference");
        // Qt 5.2 polishes style sheet fonts with the default hinting. The
        // window's keeper turns such a font back to unhinted.
        QWidget window;
        keepInterfaceUnhinted(&window);
        QLabel reset(&window);
        QFont polished = interfaceFont(20);
        polished.setHintingPreference(QFont::PreferDefaultHinting);
        reset.setFont(polished);
        require(reset.font().hintingPreference() == QFont::PreferNoHinting,
                "Window kept a font with the default hinting");
        QWidget elsewhere;
        QLabel outside(&elsewhere);
        outside.setFont(polished);
        require(outside.font().hintingPreference() == QFont::PreferDefaultHinting,
                "Hinting keeper changed a widget in another window");
    }
    {
        using namespace kobalt::ui;
        for (int id = int(ControlIcon::Search); id <= int(ControlIcon::More); ++id) {
            const auto black = controlIcon(ControlIcon(id))
                                       .pixmap(64, 64)
                                       .toImage()
                                       .convertToFormat(QImage::Format_ARGB32);
            const auto white = controlIcon(ControlIcon(id), Qt::white)
                                       .pixmap(64, 64)
                                       .toImage()
                                       .convertToFormat(QImage::Format_ARGB32);
            require(!black.isNull() && black.size() == white.size(),
                    "Bundled icon resource is missing");
            int ink = 0;
            for (int y = 0; y < black.height(); ++y)
                for (int x = 0; x < black.width(); ++x) {
                    const auto b = QColor::fromRgba(black.pixel(x, y)),
                               w = QColor::fromRgba(white.pixel(x, y));
                    require(b.alpha() == w.alpha(), "Icon tint changed its shape");
                    if (b.alpha() > 128) {
                        ++ink;
                        require(b.lightness() == 0 && w.lightness() == 255,
                                "Icon tint lost theme contrast");
                    }
                }
            require(ink > 20, "Bundled icon is empty");
        }
    }
    {
        using namespace kobalt::ui;
        const int savedDensity = density();
        for (int ppi : { 167, 212, 300 }) {
            setDensity(ppi);
            for (int id = int(ControlIcon::Search); id <= int(ControlIcon::Download); ++id) {
                const auto glyph = ControlIcon(id);
                const auto icon = controlIcon(glyph);
                for (int size : { 19, 37, 83, 256 }) {
                    const auto raster = icon.pixmap(size, size).toImage();
                    require(raster.size() == QSize(size, size),
                            "Vector icon did not render at its requested pixel size");
                    int visible = 0;
                    for (int y = 0; y < size; ++y)
                        for (int x = 0; x < size; ++x)
                            visible += qAlpha(raster.pixel(x, y)) > 32;
                    require(visible > 3, "Vector glyph has no visible ink");
                    QImage direct(size, size, QImage::Format_ARGB32_Premultiplied);
                    direct.fill(Qt::transparent);
                    {
                        QPainter painter(&direct);
                        icon.paint(&painter, direct.rect());
                    }
                    require(direct.convertToFormat(QImage::Format_ARGB32)
                                    == raster.convertToFormat(QImage::Format_ARGB32),
                            "Icon pixmap and direct drawing use different geometry");
                }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                for (qreal dpr : { 1., 1.5, 2. }) {
                    const int pixels = qRound(38 * dpr);
                    const auto raster = icon.pixmap(QSize(38, 38), dpr);
                    require(raster.size() == QSize(pixels, pixels)
                                    && qFuzzyCompare(raster.devicePixelRatio(), dpr),
                            "Vector icon applied device pixel ratio twice or lost its physical resolution");
                    QImage direct(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
                    direct.fill(Qt::transparent);
                    direct.setDevicePixelRatio(dpr);
                    {
                        QPainter painter(&direct);
                        icon.paint(&painter, QRect(0, 0, 38, 38));
                    }
                    require(direct.convertToFormat(QImage::Format_ARGB32)
                                    == raster.toImage().convertToFormat(QImage::Format_ARGB32),
                            "High-DPI icon bitmap differs from direct vector drawing");
                }
#endif
            }
            QWidget host;
            host.setProperty("reader_dark", false);
            Button action(&host);
            for (auto glyph : { ControlIcon::Close, ControlIcon::Search, ControlIcon::Portrait,
                                ControlIcon::Download }) {
                action.setIconConfiguration({ glyph, IconRendition::Action });
                for (auto state : { QIcon::Off, QIcon::On }) {
                    const auto image = action.icon()
                                               .pixmap(QSize(128, 128), QIcon::Normal, state)
                                               .toImage()
                                               .convertToFormat(QImage::Format_ARGB32);
                    int ink = 0;
                    QRect bounds;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const auto color = QColor::fromRgba(image.pixel(x, y));
                            if (color.alpha() > 128) {
                                require(color.lightness() == (state == QIcon::Off ? 0 : 255),
                                        "Action icon lost its active color");
                                bounds = bounds.united(QRect(x, y, 1, 1));
                                ++ink;
                            }
                        }
                    require(ink > 20, "Action rendition has no visible ink");
                    int extent = qRound(128.
                                        * int(glyph == ControlIcon::Close
                                                      ? metrics::control::closeInkSize
                                                      : metrics::control::actionInkSize)
                                        / int(metrics::control::iconCanvas));
                    if (glyph == ControlIcon::Search || glyph == ControlIcon::Download) {
                        // Regular Phosphor icons keep a shared scale rather than
                        // stretching each silhouette to the same visible extent.
                        const auto original = controlIcon(glyph).pixmap(256, 256).toImage();
                        QRect originalBounds;
                        for (int y = 0; y < original.height(); ++y)
                            for (int x = 0; x < original.width(); ++x)
                                if (qAlpha(original.pixel(x, y)) > 128)
                                    originalBounds = originalBounds.united(QRect(x, y, 1, 1));
                        extent = qRound(128. * int(metrics::control::actionInkSize)
                                        / int(metrics::control::iconCanvas)
                                        * qMax(originalBounds.width(), originalBounds.height())
                                        / 224.);
                    }
                    require(qAbs(qMax(bounds.width(), bounds.height()) - extent) <= 2,
                            "Action icon does not fit its configured vector scale");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    const auto highDpi =
                            action.icon().pixmap(QSize(38, 38), 2., QIcon::Normal, state);
                    QImage direct(76, 76, QImage::Format_ARGB32_Premultiplied);
                    direct.fill(Qt::transparent);
                    direct.setDevicePixelRatio(2.);
                    {
                        QPainter painter(&direct);
                        action.icon().paint(&painter, QRect(0, 0, 38, 38), Qt::AlignCenter,
                                            QIcon::Normal, state);
                    }
                    require(highDpi.size() == QSize(76, 76) && highDpi.devicePixelRatio() == 2.
                                    && direct.convertToFormat(QImage::Format_ARGB32)
                                            == highDpi.toImage().convertToFormat(
                                                    QImage::Format_ARGB32),
                            "Action or active icon loses final device resolution");
#endif
                }
            }
        }
        setDensity(savedDensity);
        const auto cachedIcon = controlIcon(ControlIcon::Search, QColor("#153759"));
        const auto first = cachedIcon.pixmap(64, 64);
        const auto repeated = cachedIcon.pixmap(64, 64);
        require(first.cacheKey() == repeated.cacheKey(),
                "Repeated icon requests did not reuse their final-size raster");
        const auto recreated = controlIcon(ControlIcon::Search, QColor("#153759")).pixmap(64, 64);
        require(first.cacheKey() == recreated.cacheKey(),
                "Short-lived icons did not share their final-size raster");
        for (int color = 0; color < 80; ++color)
            controlIcon(ControlIcon::Search, QColor(color, 100, 200)).pixmap(64, 64);
        const auto evicted = cachedIcon.pixmap(64, 64);
        require(first.cacheKey() != evicted.cacheKey() && first.toImage() == evicted.toImage(),
                "Bounded icon cache failed to evict or changed regenerated pixels");
        const auto oversized = cachedIcon.pixmap(400, 400);
        const auto oversizedAgain = cachedIcon.pixmap(400, 400);
        require(oversized.cacheKey() != oversizedAgain.cacheKey()
                        && oversized.toImage() == oversizedAgain.toImage(),
                "Oversized icon entered the bounded cache or changed uncached output");
        const auto disabled = cachedIcon.pixmap(QSize(64, 64), QIcon::Disabled);
        require(disabled.toImage() != evicted.toImage(),
                "Icon cache mixed disabled and normal pixels");
        for (const QColor &ink : { QColor(Qt::black), QColor(Qt::white), QColor("#666666") }) {
            for (int transform = 0; transform < 5; ++transform) {
                QImage expected(120, 120, QImage::Format_ARGB32_Premultiplied);
                expected.fill(QColor("#4675a3"));
                QImage actual = expected;
                const auto prepare = [transform](QPainter &painter) {
                    if (transform == 1)
                        painter.translate(.25, .5);
                    if (transform == 2)
                        painter.scale(1.5, .75);
                    if (transform == 3) {
                        painter.translate(110, 0);
                        painter.scale(-1, 1);
                    }
                    if (transform == 4)
                        painter.setOpacity(.8);
                };
                {
                    QPainter painter(&expected);
                    prepare(painter);
                    paintOpeningIcon(painter, ControlIcon::Lock, QRect(10, 10, 64, 64), ink);
                }
                {
                    QPainter painter(&actual);
                    prepare(painter);
                    controlIcon(ControlIcon::Lock, ink).paint(&painter, QRect(10, 10, 64, 64));
                }
                require(actual == expected,
                        "Cached or transformed icon changed direct vector edge composition");
            }
        }
        QImage paper(96, 96, QImage::Format_ARGB32_Premultiplied);
        paper.fill(QColor("#4675a3"));
        QPainter painter(&paper);
        painter.setOpacity(.8);
        painter.setPen(Qt::red);
        painter.setBrush(Qt::green);
        painter.translate(2, 3);
        painter.setClipRect(QRect(0, 0, 90, 90));
        const auto transform = painter.transform();
        const auto pen = painter.pen();
        const auto brush = painter.brush();
        const auto clip = painter.clipRegion();
        for (int turn = 0; turn < 4; ++turn)
            controlIcon(ControlIcon(int(ControlIcon::Portrait) + turn), Qt::black, true)
                    .paint(&painter, QRect(5, 5, 76, 76));
        require(painter.transform() == transform && painter.pen() == pen && painter.brush() == brush
                        && painter.clipRegion() == clip && qFuzzyCompare(painter.opacity(), .8)
                        && painter.compositionMode() == QPainter::CompositionMode_SourceOver,
                "Drawing an icon changed its caller's painter state");
        painter.end();
        for (int y = 0; y < paper.height(); ++y)
            for (int x = 0; x < paper.width(); ++x)
                require(qAlpha(paper.pixel(x, y)) == 255,
                        "Orientation icon erased its caller's background");
    }
    {
        using namespace kobalt::ui;
        // The thick bezel must match the physical edge after Qt's rotation.
        for (bool sideButtons : { false, true }) {
            for (int turn = 0; turn < 4; ++turn) {
                const auto image = controlIcon(ControlIcon(int(ControlIcon::Portrait) + turn),
                                               Qt::black, sideButtons)
                                           .pixmap(128, 128)
                                           .toImage();
                int ink[4]{}; // top, right, bottom, left
                for (int along = 45; along < 83; ++along)
                    for (int edge = 0; edge < 40; ++edge) {
                        ink[0] += qAlpha(image.pixel(along, edge));
                        ink[1] += qAlpha(image.pixel(127 - edge, along));
                        ink[2] += qAlpha(image.pixel(along, 127 - edge));
                        ink[3] += qAlpha(image.pixel(edge, along));
                    }
                const int expected = ((sideButtons ? 1 : 2) - turn + 4) % 4;
                for (int edge = 0; edge < 4; ++edge)
                    require(edge == expected || ink[expected] > ink[edge],
                            "Rotation icon shows the wrong physical bezel edge");
            }
        }
    }
    {
        QWidget host;
        host.resize(1072, 1448);
        host.show();
        kobalt::LinkPanel note(&host);
        kobalt::LinkDetails details;
        details.kind = kobalt::LinkKind::Footnote;
        details.target = "passage";
        details.chapter = "Notes:";
        details.html = "<p>Short note.</p>";
        const QRect readingArea(0, 80, 1072, 1288);
        QString destination;
        note.navigate = [&](const QString &target) { destination = target; };
        note.open(details, QRect(80, 100, 150, 40), readingArea);
        QApplication::processEvents();
        auto *text = dynamic_cast<kobalt::ui::PagedText *>(note.findChild<QWidget *>("rrLinkText"));
        auto *open = note.findChild<QPushButton *>("rrLinkOpen");
        auto *next = note.findChild<QPushButton *>("rrLinkNext");
        require(readingArea.contains(note.geometry()), "Lower note overlaps the reading footer");
        require(note.y() > 500 && text->pages() == 1 && !next->isEnabled(),
                "Short note geometry or paging is incorrect");
        require(destination.isEmpty(), "Opening a note navigated without choosing Show in book");
        auto *row = static_cast<kobalt::ui::CaptionButton *>(open);
        require(row->text() == kobalt::strings::get("link_panel.show_in_book")
                        && row->caption()
                                == kobalt::strings::get("link_panel.in_chapter").arg("Notes"),
                "Show in book does not name the note's chapter");
        require(open->height() == kobalt::ui::metrics::links::returnHeight,
                "Show in book is not a return-row height");
        {
            // The row closes a lower sheet with a divider where it meets the footer strip.
            QImage image(note.size(), QImage::Format_ARGB32);
            note.render(&image);
            const QColor edge =
                    QColor::fromRgba(image.pixel(note.width() / 2, open->geometry().bottom()));
            require(edge == kobalt::ui::dividerColor(&note),
                    "Show in book lacks its bottom divider");
        }
        open->click();
        require(destination == details.target, "Show in book lost its destination");
        details.html =
                QString("<p>A long paragraph with enough text to wrap into several lines at the reader UI size.</p>")
                        .repeated(80);
        note.open(details, QRect(80, 1100, 150, 40), readingArea);
        QApplication::processEvents();
        require(readingArea.contains(note.geometry()), "Upper note overlaps the reading header");
        require(note.y() < 120 && text->pages() > 1 && next->isEnabled(),
                "Long note did not paginate above its reference");
        next->click();
        require(text->page() == 1, "Note Next did not advance");
        note.findChild<QPushButton *>("rrLinkPrevious")->click();
        require(text->page() == 0, "Note Previous did not return");
        details.target.clear();
        details.chapter.clear();
        details.html = "<p>A hidden note.</p>";
        note.open(details, {});
        QApplication::processEvents();
        require(!open->isVisible() && text->page() == 0 && text->pages() == 1,
                "Hidden note retained a target or old pagination");
        QWidget browserHost;
        browserHost.resize(1264, 1680);
        browserHost.show();
        kobalt::WebsiteDialog website(&browserHost);
        const QString address = "https://example.org/";
        website.open(address);
        QApplication::processEvents();
        auto *confirm = website.findChild<QPushButton *>("rrLinkOpen");
        require(confirm && !confirm->isEnabled(),
                "Unavailable browser did not disable confirmation");
        auto *decline = website.findChild<QPushButton *>("rrLinkNo");
        require(decline && decline->isVisible(), "External website has no No action");
        int browserCalls = 0, dismissals = 0;
        website.browserAvailable = [] { return true; };
        website.openBrowser = [&](const QUrl &url) {
            ++browserCalls;
            return url == QUrl(address);
        };
        website.dismissed = [&] { ++dismissals; };
        website.open(address);
        QApplication::processEvents();
        require(confirm->isEnabled() && browserCalls == 0, "Browser opened before confirmation");
        decline->click();
        require(browserCalls == 0 && dismissals == 1, "No opened the browser");
        confirm->click();
        require(browserCalls == 1 && dismissals == 2, "Yes did not open and dismiss the link");
        const auto savedScale = kobalt::ui::textScale();
        kobalt::ui::setLargeText(1.5);
        {
            kobalt::WebsiteDialog large(&browserHost);
            large.browserAvailable = [] { return true; };
            large.secondaryButton()->setText("Keep reading without opening the browser");
            large.open(address);
            QApplication::processEvents();
            auto *card = large.card();
            auto *cancel = large.secondaryButton();
            auto *heading = card->findChild<QLabel *>("rrBrowserPrompt");
            auto *url = card->findChild<QLabel *>("rrBrowserUrl");
            require(url->mapTo(card, QPoint()).y() - heading->mapTo(card, QPoint()).y()
                                    - heading->height()
                            == int(kobalt::ui::metrics::notice::headingGap),
                    "Scrollable browser content changed the shared heading gap");
            require(large.primaryButton()->y() < cancel->y(),
                    "Large-text browser prompt did not stack its actions");
            require(cancel->geometry().bottom() == card->height() - 3,
                    "Scrollable browser content displaced the modal footer");
            const auto pixels = card->grab().toImage();
            const auto ink = card->palette().color(QPalette::WindowText);
            require(pixels.pixelColor(0, cancel->geometry().center().y()) == ink
                            && pixels.pixelColor(pixels.width() / 2, pixels.height() - 1) == ink,
                    "Large-text browser prompt lost its footer frame");
        }
        kobalt::ui::setLargeText(savedScale);
    }
    for (bool dark : { false, true }) {
        QWidget host;
        host.resize(1072, 1448);
        host.setProperty("reader_dark", dark);
        QPalette palette = host.palette();
        palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        host.setPalette(palette);
        host.show();
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        kobalt::ChapterContents choices(&host);
        choices.resize(host.size());
        QVector<kobalt::Choice> chapters;
        for (int i = 0; i < 35; ++i)
            chapters.append({ QString("Chapter %1").arg(i + 1),
                              QString::number(i),
                              QString("chapter%1").arg(i),
                              {},
                              i == 1 ? 2 : 1,
                              i * 10 });
        chapters[2].value.clear();
        choices.showChapters("Book <title>", chapters, "chapter34");
        QApplication::processEvents();
        auto *contents = &choices;
        auto *previous = choices.findChild<QPushButton *>("rrContentsPrevious");
        require(choices.currentPage() == 2
                        && choices.findChild<QPushButton *>("rrContentsNext")->isEnabled(),
                "Contents did not open on the current chapter's page");
        auto *current = contents->findChild<QLabel *>("chapter34Title");
        require(current && !current->font().bold(), "Current chapter uses a bold label");
        require(contents->findChild<QLabel *>("chapter34Number")->text() == "35"
                        && contents->findChild<QLabel *>("chapter34Page")->text() == "341",
                "Current chapter lost its number or start page");
        auto *currentButton = contents->findChild<QPushButton *>("chapter34");
        const auto selectedPaper = currentButton->grab().toImage().pixel(4, 4);
        require(qGray(selectedPaper) == (dark ? 255 : 0), "Current chapter is not filled with ink");
        currentButton->setDown(true);
        QApplication::processEvents();
        require(currentButton->grab().toImage().pixel(4, 4) == selectedPaper,
                "Pressing the current chapter washes out its selected background");
        currentButton->setDown(false);
        previous->click();
        previous->click();
        QApplication::processEvents();
        require(choices.currentPage() == 0 && previous->isEnabled(),
                "Contents pager did not change the visible page");
        contents->findChild<QPushButton *>("chapter0Toggle")->click();
        auto *parent = contents->findChild<QLabel *>("chapter0Title");
        auto *nested = contents->findChild<QLabel *>("chapter1Title");
        require(parent && nested
                        && parent->x()
                                == contents->findChild<QLabel *>("chapter0Number")
                                                ->geometry()
                                                .right()
                                        + 1
                        && nested->x() > parent->x(),
                "Contents lost chapter depth or indented the root level");
        require(contents->findChild<QLabel *>("chapter0Number")->text() == "01",
                "Chapter numbers are not zero-padded");
        require(!contents->findChild<QPushButton *>("chapter2")->isEnabled(),
                "Unresolved chapter remained navigable");
        auto *row = contents->findChild<QPushButton *>("chapter1");
        row->setFocus(Qt::MouseFocusReason);
        row->setDown(true);
        QApplication::processEvents();
        const int highlight = dark ? 51 : 221;
        const auto pressed = row->grab().toImage();
        require(qGray(pressed.pixel(4, 4)) == highlight
                        && qGray(pressed.pixel(nested->x() + 2, 3)) == highlight,
                "Pressed chapter row does not show a full gray highlight");
        for (int x = 1; x < pressed.width() - 1; ++x)
            require(qGray(pressed.pixel(x, 1)) == highlight,
                    "Chapter highlight has an inset focus outline");
        row->setDown(false);
        require(qGray(row->grab().toImage().pixel(4, 4)) == (dark ? 0 : 255),
                "Released chapter row retains a focus highlight");
        row->clearFocus();
        row->setAttribute(Qt::WA_UnderMouse, true);
        QMouseEvent enter(QEvent::MouseMove, QPointF(4, 4), row->mapToGlobal(QPoint(4, 4)),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(row, &enter);
        require(qGray(row->grab().toImage().pixel(4, 4)) == (dark ? 0 : 255),
                "Chapter row highlights on hover");
        row->setAttribute(Qt::WA_UnderMouse, false);
        QMouseEvent leave(QEvent::MouseMove, QPointF(-1, -1), row->mapToGlobal(QPoint(-1, -1)),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(row, &leave);
        require(qGray(row->grab().toImage().pixel(4, 4)) == (dark ? 0 : 255),
                "Chapter row retained its highlight after leaving");
        auto *disabled = contents->findChild<QPushButton *>("chapter2");
        disabled->setAttribute(Qt::WA_UnderMouse, true);
        QApplication::sendEvent(disabled, &enter);
        require(qGray(disabled->grab().toImage().pixel(4, 4)) == (dark ? 0 : 255),
                "Unavailable chapter shows a hover highlight");

        QString target;
        choices.selected = [&](const QString &value) { target = value; };
        contents->findChild<QPushButton *>("chapter1")->click();
        require(target == "1", "Contents paging selected a stale destination");
        bool closed = false;
        choices.back = [&] { closed = true; };
        auto *close = choices.findChild<QPushButton *>("rrContentsClose");
        close->click();
        require(closed, "Contents close did not return to the reader");
        require(close->geometry().topLeft() == QPoint()
                        && close->width() == kobalt::ui::metrics::chrome::cellWidth,
                "The Chapters bar lost its close cell");
        auto *next = choices.findChild<QPushButton *>("rrContentsNext");
        previous->click();
        require(choices.currentPage() == 2, "Previous did not wrap to the final contents page");
        next->click();
        require(choices.currentPage() == 0, "Next did not wrap to the first contents page");
        previous->click();
        require(contents->findChild<QPushButton *>("chapter34")->grab().toImage().pixel(4, 4)
                        == selectedPaper,
                "Wrapping changed the selected chapter highlight");
        choices.resize(1448, 1072);
        QApplication::processEvents();
        const auto count = choices.findChild<QLabel *>("rrContentsPage")->text().split(" of ");
        require(count.size() == 2 && count[0].toInt() == choices.currentPage() + 1
                        && count[0].toInt() <= count[1].toInt(),
                "Contents resize left the current page outside its range");
        choices.showChapters("Short book", chapters.mid(0, 3), "chapter0");
        require(!next->isVisibleTo(&choices), "A single page of contents shows a pager");
        choices.showChapters("Empty book", {}, {});
        require(contents->findChild<QLabel *>("rrContentsEmpty")->isVisible(),
                "Empty contents retained stale chapter rows");
        for (auto *button : contents->findChildren<QPushButton *>())
            require(!button->objectName().startsWith("chapter") || !button->isVisible(),
                    "Empty contents retained a navigable chapter");
    }
    // Checks nested visibility, independent heading actions and source identities after filtering.
    for (bool dark : { false, true }) {
        QWidget host;
        host.resize(1072, 1448);
        host.setProperty("reader_dark", dark);
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        host.show();
        kobalt::ChapterContents contents(&host);
        contents.resize(host.size());
        const QVector<kobalt::Choice> chapters{
            { "Part A", "part-a", "a", {}, 2, 0 },
            { "Heading", {}, "heading", {}, 4, -1 },
            { "Duplicate", "same", "current", {}, 8, 12 },
            { "Duplicate", "same", "duplicate", {}, 8, 12 },
            { "Other chapter", "other", "other", {}, 4, 20 },
            { "Part B", "part-b", "b", {}, 2, 30 },
            { "Last chapter", "last", "last", {}, 4, 31 },
        };
        auto button = [&](const QString &name) { return contents.findChild<QPushButton *>(name); };
        auto shown = [&](const QString &name) {
            auto *control = button(name);
            return control && control->isVisibleTo(&contents);
        };
        QString selected;
        contents.selected = [&](const QString &value) { selected = value; };
        contents.showChapters("Nested book", chapters, "current");
        require(shown("current") && shown("duplicate") && !shown("last"),
                "Contents did not expand only the current chapter's ancestors");
        require(button("current")->isChecked() && !button("duplicate")->isChecked(),
                "Duplicate destinations confused the current chapter identity");
        require(button("current")->x() == 0 && button("current")->width() == contents.width(),
                "A leaf chapter does not own its full row hit area");
        require(contents.findChild<QLabel *>("currentNumber")->mapTo(&contents, QPoint()).x()
                        == contents.findChild<QLabel *>("aNumber")->mapTo(&contents, QPoint()).x(),
                "Full-width chapter rows shifted the number column");
        const auto chapterImage = contents.grab().toImage();
        const int activeY = button("current")->geometry().center().y();
        require(chapterImage.pixelColor(0, activeY)
                                == contents.palette().color(QPalette::WindowText)
                        && chapterImage.pixelColor(contents.width() - 1, activeY)
                                == contents.palette().color(QPalette::WindowText),
                "The current leaf chapter does not fill its empty disclosure gutter");
        const int separator = button("duplicate")->geometry().bottom();
        require(chapterImage.pixelColor(0, separator) == kobalt::ui::dividerColor(&contents)
                        && chapterImage.pixelColor(contents.width() - 1, separator)
                                == kobalt::ui::dividerColor(&contents),
                "A leaf chapter's separator does not span the whole panel");
        require(!button("heading")->isEnabled() && button("headingToggle")->isEnabled(),
                "A heading without a destination cannot expand its children");
        require(button("headingToggle")->isChecked()
                        && button("headingToggle")->accessibleName().contains("Heading")
                        && !shown("currentToggle"),
                "Disclosure state, label or leaf visibility is wrong");
        require(button("aToggle")->geometry().right() < button("a")->geometry().left(),
                "Disclosure and title hit areas overlap");
        button("aToggle")->click();
        require(!shown("current") && shown("a") && shown("b") && selected.isEmpty(),
                "Collapsing a group navigated or kept descendants visible");
        button("aToggle")->click();
        require(shown("current") && button("headingToggle")->isChecked(),
                "Reopening a group forgot its nested expansion state");
        button("headingToggle")->click();
        require(!shown("current") && shown("other"),
                "Collapsing an unavailable heading hid a sibling");
        button("bToggle")->click();
        require(shown("last") && contents.findChild<QLabel *>("lastNumber")->text() == "07"
                        && contents.findChild<QLabel *>("lastPage")->text() == "32",
                "Filtering changed source numbering or page captions");
        button("last")->click();
        require(selected == "last", "A recycled row selected its old destination");
        button("a")->click();
        require(selected == "part-a", "A parent title toggled instead of navigating");
        contents.resize(720, 700);
        require(shown("aToggle") && !button("headingToggle")->isChecked(),
                "Resize reset chapter expansion");
        contents.showChapters("Nested book", chapters, {}, kobalt::ChapterGroups::All);
        require(shown("current") && button("aToggle")->isChecked()
                        && button("headingToggle")->isChecked(),
                "All chapters left a current-page group collapsed");
        button("rrContentsNext")->click();
        require(shown("last") && button("bToggle")->isChecked(),
                "All chapters left a later-page group collapsed");
        contents.showChapters("No collapsing", chapters, "current", kobalt::ChapterGroups::Off);
        require(shown("current") && button("a")->x() == 0 && !shown("aToggle")
                        && !shown("headingToggle"),
                "Off retained disclosure controls or collapsed chapters");
        contents.showChapters("Nested book", chapters, {});
        require(!shown("heading") && !shown("last") && contents.currentPage() == 0,
                "Reopening without a current entry retained expansion state");

        QVector<kobalt::Choice> many;
        for (int i = 0; i < 80; ++i)
            many.append({ QString::number(i),
                          QString::number(i),
                          QString("many%1").arg(i),
                          {},
                          i % 2,
                          i });
        contents.showChapters("Paged groups", many, "many79");
        require(shown("many79"), "The current child did not open on its visible page");
        if (!shown("many78Toggle"))
            button("rrContentsPrevious")->click();
        require(shown("many78Toggle"), "The preceding page lost the current chapter's parent");
        button("many78Toggle")->click();
        require(shown("many78") && !shown("many79"),
                "Last-page collapse lost its parent or left its child visible");
        contents.resize(1448, 1072);
        const auto caption = contents.findChild<QLabel *>("rrContentsPage")->text().split(" of ");
        require(caption.size() == 2 && caption[0].toInt() <= caption[1].toInt(),
                "Filtered contents resize left an invalid page");
        contents.showChapters("One group", many.mid(0, 2), {});
        require(!shown("rrContentsNext"), "Collapsed contents retained an unnecessary pager");
        const int twoRows = kobalt::ui::metrics::chrome::actionHeight
                + kobalt::ui::metrics::contents::stripHeight
                + 2 * kobalt::ui::metrics::list::oneLine;
        contents.resize(720, twoRows);
        auto group = many.mid(0, 4);
        group[2].depth = group[3].depth = 1;
        contents.showChapters("Pager threshold", group, {});
        button("many0Toggle")->click();
        require(shown("rrContentsNext"), "Expanding did not reserve space for the pager");
        button("rrContentsNext")->click();
        require(contents.currentPage() == 1, "Expanded contents did not page");
        button("rrContentsPrevious")->click();
        button("many0Toggle")->click();
        require(!shown("rrContentsNext") && contents.currentPage() == 0,
                "Collapsing did not reclaim the pager space");
        contents.showChapters("Empty", {}, {});
        for (auto *control : contents.findChildren<QPushButton *>())
            require(!control->isVisibleTo(&contents) || !control->objectName().endsWith("Toggle"),
                    "An empty list retained a disclosure button");
        contents.showChapters("Current group", chapters, "a");
        const auto groupImage = contents.grab().toImage();
        const int groupY = button("a")->geometry().center().y();
        require(groupImage.pixelColor(0, groupY) == contents.palette().color(QPalette::Window)
                        && groupImage.pixelColor(contents.width() - 1, groupY)
                                == contents.palette().color(QPalette::WindowText),
                "The current group's highlight covered its separate disclosure cell");
    }
    std::puts(
            "PASS: chapter groups, row highlights, paging and navigation in light and dark themes");
    {
        namespace chapterMetrics = kobalt::ui::metrics::chapters;
        namespace chrome = kobalt::ui::metrics::chrome;
        using kobalt::strings::get;
        QWidget host;
        host.resize(1072, 1448);
        // Match the font inherited from the production Reader window.
        host.setFont(kobalt::ui::interfaceFont());
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        kobalt::ChapterGraph graph(&host);
        graph.resize(1072, chapterMetrics::graphHeight);
        host.show();
        graph.open();
        QApplication::processEvents();
        auto *message = graph.findChild<QLabel *>("rrGraphMessage");
        auto *bars = graph.findChild<QWidget *>("rrChapterBars");
        auto *bookCell = graph.findChild<QWidget *>("rrGraphBookCell");
        auto text = [&](const char *name) { return graph.findChild<QLabel *>(name)->text(); };
        graph.setData({}, {}, false, {});
        require(message->isVisible() && message->text() == get("stats.counting")
                        && !bars->isVisible(),
                "Pending counts appeared as an empty graph");
        require(bookCell->isVisible(), "Page figures waited for word counts");
        graph.setData({}, {}, false, "Native count failed");
        require(message->text() == get("chapter_graph.chapter_lengths_are_unavailable")
                        && bookCell->isVisible(),
                "Failed counts remained in the loading state");
        QVector<kobalt::ChapterLink> chapters{ { "Short", "a", 0, 0, 100 },
                                               { "Long", "b", 1, 0, 400 } };
        QPalette colors = host.palette();
        colors.setColor(QPalette::Window, Qt::white);
        colors.setColor(QPalette::WindowText, Qt::black);
        colors.setColor(QPalette::Mid, QColor(128, 128, 128));
        graph.setPalette(colors);
        graph.setData(chapters, {}, true, {});
        QApplication::processEvents();
        const auto image = bars->grab().toImage();
        const int chartHeight = chapterMetrics::barsHeight,
                  barWidth = chapterMetrics::maximumBarWidth;
        const int groupLeft = (image.width() - 2 * barWidth - chapterMetrics::gapWide) / 2;
        require(qGray(image.pixel(groupLeft, chartHeight / 2)) == 255
                        && qGray(image.pixel(image.width() - groupLeft - barWidth, chartHeight / 2))
                                == 0,
                "Graph heights do not distinguish native word counts from equal page lengths");
        require(qGray(image.pixel(groupLeft - 1, chartHeight - 4)) == 255
                        && qGray(image.pixel(groupLeft, chartHeight - 4)) == 0
                        && qGray(image.pixel(image.width() - groupLeft, chartHeight - 4)) == 255,
                "Short-book bars exceed their width cap or are not centered");
        require(!graph.findChild<QPushButton *>("rrGraphNext")->isVisible(),
                "A short graph exposed paging controls");
        // Long chapters retain their ratios even when all exceed three times
        // the median. The maximum on another graph page still sets the scale.
        QVector<kobalt::ChapterLink> even;
        for (int i = 0; i < 21; ++i)
            even.append({ QString::number(i), QString::number(i), i, 0, 1000 });
        even[18].words = 6500;
        even[19].words = 9500;
        even[20].words = 10000;
        graph.setData(even, {}, true, {});
        QApplication::processEvents();
        require(graph.graphPages() == 1, "Twenty-one chapters did not fit on one dense chart");
        auto barHeight = [&](const QImage &image, int x) {
            for (int y = 0; y < chartHeight; ++y)
                if (qGray(image.pixel(x, y)) == 0)
                    return chartHeight - y;
            return 0;
        };
        const auto proportional = bars->grab().toImage();
        const int evenWidth = (proportional.width() - 20 * chapterMetrics::gapWide) / 21,
                  evenLeft = (proportional.width() - 21 * evenWidth - 20 * chapterMetrics::gapWide)
                / 2;
        auto barLeft = [&](int index) {
            return evenLeft + index * (evenWidth + chapterMetrics::gapWide);
        };
        require(barHeight(proportional, barLeft(18)) == int(0.65 * chartHeight)
                        && barHeight(proportional, barLeft(19)) == int(0.95 * chartHeight)
                        && barHeight(proportional, barLeft(20)) == chartHeight,
                "Unequal long chapters lost their proportions");
        graph.setDense(false);
        QApplication::processEvents();
        const auto firstPage = bars->grab().toImage();
        const int sparseWidth = (firstPage.width() - 19 * chapterMetrics::sparseGap) / 20;
        const int sparseLeft =
                (firstPage.width() - 20 * sparseWidth - 19 * chapterMetrics::sparseGap) / 2;
        require(barHeight(firstPage, sparseLeft + 19 * (sparseWidth + chapterMetrics::sparseGap))
                        == int(0.95 * chartHeight),
                "Paging normalized a chapter against only the visible chapters");
        graph.findChild<QPushButton *>("rrGraphNext")->click();
        QApplication::processEvents();
        const auto secondPage = bars->grab().toImage();
        require(barHeight(secondPage, (secondPage.width() - barWidth) / 2) == chartHeight,
                "The longest chapter lost its height on the final graph page");
        graph.setDense(true);
        even.last().words = 10000000;
        graph.setData(even, {}, true, {});
        QApplication::processEvents();
        const auto extreme = bars->grab().toImage();
        require(barHeight(extreme, barLeft(0)) == int(chapterMetrics::minimumBarHeight)
                        && barHeight(extreme, barLeft(20)) == chartHeight,
                "Short chapters lost their visible minimum or changed the longest chapter");
        require(!graph.findChild<QLabel *>("rrGraphLengthCaption"),
                "A length label occupies the chart padding");
        for (int i = 21; i < 80; ++i)
            even.append({ QString::number(i), QString::number(i), i, 0, 1000 });
        graph.setData(even, {}, true, {});
        require(graph.graphPages() == 1
                        && !graph.findChild<QPushButton *>("rrGraphNext")->isVisible(),
                "Eighty chapters did not fit on one dense chart");
        for (int i = 2; i < 180; ++i)
            chapters.append({ QString::number(i), QString::number(i), i, 0, i });
        graph.setData(chapters, { 120, 121, 2, 8 }, true, {});
        graph.open();
        require(graph.graphPage() == 1 && graph.graphPages() == 2,
                "A long dense graph did not page with narrow bars or open at the current chapter");
        auto *next = graph.findChild<QPushButton *>("rrGraphNext");
        require(next->isVisible(), "A paged graph hides its pager");
        graph.findChild<QPushButton *>("rrGraphPrevious")->click();
        while (next->isEnabled())
            next->click();
        require(graph.graphPage() == graph.graphPages() - 1,
                "Graph paging skipped its final entries");
        graph.resize(1448, chapterMetrics::graphHeight);
        QApplication::processEvents();
        require(graph.graphPage() < graph.graphPages(),
                "Resizing left the graph outside its page range");
        graph.resize(1072, chapterMetrics::graphHeight);
        // The sparse chart pages twenty chapters at a time.
        chapters.resize(21);
        graph.setDense(false);
        graph.setData(chapters, { 20, -1, 1, 1 }, true, {});
        graph.open();
        require(graph.graphPage() == 1 && graph.graphPages() == 2,
                "The 21st chapter did not get its own sparse page");
        require(bars->accessibleName()
                        == get("chapter_graph.chapters_to_of").arg(21).arg(21).arg(21),
                "Final graph page has the wrong accessible chapter range");
        QApplication::processEvents();
        const auto lastPage = bars->grab().toImage();
        int filled = 0;
        for (int x = 0; x < lastPage.width(); ++x)
            if (qGray(lastPage.pixel(x, chartHeight - 2)) < 255)
                ++filled;
        require(filled == barWidth, "A single final chapter expanded beyond the bar width cap");
        graph.setDense(true);
        require(graph.graphPages() == 1, "The dense chart kept the sparse pages");
        const QString longTitle =
                "A chapter with a very long name that needs more than one line of the Stats panel to display completely";
        graph.setProgress(longTitle, 4, 10, 12, 25, 100);
        QApplication::processEvents();
        auto *title = graph.findChild<QLabel *>("rrGraphChapterTitle");
        require(title->text().endsWith(QChar(0x2026)) && title->text().size() < longTitle.size()
                        && title->height() < 2 * title->fontMetrics().lineSpacing(),
                "A long chapter title did not elide on one line");
        graph.setProgress("The Second Voyage", 4, 10, 12, 25, 100);
        QApplication::processEvents();
        PaintCountingObserver graphPaints, barPaints, titlePaints;
        graph.installEventFilter(&graphPaints);
        bars->installEventFilter(&barPaints);
        title->installEventFilter(&titlePaints);
        graph.setData(chapters, { 20, -1, 1, 1 }, true, {});
        graph.setProgress("The Second Voyage", 4, 10, 12, 25, 100);
        QApplication::processEvents();
        require(graphPaints.paints == 0 && barPaints.paints == 0 && titlePaints.paints == 0,
                "Repeated Stats data and progress repainted the graph");
        auto changedChapters = chapters;
        ++changedChapters[20].words;
        graph.setData(changedChapters, { 20, -1, 1, 1 }, true, {});
        QApplication::processEvents();
        require(barPaints.paints > 0, "Changed chapter word count did not repaint the bars");
        graph.setData(chapters, { 20, -1, 1, 1 }, true, {});
        require(title->text() == "The Second Voyage", "The chapter title is missing");
        // Nickel's count: chapter page 4 of 10 is 40%, book page 26 of 100 is 26%.
        require(text("rrGraphChapterPercent") == get("common.percent").arg(40)
                        && text("rrGraphChapterFigure") == "7"
                        && text("rrGraphChapterUnit") == get("stats.pages_left"),
                "Chapter progress differs from the shared display rule");
        require(text("rrGraphChapterPosition") == get("stats.page_of").arg(4).arg(10),
                "Chapter page position is missing");
        require(text("rrGraphNextChapter")
                        == get("chapter_graph.next_chapter").arg(12).arg(get("common.pages")),
                "Next chapter length is missing");
        require(text("rrGraphBookPercent") == get("common.percent").arg(26),
                "Book progress disagrees with the current page");
        require(text("rrGraphBookFigure") == "75", "Book pages remaining omit the displayed page");
        graph.setChapterNumber(13, 24);
        graph.setReading(23, 35706);
        require(text("rrGraphChapterNumber") == get("stats.contents_position").arg(13).arg(24)
                        && text("rrGraphSessionFigure") == "23"
                        && text("rrGraphBookDetail") == get("stats.words").arg("35,706"),
                "Chapter number, session or word count is missing");
        for (int width : { 758, 1072, 1448 }) {
            graph.resize(width, chapterMetrics::graphHeight);
            graph.setProgress("Large book", 1, 3500, 3500, 1, 10000);
            QApplication::processEvents();
            for (const auto *name :
                 { "rrGraphChapterCell", "rrGraphBookCell", "rrGraphSessionCell" }) {
                const auto *cell = graph.findChild<QWidget *>(name);
                const auto *figure = cell->findChildren<QLabel *>().at(2),
                           *unit = cell->findChildren<QLabel *>().at(3),
                           *detail = cell->findChildren<QLabel *>().at(4);
                require(figure->width()
                                >= kobalt::qt::textWidth(figure->fontMetrics(), figure->text()),
                        "Stats clips a large figure");
                require(unit->geometry().right() < cell->width() - chrome::inset + 1,
                        "A figure unit runs past its cell");
                // The same padding sits above the caption and below the last line.
                require(cell->findChildren<QLabel *>().at(0)->y() == chapterMetrics::cellPadding
                                && detail->geometry().bottom() + 1 + chapterMetrics::cellPadding
                                        <= cell->height(),
                        "A Stats cell lost its padding");
            }
        }
        // Times at the session's pace fit on one line from the Clara up, and
        // wrap evenly on narrower screens instead of shortening.
        graph.setReading(23, 35706, 45 * 60000, (12 * 60 + 45) * 60000);
        for (int width : { 758, 1072, 1264 }) {
            graph.resize(width, chapterMetrics::graphHeight);
            QApplication::processEvents();
            for (const auto *name :
                 { "rrGraphChapterCell", "rrGraphBookCell", "rrGraphSessionCell" }) {
                const auto *detail =
                        graph.findChild<QWidget *>(name)->findChildren<QLabel *>().at(4);
                const auto lines = detail->text().split('\n');
                require(!detail->text().contains(QChar(0x2026))
                                && lines.size() <= chapterMetrics::detailLines
                                && (width < 1072 || lines.size() == 1),
                        "A Stats detail was cut off or wrapped on a wide screen");
                for (const auto &line : lines)
                    require(kobalt::qt::textWidth(detail->fontMetrics(), line) <= detail->width(),
                            "A Stats detail line is wider than its cell");
            }
        }
        require(text("rrGraphBookDetail")
                        == get("stats.time_left").arg(get("stats.duration_hours").arg(12).arg(45)),
                "Book time left is missing");
        // On the Nia the hours use a clock format instead of wrapping.
        graph.setReading(23, 35706, 45 * 60000, (12 * 60 + 5) * 60000);
        graph.resize(758, chapterMetrics::graphHeight);
        QApplication::processEvents();
        require(text("rrGraphBookDetail")
                                == get("stats.time_left")
                                           .arg(get("stats.duration_clock").arg(12).arg("05"))
                        && text("rrGraphNextChapter")
                                == get("stats.time_left")
                                           .arg(get("stats.duration_minutes").arg(45)),
                "A narrow cell did not use the clock format for hours only");
        graph.setReading(23, 35706);
        graph.resize(1072, chapterMetrics::graphHeight);
        // At its own density a Nia has the Clara's physical layout, so every
        // detail fits on one line, as it does on the Clara.
        for (const int ppi : { 212, 227 }) {
            kobalt::ui::setDensity(ppi);
            kobalt::ChapterGraph scaled(&host);
            scaled.resize(ppi == 212 ? 758 : 1404, chapterMetrics::graphHeight);
            scaled.setData(chapters, { 20, -1, 1, 1 }, true, {});
            scaled.setProgress("The Second Voyage", 4, 10, 12, 25, 100);
            scaled.setReading(23, 35706, 45 * 60000, (12 * 60 + 45) * 60000);
            scaled.open();
            QApplication::processEvents();
            require(scaled.height() >= int(chapterMetrics::graphHeight)
                            && int(chapterMetrics::cellHeight) == kobalt::ui::px(253),
                    "Stats did not scale its height");
            for (const auto *name :
                 { "rrGraphChapterCell", "rrGraphBookCell", "rrGraphSessionCell" }) {
                const auto *cell = scaled.findChild<QWidget *>(name);
                const auto labels = cell->findChildren<QLabel *>();
                require(cell->height() == int(chapterMetrics::cellHeight)
                                && labels.at(0)->y() == int(chapterMetrics::cellPadding),
                        "A scaled Stats cell lost its size or padding");
                require(!labels.at(4)->text().contains('\n')
                                && !labels.at(4)->text().contains(QChar(0x2026))
                                && kobalt::qt::textWidth(labels.at(4)->fontMetrics(),
                                                         labels.at(4)->text())
                                        <= labels.at(4)->width(),
                        "A Stats detail did not fit one line at its own density");
            }
        }
        kobalt::ui::setDensity(300);
        graph.setProgress("Last chapter", 1, 1, 0, 0, 1);
        require(text("rrGraphNextChapter") == get("stats.last_chapter"),
                "Last chapter still names a next chapter");
        // Without shared counts, Stats charts chapters by their length in pages.
        const auto lengths = kobalt::chapterPageLengths({ { "One", "a", 0 },
                                                          { "Two", "b", 4 },
                                                          { "Part", "c", 4 },
                                                          { "Lost", "d", -1 },
                                                          { "End", "e", 9 } },
                                                        12);
        require(lengths.size() == 5 && lengths[0].words == 4 && lengths[1].words == 5
                        && lengths[2].words == 5 && lengths[3].words == 0 && lengths[4].words == 3,
                "Page lengths did not follow the chapter starts");
        require(kobalt::chapterPageLengths({ { "Lost", "a", -1 } }, 12).isEmpty(),
                "Chapters without pages gave a chart");
        graph.setData({}, {}, true, {});
        require(message->isVisible()
                        && message->text()
                                == kobalt::strings::get(
                                        "chapter_graph.no_chapter_lengths_available")
                        && !bars->isVisible(),
                "Empty completed counts appeared as loading or stale bars");
    }
    {
        using namespace kobalt;
        namespace cm = ui::metrics::chapters;
        const QVector<ChapterLink> balanced{ { "One", "a", 0, 0, 1000 },
                                             { "Two", "b", 1, 0, 1000 },
                                             { "Three", "c", 2, 0, 1000 } };
        auto uneven = balanced;
        uneven.last().words = 10000;
        for (const int ppi : { 300, 227 }) {
            ui::setDensity(ppi);
            for (const qreal textScale : { 1.0, 1.5 }) {
                ui::setLargeText(textScale);
                for (const QSize design :
                     { QSize(1072, 1448), QSize(1264, 1680), QSize(1855, 2474), QSize(1448, 1072),
                       QSize(720, 1600), QSize(1600, 720) }) {
                    const bool phone = qMin(design.width(), design.height()) == 720;
                    if (phone && textScale > 1)
                        continue; // Phone mode supports 100% interface text.
                    const int controls = ui::px(phone ? 224 : 300);
                    QWidget host;
                    host.resize(ui::px(design.width()), ui::px(design.height()));
                    ChapterGraph graph(&host);
                    graph.resize(host.width(), cm::graphHeight);
                    graph.setAvailableHeight(host.height() - controls);
                    graph.setData(balanced, {}, true, {});
                    auto *bars = graph.findChild<QWidget *>("rrChapterBars");
                    host.show();
                    graph.open();
                    const int bottom = graph.geometry().bottom();
                    const int balancedHeight = bars->height();
                    graph.setData(uneven, {}, true, {});
                    require(graph.geometry().bottom() == bottom,
                            "Chart growth moved the bottom edge into the navigation bar");
                    const bool larger = qMin(design.width(), design.height()) > 1072;
                    require(larger ? bars->height() > balancedHeight
                                   : bars->height() == balancedHeight,
                            "Chart growth ignores physical screen size or chapter lengths");
                    require(graph.height() <= host.height() - controls,
                            "Stats overlaps the reserved toolbars");
                    require(bars->y() == int(cm::rowHeight) + int(cm::barsTop),
                            "The graph lost its top padding");
                    for (const auto *name :
                         { "rrGraphChapterCell", "rrGraphBookCell", "rrGraphSessionCell" })
                        require(graph.findChild<QWidget *>(name)->height() == int(cm::cellHeight),
                                "Chart growth shrank a Stats cell");
                }
            }
        }
        ui::setDensity(300);
        ui::setLargeText(1);
    }
    {
        using namespace kobalt;
        using namespace kobalt::ui;
        QWidget host;
        host.resize(1072, 1448);
        host.show();
        ChoiceScreen picker(&host);
        QVector<Choice> choices;
        for (int i = 0; i < 25; ++i)
            choices.append({ QString("Font %1").arg(i), QString("Family %1").arg(i),
                             QString("font%1").arg(i) });
        const auto bytesBeforeBrowsing = privateInterfaceFontBytes();
        picker.setDropdownGeometry(QRect(0, 200, 1072, picker.fontPanelHeight()));
        picker.showChoices("Font", choices, {}, ChoiceScreen::Mode::Fonts);
        auto *next = picker.findChild<QPushButton *>("rrFontPagerNext");
        auto *previous = picker.findChild<QPushButton *>("rrFontPagerPrevious");
        require(next && previous, "Font picker lacks paging controls");
        for (int page = 0; page < 3; ++page) {
            app.processEvents();
            int visible = 0;
            QVector<QRect> rectangles;
            for (int i = 0; i < choices.size(); ++i) {
                auto *button = picker.findChild<QPushButton *>(choices[i].name);
                if (!button || !button->isVisible())
                    continue;
                require((i / 12) == page, "Grid skipped or repeated a font");
                const auto *name = picker.findChild<QLabel *>(choices[i].name + "Title");
                require(name && name->font().family() == chromeFont().family(),
                        "Font browsing used the candidate family to paint its name");
                require(picker.rect().contains(button->geometry()), "Grid cell escaped the panel");
                for (const auto &rect : rectangles)
                    require(!rect.intersects(button->geometry()), "Grid cells overlap");
                rectangles.append(button->geometry());
                ++visible;
            }
            require(visible == (page < 2 ? 12 : 1), "Grid page capacity is wrong");
            require(next->isEnabled(), "Grid last-page boundary is wrong");
            if (page < 2)
                next->click();
        }
        for (int turn = 0; turn < 2; ++turn)
            previous->click();
        previous->click();
        next->click();
        for (const QSize size : { QSize(1072, picker.fontPanelHeight()), QSize(600, 450) }) {
            picker.setFontGeometry(QRect(QPoint(), size));
            app.processEvents();
            for (int i = 0; i < 12; ++i) {
                auto *cell = picker.findChild<QPushButton *>(choices[i].name);
                require(cell && cell->isVisible() && picker.rect().contains(cell->geometry()),
                        "Grid clips a cell after resize");
            }
        }
        require(privateInterfaceFontBytes() == bytesBeforeBrowsing,
                "Browsing or resizing the font picker retained candidate font bytes");
        require(picker.findChild<QPushButton *>("font0")->width() == picker.width() / 3
                        && picker.findChild<QPushButton *>("font1")->width() == picker.width() / 3,
                "Default slots must each occupy a third of a row");
        bool finished = false;
        picker.back = [&] { finished = true; };
        auto *done = picker.findChild<QPushButton *>("rrFontClose");
        require(done && done->isVisible(), "Missing close button");
        require(done->text().isEmpty() && done->width() <= metrics::choices::pickerHeader,
                "Close cell is not a compact icon button");
        for (const bool pressed : { false, true }) {
            done->setDown(pressed);
            const auto image = done->grab().toImage();
            const auto background =
                    done->palette().color(pressed ? QPalette::WindowText : QPalette::Window);
            for (int x = image.width() / 4; x < 3 * image.width() / 4; ++x)
                require(QColor::fromRgba(image.pixel(x, image.height() - 1)) == background,
                        "Close button draws a stray bottom rule");
        }
        done->setDown(false);
        done->click();
        require(finished, "Close button did not close the picker");
        const QVector<Choice> sections{
            { "Libron", "Libron", "recommended", "Recommended Fonts" },
            { "Ancizar Serif", "NV Ancizar Serif", "ancizar", "Recommended Fonts" },
            { "System", "System", "system", "Built-in Fonts" },
            { "Personal", "Personal", "personal", "Sideloaded Fonts" },
            { "Arabic", "Noto Naskh Arabic", "arabic", "Supplemental Fonts" }
        };
        picker.setFontGeometry(QRect(0, 0, 1072, picker.fontPanelHeight()));
        picker.showChoices("Font", sections, "NV Ancizar Serif", ChoiceScreen::Mode::Fonts);
        auto *section = picker.findChild<QLabel *>("rrFontSection");
        require(section && section->text() == "RECOMMENDED FONTS",
                "Font section heading is missing");
        require(picker.findChild<QPushButton *>("ancizar")->isChecked(),
                "Display name changed the stored selection");
        const auto selectedCell = picker.findChild<QPushButton *>("ancizar")->grab().toImage();
        int markerRows = 0;
        for (int y = selectedCell.height() - ui::px(44); y < selectedCell.height() - ui::px(12);
             ++y) {
            bool visibleInk = false;
            for (int x = selectedCell.width() - ui::px(44); x < selectedCell.width() - ui::px(12);
                 ++x)
                visibleInk |= QColor::fromRgba(selectedCell.pixel(x, y))
                        == picker.palette().color(QPalette::Window);
            if (visibleInk)
                ++markerRows;
        }
        require(markerRows >= ui::px(10), "Font label covers the active-font checkmark");
        QString chosen;
        picker.selected = [&](const QString &value) { chosen = value; };
        picker.findChild<QPushButton *>("ancizar")->click();
        require(chosen == "NV Ancizar Serif" && fontDisplayName(chosen) == "Ancizar Serif",
                "Picker changed the font identifier");
        for (int i = 2; i < sections.size(); ++i) {
            next->click();
            app.processEvents();
            require(section->text() == sections[i].section.toUpper(),
                    "Pager skipped a font section");
            auto *cell = picker.findChild<QPushButton *>(sections[i].name);
            require(cell && cell->isVisible(), "Section page lost its first font");
            cell->click();
            require(chosen == sections[i].value, "Section page chose a font from another page");
        }
        require(next->isEnabled(), "Supplemental page cannot wrap to the first section");
        const auto sparse = picker.grab().toImage();
        const auto paper = picker.palette().color(QPalette::Window);
        const auto ink = picker.palette().color(QPalette::WindowText);
        for (int y = 0; y < metrics::chrome::heavyRule; ++y)
            for (int x = 0; x < sparse.width(); ++x)
                require(QColor::fromRgba(sparse.pixel(x, y)) == ink,
                        "Font panel top rule has a gap");
        const int emptyY = picker.height() / 2;
        for (const int x : { 0, sparse.width() - 1 })
            require(QColor::fromRgba(sparse.pixel(x, emptyY)) == paper,
                    "Sparse font page has an outer side border");
        require(QColor::fromRgba(
                        sparse.pixel(sparse.width() / 2, metrics::choices::pickerHeader - 1))
                        == paper,
                "Font header has an unwanted bottom rule");
        choices[3].label = "Atkinson Hyperlegible Next";
        choices[3].value = "Libron";
        picker.showChoices("Font", choices, {}, ChoiceScreen::Mode::Fonts);
        auto *longName = dynamic_cast<SampleLabel *>(picker.findChild<QWidget *>("font3Title"));
        require(longName && longName->isVisible(), "Long font name is missing from the grid");
        const auto namePixels =
                longName->parentWidget()->grab().toImage().copy(longName->geometry());
        int nameBands = 0;
        bool previousInk = false;
        for (int y = 0; y < namePixels.height(); ++y) {
            bool ink = false;
            for (int x = 0; x < namePixels.width(); ++x)
                ink |= qGray(namePixels.pixel(x, y)) < 128;
            if (ink && !previousInk)
                ++nameBands;
            previousInk = ink;
        }
        require(nameBands == 2, "The font grid leaves insufficient height for a two-line name");
        choices[2].placeholder = true;
        picker.showChoices("Font", choices, {}, ChoiceScreen::Mode::Fonts);
        require(!picker.findChild<QLabel *>("font2Title")->isVisible(),
                "Placeholder label covers its circle");
        picker.showChoices("Header size",
                           { { "Off", "off", "sizeOff" },
                             { "Small", "small", "sizeSmall" },
                             { "Large", "large", "sizeLarge" } },
                           "small", ChoiceScreen::Mode::Sizes);
        require(picker.findChild<QLabel *>("sizeLargeTitle")->isVisible(),
                "Reused placeholder cell hid a size label");
    }
    {
        using namespace kobalt;
        using namespace kobalt::ui;
        QWidget host;
        host.resize(1072, 1448);
        host.show();
        ChoiceScreen picker(&host);
        picker.setGeometry(20, 20, 600, 4 * metrics::choices::fontRowHeight);
        const QVector<Choice> choices{ { "Off", "off", "headerOff" },
                                       { "Small", "small", "headerSmall" },
                                       { "Medium", "medium", "headerMedium" },
                                       { "Large", "large", "headerLarge" } };
        picker.showChoices("Header size", choices, "large", ChoiceScreen::Mode::Sizes);
        app.processEvents();
        // The size dropdown has a two-pixel outline above its first choice.
        int bottom = 2;
        for (const auto &choice : choices) {
            auto *button = picker.findChild<QPushButton *>(choice.name);
            require(button && button->isVisible(), "Size selector hid an available choice");
            require(button->geometry().top() == bottom, "Size selector left a gap between choices");
            bottom = button->geometry().bottom() + 1;
        }
        require(picker.findChild<QPushButton *>("headerLarge")->isChecked(),
                "Size selector lost its selected value");
        auto *current = dynamic_cast<ListRow *>(picker.findChild<QWidget *>("headerLarge"));
        require(current && current->titleLabel()->font().pixelSize() == int(metrics::text::body),
                "Size selector text is smaller than its control");
        const auto selectedImage = current->grab().toImage();
        current->setAccessory(ListRow::None{});
        require(current->grab().toImage() != selectedImage,
                "Size selector has no checkmark on its current choice");
        require(!picker.findChild<QPushButton *>("rrChoicesNext")->isVisible()
                        && !picker.findChild<QPushButton *>("rrChoicesPrevious")->isVisible(),
                "Size selector exposed paging controls");
    }
    // Compare visible mixed-case ink across the bundled families, independent
    // of their claimed font metrics. Use the same sample sizing as the picker.
    QStringList readingFamilies;
    for (const auto &file :
         QDir(kobalt::test::readerAssets().file("fonts/ebook-core")).entryList({ "*.ttf" })) {
        const auto id = QFontDatabase::addApplicationFont(kobalt::test::readerFontCopies().file(
                "ebook-core/" + file, kobalt::test::readerAssets().fontData("ebook-core/" + file)));
        require(id >= 0, "Core font could not be registered for optical check");
        const auto families = QFontDatabase::applicationFontFamilies(id);
        for (const auto &family : families)
            require(kobalt::ui::registerPrivateInterfaceFont(
                            kobalt::test::readerFontCopies().file(
                                    "ebook-core/" + file,
                                    kobalt::test::readerAssets().fontData("ebook-core/" + file)),
                            family),
                    "Core font could not be retained for optical check");
        readingFamilies.append(families);
    }
    readingFamilies.removeDuplicates();
    for (const auto &face : readingFamilies) {
        const auto font = kobalt::ui::fontSample(face);
        require(QFontInfo(font).family() == face, "Optical font check used a fallback");
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.setFont(font);
            painter.setPen(Qt::black);
            const auto glyphs = kobalt::ui::renderPrivateText("Hox", font, 0);
            require(glyphs.missingGlyphs == 0, "Optical sample lost a glyph");
            painter.drawImage(QPoint(10, 70) + glyphs.origin, glyphs.image);
        }
        int top = 100, bottom = -1;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qGray(image.pixel(x, y)) < 128) {
                    top = qMin(top, y);
                    bottom = y;
                    break;
                }
        require(qAbs(bottom - top + 1 - kobalt::ui::metrics::choices::sampleInkHeight) <= 2,
                "Font preview has inconsistent visible size");
    }
    const auto nativeFont = QApplication::font();
    // Regular and Bold share their legacy family names on Qt 5 and Qt 6.
    // The interface registers the bundled faces unchanged.
    const QString nativeFamily = "IBM Plex Sans";
    const QString chromeFamily = "IBM Plex Mono";
    require(kobalt::ui::interfaceFontFamily() == nativeFamily,
            "The interface did not register Plex Sans");
    require(QFontInfo(kobalt::ui::interfaceFont()).family() == nativeFamily,
            "The interface resolved another font");
    require(QFontInfo(kobalt::ui::chromeFont()).family() == chromeFamily,
            "Chrome did not register Plex Mono");
    require(QFontInfo(kobalt::ui::interfaceFont()).weight() == QFont::Normal,
            "Controls did not resolve Plex Regular");
    require(QFontInfo(kobalt::ui::chromeFont()).weight() == QFont::Normal,
            "Chrome did not resolve Plex Regular");
    auto boldUi = kobalt::ui::interfaceFont();
    boldUi.setBold(true);
    require(QFontInfo(boldUi).family() == nativeFamily,
            "Bold interface text resolved another family");
    require(QFontInfo(boldUi).weight() == QFont::Bold,
            "Bold interface text did not resolve Plex Bold");
    require(QApplication::font() == nativeFont,
            "The interface changed the native application font");

    // Heavier interface text applies to shown widgets that the private
    // renderer draws. Layout stays the same, and switching back draws the
    // same pixels. Text that Qt draws itself keeps the regular faces.
    {
        using namespace kobalt::ui;
        QWidget host;
        host.setFont(interfaceFont());
        // As the reader window does, so style sheet fonts stay unhinted on Qt 5.2.
        keepInterfaceUnhinted(&host);
        readerStyle(&host, styles::reader());
        auto *plain = new InterfaceLabel("Swipe along edges to adjust frontlight", &host);
        plain->setGeometry(10, 10, 880, 60);
        auto *rich = new InterfaceLabel("<b>Chapter</b> 12 of 40", &host);
        rich->setTextFormat(Qt::RichText);
        rich->setGeometry(10, 80, 880, 60);
        auto *caption = new InterfaceLabel("12 pages left", &host);
        caption->setFont(captionFont());
        readerStyle(caption, styles::caption());
        caption->setGeometry(10, 150, 880, 60);
        auto *brand = new InterfaceLabel("Kobalt", &host);
        readerStyle(brand, styles::brand(48));
        brand->setGeometry(10, 220, 880, 90);
        // A label's own style sheet keeps the font of its rules.
        auto *styled = new InterfaceLabel("12 pages left", &host);
        readerStyle(styled, styles::caption());
        styled->setGeometry(10, 150, 880, 60);
        auto *explicitFont = new InterfaceLabel("12 pages left", &host);
        explicitFont->setFont(chromeFont(22));
        explicitFont->setGeometry(10, 150, 880, 60);
        auto *native = new QLabel("Swipe along edges to adjust frontlight", &host);
        native->setGeometry(10, 240, 880, 60);
        // Dictionary and link panels keep a laid-out document.
        auto *paged = new PagedText(&host);
        paged->setTextFont(interfaceFont(28));
        paged->setHtml("<p>Swipe along edges to adjust frontlight</p>");
        paged->setGeometry(10, 150, 880, 120);
        const QVector<QWidget *> labels{ plain, rich, caption, brand, styled, explicitFont, paged };
        host.resize(900, 320);
        host.show();
        app.processEvents();
        // Render on white: captions have a transparent background.
        const auto ink = [](QWidget *widget) {
            QImage image(widget->size(), QImage::Format_RGB32);
            image.fill(Qt::white);
            widget->render(&image);
            double sum = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    sum += 255 - qGray(image.pixel(x, y));
            return sum / 255;
        };
        const auto measure = [&] {
            QVector<double> values;
            for (auto *widget : labels)
                values.append(ink(widget));
            return values;
        };
        const auto regular = measure();
        const auto nativeInk = ink(native);
        const auto hint = plain->sizeHint();
        const auto richHint = rich->sizeHint();
        QVector<QRect> rectangles;
        QVector<QString> families;
        QVector<int> fontSizes;
        for (auto *widget : labels) {
            rectangles.append(widget->geometry());
            families.append(QFontInfo(widget->font()).family());
            fontSizes.append(widget->font().pixelSize());
        }
        const auto fits = [](QWidget *widget) {
            const auto hint = widget->sizeHint();
            return hint.width() <= widget->width() && hint.height() <= widget->height();
        };
        require(fits(plain) && fits(rich), "Regular text does not fit its widget");
        const auto regularPixels = plain->grab().toImage();
        require(interfaceWeight() == 0, "Regular font fixture changed its weight");
        kobalt::ChapterGraph graph(&host);
        graph.resize(900, kobalt::ui::metrics::chapters::graphHeight);
        graph.setProgress("A chapter with a title that needs layout", 4, 10, 12, 25, 100);
        auto *graphTitle = graph.findChild<QLabel *>("rrGraphChapterTitle");
        for (int round = 0; round < 2; ++round) {
            require(setInterfaceWeight(heavierTextSteps), "Heavier text did not replace the faces");
            require(!setInterfaceWeight(heavierTextSteps),
                    "The same weight replaced the faces again");
            refreshInterfaceText(&host);
            app.processEvents();
            graphTitle->setGeometry(0, 0, 1, 1);
            graph.setProgress("A chapter with a title that needs layout", 4, 10, 12, 25, 100);
            require(graphTitle->width() == graph.width() - 2 * int(metrics::chrome::inset),
                    "Heavier interface text did not lay out unchanged Stats progress");
            require(interfaceWeight() == heavierTextSteps
                            && QFontInfo(interfaceFont()).family() == nativeFamily
                            && QFontInfo(chromeFont()).family() == chromeFamily,
                    "Heavier text changed the family");
            const auto heavier = measure();
            require(ink(native) == nativeInk, "Qt drew its own text heavier");
            require(paged->textFont().pixelSize() == 28,
                    "Paged text lost its size when the faces changed");
            for (int index = 0; index < labels.size(); ++index) {
                std::printf("interface weight: widget %d ink %+.1f%%\n", index,
                            100 * (heavier[index] / regular[index] - 1));
                require(heavier[index] > regular[index] * 1.02
                                && heavier[index] < regular[index] * 1.3,
                        "A widget did not draw moderately heavier text");
            }
            for (int index = 0; index < labels.size(); ++index) {
                require(labels[index]->geometry() == rectangles[index],
                        "Heavier text moved a widget");
                require(QFontInfo(labels[index]->font()).family() == families[index]
                                && labels[index]->font().pixelSize() == fontSizes[index],
                        "Heavier text changed a widget font family or size");
            }
            require(fits(plain) && fits(rich), "Heavier text does not fit its widget");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            // Qt5 grid fitting can change advances without changing the stored
            // font metrics.
            require(plain->sizeHint().height() == hint.height()
                            && rich->sizeHint().height() == richHint.height(),
                    "Heavier text changed a line height");
#else
            require(plain->sizeHint() == hint && rich->sizeHint() == richHint,
                    "Heavier text changed the layout");
#endif
            require(setInterfaceWeight(0), "Regular text did not replace the faces");
            refreshInterfaceText(&host);
            app.processEvents();
            graphTitle->setGeometry(0, 0, 1, 1);
            graph.setProgress("A chapter with a title that needs layout", 4, 10, 12, 25, 100);
            require(graphTitle->width() == graph.width() - 2 * int(metrics::chrome::inset),
                    "Regular interface text did not lay out unchanged Stats progress");
            require(plain->grab().toImage() == regularPixels && measure() == regular,
                    "Regular text did not return to its pixels");
        }
    }

    require(kobalt::ui::metrics::reading::textPixels(24, false) == 24
                    && kobalt::ui::metrics::reading::textPixels(24, true) == 24
                    && kobalt::ui::metrics::reading::textPixels(26, false) == 26
                    && kobalt::ui::metrics::reading::textPixels(26, true) == 26
                    && kobalt::ui::metrics::reading::textPixels(28, false) == 28
                    && kobalt::ui::metrics::reading::textPixels(28, true) == 28,
            "Header and footer checkpoint sizes must match");
    require(kobalt::ui::metrics::reading::stripHeight(24) == 48
                    && kobalt::ui::metrics::reading::stripHeight(26) == 52
                    && kobalt::ui::metrics::reading::stripHeight(28) == 56
                    && kobalt::ui::metrics::reading::stripHeight(48) == 96
                    && kobalt::ui::metrics::reading::textBandHeight(48) == 88,
            "Chrome strips do not reserve enough room for large text");
    // Exercise the final styled layout, not only the constructor's size hints.
    {
        using namespace kobalt::ui;
        const auto originalStyle = app.styleSheet();
        const int originalDensity = density();
        app.setStyleSheet("QSlider { min-height: 80px; max-height: 80px; }");
        for (const int ppi : { 167, 212, 300 }) {
            setDensity(ppi);
            QWidget host;
            // Wide enough for a side caption at every density; narrower rows
            // stack their caption above the controls and grow taller.
            host.resize(900, metrics::control::stepSize);
            auto *rows = new QVBoxLayout(&host);
            rows->setContentsMargins(0, 0, 0, 0);
            auto *slider = sliderRow(&host, rows, "density-slider", "density-value", "Size", "less",
                                     "more", 0, 100, 1, {});
            host.show();
            app.processEvents();
            require(slider->height() == int(metrics::control::sliderHeight),
                    "Setting slider inherits Nickel's unscaled height");
            require(slider->parentWidget()->rect().contains(slider->geometry()),
                    "Setting slider leaves its row");
            require(slider->visibleRegion() == QRegion(slider->rect()),
                    "Setting slider handle is clipped by its row");
            host.hide();
        }
        app.setStyleSheet(originalStyle);
        setDensity(originalDensity);
    }
    // A stylesheet repolish previously flattened label-only stepper buttons.
    for (const bool dark : { false, true })
        for (const auto size : { QSize(1072, 1448), QSize(1448, 1072) }) {
            using namespace kobalt::ui;
            QWidget host;
            host.setProperty("reader_dark", dark);
            host.resize(size);
            readerStyle(&host, styles::reader());
            auto *rows = new QVBoxLayout(&host);
            rows->setContentsMargins(metrics::panel::contentMargins);
            rows->setSpacing(metrics::control::rowGap);
            auto *level = sliderRow(&host, rows, "level", "value", "Brightness", "less", "more", 0,
                                    100, 1, {});
            auto *actions = horizontalRow();
            rows->addLayout(actions);
            auto *keep = new Button("Keep settings", &host);
            auto *restore = new Button("Restore defaults", &host);
            readerStyle(keep, styles::outlined());
            readerStyle(restore, styles::primary());
            actions->addWidget(keep);
            actions->addWidget(restore);
            rows->addStretch(1);
            host.show();
            app.processEvents();
            for (int pass = 0; pass < 2; ++pass) {
                readerStyle(&host, styles::reader());
                for (auto *widget : host.findChildren<QWidget *>())
                    if (widget->property("reader_style").isValid())
                        readerStyle(widget, widget->property("reader_style").toString());
                level->hide();
                app.processEvents();
                level->show();
                app.processEvents();
                for (auto *button :
                     { static_cast<StepButton *>(host.findChild<QPushButton *>("less")),
                       static_cast<StepButton *>(host.findChild<QPushButton *>("more")) }) {
                    require(button->size() == QSize(72, 72), "Styled step button is not square");
                    int clicks = 0;
                    const auto connection =
                            QObject::connect(button, &QPushButton::clicked, [&] { ++clicks; });
                    for (const auto point :
                         { QPoint(2, 2), QPoint(69, 2), QPoint(2, 69), QPoint(69, 69) }) {
                        QMouseEvent press(QEvent::MouseButtonPress, point,
                                          button->mapToGlobal(point), Qt::LeftButton,
                                          Qt::LeftButton, Qt::NoModifier);
                        QMouseEvent release(QEvent::MouseButtonRelease, point,
                                            button->mapToGlobal(point), Qt::LeftButton,
                                            Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(button, &press);
                        QApplication::sendEvent(button, &release);
                    }
                    QObject::disconnect(connection);
                    require(clicks == 4, "Stepper corners do not accept taps");
                    require(host.rect().contains(
                                    QRect(button->mapTo(&host, QPoint()), button->size())),
                            "Step button leaves its panel");
                }
                require(keep->height() == restore->height() && keep->y() == restore->y(),
                        "Paired actions have different heights or alignment");
            }
            host.hide();
        }
    {
        using namespace kobalt::ui;
        BackButton back("Back", nullptr);
        back.setFont(kobalt::ui::interfaceFont());
        back.resize(192, 68);
        // Native styles may supply ButtonText independently of reader text.
        // The custom arrow and caption must both use the reader's ink role.
        QPalette colors = back.palette();
        colors.setColor(QPalette::Window, Qt::white);
        colors.setColor(QPalette::WindowText, Qt::black);
        colors.setColor(QPalette::ButtonText, Qt::white);
        back.setPalette(colors);
        readerStyle(&back, "QPushButton{background:white;border:none;padding:0;}");
        back.show();
        app.processEvents();
        const auto image = back.grab().toImage();
        int captionInk = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = metrics::navigation::labelLeft; x < image.width(); ++x)
                captionInk += qGray(image.pixel(x, y)) < 128;
        require(captionInk > 50,
                "Back caption follows the native button palette instead of reader ink");
    }
    {
        using namespace kobalt::ui;
        const auto font = interfaceFont(metrics::chrome::descriptionText);
        const QString text = "Decide what this hardware button should do when you're reading.";
        const int width = interfaceTextWidth(text, font) * 3 / 4;
        const auto balanced = balancedInterfaceText(text, font, width);
        const auto lines = balanced.split('\n');
        require(lines.size() == 2, "Balancing added unnecessary lines");
        require(balanced.simplified() == text.simplified(), "Balancing changed the words");
        require(qAbs(interfaceTextWidth(lines[0], font) - interfaceTextWidth(lines[1], font))
                        < width / 3,
                "Balanced instructions leave a short orphan line");
        for (const auto &line : lines)
            require(interfaceTextWidth(line, font) <= width,
                    "Balanced text exceeds its available width");
        require(balancedInterfaceText(text, font, interfaceTextWidth(text, font)) == text,
                "A single-line heading was changed");
        require(balancedInterfaceText("First paragraph.\n\nSecond paragraph.", font, width)
                        == "First paragraph.\n\nSecond paragraph.",
                "Balancing removed an explicit paragraph break");
        const QString japanese =
                QString::fromUtf8("読書中にこのボタンで何をするか選んでください。");
        auto wrapped = balancedInterfaceText(japanese, font, width / 2);
        require(wrapped.remove('\n') == japanese, "Balancing changed text without word spaces");
    }

    QWidget optionsHost;
    kobalt::ui::readerStyle(&optionsHost, kobalt::ui::styles::reader());
    kobalt::ui::DescribedOption option(
            "Enable page animations",
            "Sweep between text pages. Images and colour keep their usual refresh.", &optionsHost);
    optionsHost.show();
    option.show();
    app.processEvents();
    for (int width : { 240, 900, 1250 }) {
        option.resize(width, option.heightForWidth(width));
        const auto reference = option.grab().toImage();
        int firstInk = -1, lastInk = -1;
        for (int y = 0; y < reference.height(); ++y)
            for (int x = 60; x < reference.width(); ++x)
                if (qGray(reference.pixel(x, y)) < 128) {
                    if (firstInk < 0)
                        firstInk = y;
                    lastInk = y;
                }
        require(firstInk >= kobalt::ui::px(12) && lastInk >= 0
                        && lastInk < reference.height() - kobalt::ui::px(12),
                "Setting description is too close to a row edge");
    }
    {
        kobalt::ui::DescribedOption sample("Pages", "", &optionsHost);
        auto look = sample.presentation();
        look.valueBelowLabel = true;
        look.leadingIndicator = true;
        sample.setPresentation(look);
        sample.resize(720, sample.heightForWidth(720));
        sample.show();
        const auto firstValueInk = [&](bool halfSpaces) {
            kobalt::ui::DescribedOption::ValueStyle style;
            style.halfSpaces = halfSpaces;
            sample.setValue("123 456", style);
            sample.resize(720, sample.heightForWidth(720));
            app.processEvents();
            const auto image = sample.grab().toImage();
            for (int y = 0; y < image.height(); ++y)
                for (int x = image.width() / 2; x < image.width() - 4; ++x)
                    if (qGray(image.pixel(x, y)) < 128)
                        return y;
            return -1;
        };
        const int normal = firstValueInk(false), spaced = firstValueInk(true);
        require(normal > 0 && qAbs(normal - spaced) <= 1,
                "Half-spaced page values must stay below their label");
    }
    {
        // Standard text keeps the description at its design offset below the
        // title; only large text makes the title line taller.
        namespace metrics = kobalt::ui::metrics;
        const auto look = option.presentation();
        QFont description = option.font();
        description.setPixelSize(look.descriptionPixels);
        description.setWeight(QFont::Normal);
        description.setItalic(false);
        const int width = 900,
                  textWidth = width - 2 * look.horizontalInset - metrics::settings::labelLeft
                - look.indicatorSize.width() - metrics::settings::toggleGap;
        require(option.heightForWidth(width)
                        == qMax(look.minimumRowHeight,
                                metrics::settings::descriptionTop
                                        + metrics::settings::descriptionBottom
                                        + 2 * metrics::settings::descriptionPadding
                                        + kobalt::ui::wrappedInterfaceTextHeight(
                                                option.accessibleDescription(), description,
                                                textWidth)),
                "Standard text moved the setting description");
    }
    {
        // A radio label's capitals center on its circle at every text size.
        // A word without descenders shows where the capitals sit.
        namespace metrics = kobalt::ui::metrics;
        const auto capitalsBelowCircle = [&optionsHost, &app](qreal scale) {
            kobalt::ui::setLargeText(scale);
            kobalt::ui::RadioList list(&optionsHost);
            list.add("radioCentering", "HIDE");
            list.resize(700, list.heightForWidth(700));
            list.show();
            app.processEvents();
            const auto image = list.grab().toImage();
            const auto inkRows = [&image](int left, int right) {
                int first = -1, last = -1;
                for (int y = 0; y < image.height() - 2; ++y)
                    for (int x = left; x < right; ++x)
                        if (qGray(image.pixel(x, y)) < 128) {
                            if (first < 0)
                                first = y;
                            last = y;
                            break;
                        }
                return (first + last) / 2.0;
            };
            const int circleRight = metrics::chrome::inset + metrics::chrome::checkSize;
            return inkRows(circleRight + metrics::settings::toggleGap, image.width())
                    - inkRows(metrics::chrome::inset, circleRight);
        };
        const qreal standard = capitalsBelowCircle(1), large = capitalsBelowCircle(1.5);
        kobalt::ui::setLargeText(1);
        require(qAbs(standard) <= 1, "A radio label is not centered on its circle");
        require(qAbs(large) <= 1, "Large text moved a radio label away from its circle");
        // A value on the label's line keeps its place while the label is
        // raised, so values line up whatever their labels' descenders.
        const auto valueBelowCircle = [&optionsHost, &app](const QString &label) {
            kobalt::ui::RadioList list(&optionsHost);
            list.add("radioValue", label)->setValue("VALUE");
            list.resize(700, list.heightForWidth(700));
            list.show();
            app.processEvents();
            const auto row = list.grab().toImage();
            const auto inkMiddle = [&row](int left, int right) {
                int first = -1, last = -1;
                for (int y = 0; y < row.height() - 2; ++y)
                    for (int x = left; x < right; ++x)
                        if (qGray(row.pixel(x, y)) < 128) {
                            if (first < 0)
                                first = y;
                            last = y;
                            break;
                        }
                return (first + last) / 2.0;
            };
            const int circleRight = metrics::chrome::inset + metrics::chrome::checkSize;
            return inkMiddle(row.width() / 2, row.width())
                    - inkMiddle(metrics::chrome::inset, circleRight);
        };
        require(qAbs(valueBelowCircle("Title") - valueBelowCircle("Jumpy")) < 1,
                "A radio row's value moved with its raised label");
    }
    kobalt::ui::DescribedOption compactOption("Automatic warmth", {}, &optionsHost);
    compactOption.resize(700, compactOption.heightForWidth(700));
    compactOption.show();
    app.processEvents();
    require(compactOption.heightForWidth(700) == kobalt::ui::metrics::control::stepSize,
            "Caption-only checkbox retained description spacing");
    const QPointF checkboxPoint(compactOption.width() - 8, compactOption.height() / 2);
    QMouseEvent checkboxDown(QEvent::MouseButtonPress, checkboxPoint, checkboxPoint, Qt::LeftButton,
                             Qt::LeftButton, Qt::NoModifier);
    QMouseEvent checkboxUp(QEvent::MouseButtonRelease, checkboxPoint, checkboxPoint, Qt::LeftButton,
                           Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&compactOption, &checkboxDown);
    QApplication::sendEvent(&compactOption, &checkboxUp);
    require(compactOption.isChecked(), "Checkbox rejected a tap in the empty part of its row");
    optionsHost.hide();
    {
        using namespace kobalt::ui;
        const int savedDensity = density();
        for (int ppi : { 167, 212, 300 }) {
            setDensity(ppi);
            for (bool dark : { false, true }) {
                QWidget host;
                host.setProperty("reader_dark", dark);
                QPalette palette = host.palette();
                palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
                palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
                host.setPalette(palette);
                host.resize(px(900), px(700));
                host.show();
                PaintCountingOption option("A setting",
                                           "A description that wraps at narrow widths.", &host);
                int clicks = 0, toggles = 0;
                QObject::connect(&option, &QCheckBox::clicked, &host, [&] { ++clicks; });
                QObject::connect(&option, &QCheckBox::toggled, &host, [&] { ++toggles; });
                require(option.presentation().indicator == DescribedOption::Indicator::Toggle
                                && option.presentation().minimumRowHeight
                                        == int(metrics::settings::optionHeight)
                                && option.presentation().labelPixels == int(metrics::text::body)
                                && option.presentation().indicatorSize
                                        == QSize(metrics::settings::toggleWidth,
                                                 metrics::settings::toggleHeight),
                        "Option defaults changed with density");
                QImage previous;
                for (auto indicator :
                     { DescribedOption::Indicator::Toggle, DescribedOption::Indicator::Square,
                       DescribedOption::Indicator::Radio }) {
                    auto presentation = option.presentation();
                    presentation.indicator = indicator;
                    presentation.dividers = Edges(Qt::TopEdge) | Qt::BottomEdge;
                    presentation.horizontalInset = metrics::chrome::inset;
                    presentation.minimumRowHeight = metrics::chrome::optionHeight;
                    presentation.labelPixels = metrics::chrome::labelText;
                    presentation.descriptionPixels = metrics::chrome::descriptionText;
                    presentation.indicatorSize = indicator == DescribedOption::Indicator::Toggle
                            ? QSize(metrics::settings::toggleWidth, metrics::settings::toggleHeight)
                            : QSize(metrics::chrome::checkSize, metrics::chrome::checkSize);
                    const int beforeClicks = clicks, beforeToggles = toggles;
                    const bool beforeChecked = option.isChecked();
                    option.setPresentation(presentation);
                    require(clicks == beforeClicks && toggles == beforeToggles
                                    && option.isChecked() == beforeChecked,
                            "Option presentation emitted intent or changed selection");
                    option.resize(px(700), option.heightForWidth(px(700)));
                    option.show();
                    app.processEvents();
                    const auto unchecked = option.grab().toImage();
                    require(QColor::fromRgba(unchecked.pixel(0, 0)) == dividerColor(&option)
                                    && QColor::fromRgba(unchecked.pixel(0, unchecked.height() - 1))
                                            == dividerColor(&option),
                            "Option lost configured divider edges");
                    if (!previous.isNull())
                        require(previous != unchecked, "Option indicators paint the same shape");
                    previous = unchecked;
                    // Drain queued exposure paints before measuring setter work.
                    bool settled = false;
                    for (int attempt = 0; attempt < 8 && !settled; ++attempt) {
                        const int pendingPaints = option.paints;
                        app.processEvents();
                        settled = option.paints == pendingPaints;
                    }
                    require(settled, "Option exposure paints did not settle");
                    const int paints = option.paints;
                    option.setPresentation(presentation);
                    app.processEvents();
                    require(option.paints == paints, "Unchanged option presentation repainted");
                    const QPointF point(px(20), option.height() / 2);
                    QMouseEvent down(QEvent::MouseButtonPress, point, point, Qt::LeftButton,
                                     Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent up(QEvent::MouseButtonRelease, point, point, Qt::LeftButton,
                                   Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(&option, &down);
                    QApplication::sendEvent(&option, &up);
                    require(clicks == beforeClicks + 1 && toggles == beforeToggles + 1
                                    && option.isChecked(),
                            "Option indicator rejected row input or changed native signal timing");
                    require(option.grab().toImage() != unchecked,
                            "Checked option indicator did not change");
                    option.setEnabled(false);
                    QApplication::sendEvent(&option, &down);
                    QApplication::sendEvent(&option, &up);
                    require(clicks == beforeClicks + 1 && option.isChecked(),
                            "Disabled option accepted row input");
                    option.setEnabled(true);
                    option.setChecked(false);
                    app.processEvents();
                }
                DescribedOption compact("Compact", {}, &host);
                require(compact.heightForWidth(px(700)) == int(metrics::control::stepSize),
                        "Compact option default height changed");
                auto compactPresentation = compact.presentation();
                compactPresentation.indicator = DescribedOption::Indicator::Radio;
                compactPresentation.minimumRowHeight = px(150);
                compact.setPresentation(compactPresentation);
                require(compact.minimumHeight() == px(150),
                        "Option configuration lost caller minimum height");
                require(compact.heightForWidth(px(700))
                                == qMax(px(150), int(metrics::control::stepSize)),
                        "Compact option ignored the configured minimum height");
            }
        }
        setDensity(savedDensity);
    }

    kobalt::ui::WholeLineLabel excerpt;
    excerpt.setFont(kobalt::ui::interfaceFont());
    kobalt::ui::readerStyle(&excerpt, "color:black;background:white;font-size:30px;");
    excerpt.resize(400, 106);
    excerpt.setText("<b>MMMM</b><br>MMMM<br>MMMM<br>MMMM");
    excerpt.show();
    app.processEvents();
    const auto excerptImage = excerpt.grab().toImage();
    int excerptInkRows = 0, excerptLines = 0;
    for (int y = 0; y < excerptImage.height(); ++y) {
        bool ink = false;
        for (int x = 0; x < excerptImage.width(); ++x)
            ink |= qGray(excerptImage.pixel(x, y)) < 128;
        if (ink)
            ++excerptInkRows;
        else if (excerptInkRows) {
            require(excerptInkRows >= 20, "Search excerpt clips a text line");
            ++excerptLines;
            excerptInkRows = 0;
        }
    }
    require(excerptLines > 0 && excerptLines < 4 && excerptInkRows == 0,
            "Search excerpt does not end between complete lines");
    excerpt.hide();
    for (int ppi : { 167, 212, 300 }) {
        kobalt::ui::setDensity(ppi);
        QWidget host;
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        kobalt::ui::PopupCard card(&host);
        auto *title = card.title("Use this font for new books?", "popupTitle");
        auto *message = card.message("Your current book keeps its own appearance settings.",
                                     "popupMessage");
        auto *actions = card.actions();
        auto *cancel = kobalt::ui::makeButton(&card, actions, "popupCancel", "Cancel", [] { });
        auto *confirm =
                kobalt::ui::makeButton(&card, actions, "popupConfirm", "Make default", [] { });
        card.fitToWidth(kobalt::ui::metrics::notice::cardWidth);
        card.show();
        host.show();
        app.processEvents();
        const int textPixels = kobalt::ui::metrics::chrome::labelText;
        require(title->font().pixelSize() == textPixels && title->font().bold(),
                "Popup title lost its shared typography");
        require(message->font().pixelSize() == textPixels && !message->font().bold(),
                "Popup body lost its shared typography");
        require(cancel->font().pixelSize() == textPixels
                        && confirm->font().pixelSize() == textPixels,
                "Popup actions inherited a different text size");
        require(cancel->height() == confirm->height(), "Popup actions have different heights");
        require(confirm->x() - cancel->geometry().right() - 1
                        == int(kobalt::ui::metrics::notice::cardGap),
                "Popup actions lost their shared gap");
        require(card.rect().contains(title->geometry()) && card.rect().contains(message->geometry())
                        && card.rect().contains(cancel->geometry())
                        && card.rect().contains(confirm->geometry()),
                "Popup content exceeds the card at this density");
        require(message->height() >= message->heightForWidth(message->width()),
                "Popup body clips wrapped text");
        host.hide();
    }
    kobalt::ui::setDensity(kobalt::ui::designPpi);
    const auto originalPalette = app.palette();
    for (const bool dark : { false, true }) {
        QWidget host;
        host.resize(600, 800);
        host.setProperty("reader_dark", dark);
        QPalette colors = originalPalette;
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        host.setPalette(colors);
        kobalt::ui::readerStyle(&host, kobalt::ui::styles::reader());
        QLabel passage("Reading page", &host);
        passage.setGeometry(20, 20, 400, 80);
        kobalt::ui::Button outside("Underlying control", &host);
        outside.setGeometry(20, 680, 400, 80);
        QLabel colorPatch(&host);
        colorPatch.setGeometry(450, 20, 100, 80);
        colorPatch.setStyleSheet("background:#347abd;");
        // Qt5 draws a dotted focus outline on the first child button. Keep
        // focus on the host so the backdrop check compares the same state.
        host.setFocusPolicy(Qt::StrongFocus);
        host.show();
        host.setFocus();
        app.processEvents();
        const auto page = host.grab().toImage();
        kobalt::ui::ModalOverlay overlay(&host);
        overlay.setGeometry(host.rect());
        auto *card = overlay.card();
        auto *rows = card->rows();
        auto *label = new QLabel("Synced progress", card);
        rows->addWidget(label);
        auto *keep = new kobalt::ui::Button("Keep reading here", card);
        rows->addWidget(keep);
        auto *go = new kobalt::ui::Button("Go to synced position", card);
        rows->addWidget(go);
        card->setFixedSize(450, 320);
        overlay.centerCard();
        overlay.show();
        overlay.setFocus();
        app.processEvents();
        require(!overlay.isWindow() && overlay.window() == &host,
                "Progress overlay has a separate backing store/theme");
        const auto shown = host.grab().toImage();
        const auto paper = dark ? QColor(Qt::black) : QColor(Qt::white);
        const QPoint gutter = card->pos() + QPoint(4, 4);
        require(QColor::fromRgba(shown.pixel(gutter)) == paper,
                "Progress card gutter has the wrong theme");
        for (int y = 0; y < page.height(); ++y)
            for (int x = 0; x < page.width(); ++x)
                if (!card->geometry().contains(x, y))
                    require(shown.pixel(x, y) == page.pixel(x, y),
                            "Modal changed pixels outside its card");
        overlay.setDimmed(true);
        const auto patterned = host.grab().toImage();
        int stripedPixels = 0, paperPixels = 0;
        for (int y = 110; y < 180; ++y) {
            for (int x = 0; x < 60; ++x) {
                const auto value = QColor::fromRgba(patterned.pixel(x, y));
                stripedPixels += value != paper;
                paperPixels += value == paper;
            }
        }
        require(stripedPixels > 0 && paperPixels > 0,
                "Modal backdrop is missing its diagonal pattern");
        require(patterned.pixel(500, 50) != page.pixel(500, 50),
                "Modal backdrop did not fade background content");
        for (int y = card->y(); y < card->geometry().bottom(); ++y)
            for (int x = card->x(); x < card->geometry().right(); ++x)
                require(patterned.pixel(x, y) == shown.pixel(x, y),
                        "Modal pattern changed the foreground card");
        overlay.setInputBarrier(false);
        const auto nonblocking = host.grab().toImage();
        require(nonblocking.pixel(500, 50) == page.pixel(500, 50),
                "A nonblocking modal painted a backdrop");
        overlay.setInputBarrier(true);
        overlay.setDimmed(false);
        for (int n = 0; n < 6; ++n) {
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
            QApplication::sendEvent(host.focusWidget(), &tab);
            require(overlay.isAncestorOf(host.focusWidget()),
                    "Modal keyboard focus escaped to underlying controls");
        }
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &escape);
        require(overlay.isVisible(), "Escape silently dismissed a progress choice");
        host.resize(800, 600);
        app.processEvents();
        require(card->geometry().center() == overlay.rect().center(),
                "Modal card did not follow reader geometry");
        host.resize(600, 800);
        overlay.hide();
        host.setFocus();
        app.processEvents();
        require(host.grab().toImage() == page,
                "Dismissed backdrop did not restore the page exactly");
        kobalt::ui::PopupWindow opening(&host, dark);
        opening.setGeometry(host.geometry());
        auto *openingCard = opening.card();
        openingCard->message("Loading book…");
        auto *bar = new kobalt::ui::ProgressBar(openingCard);
        bar->setValue(50);
        openingCard->rows()->addWidget(bar);
        openingCard->fitToWidth(450);
        opening.show();
        app.processEvents();
        auto image = opening.grab().toImage();
        require(openingCard->geometry().center() == opening.rect().center(),
                "Opening card is not centered");
        require(QColor::fromRgba(image.pixel(openingCard->pos() + QPoint(4, 4))) == paper,
                "Opening popup did not use the shared theme");
        for (int y = 0; y < page.height(); ++y)
            for (int x = 0; x < page.width(); ++x)
                if (!openingCard->geometry().contains(x, y))
                    require(image.pixel(x, y) == page.pixel(x, y),
                            "Opening popup changed its captured background");
        opening.hide();
        app.processEvents();
        require(host.grab().toImage() == page, "Closing the opening popup damaged its parent");
        host.hide();
    }
    {
        using namespace kobalt::ui;
        const auto alias = QStringLiteral("Dictionary private-only canary");
        require(registerPrivateInterfaceFont(
                        kobalt::test::readerAssets().file("fonts/NotoSansMath-Regular.ttf"), alias),
                "Cannot retain the dictionary canary font");
        QFont font(alias);
        font.setPixelSize(36);
        QFile editorFace(kobalt::test::readerAssets().file("fonts/NotoSansMath-Regular.ttf"));
        require(editorFace.open(QIODevice::ReadOnly)
                        && nativeEditorFontData(font) == editorFace.readAll(),
                "A private TrueType face must remain available for a native editor");
        require(nativeEditorFontData(QFont("Unregistered editor family")).isEmpty(),
                "An unknown editor family must not silently load an unrelated font");
        require(registerPrivateInterfaceFont(
                        kobalt::test::readerAssets().file("fonts/NotoSansMath-Regular.ttf"), alias),
                "Preparing a native editor must not change private preview registration");
        const auto bytes = nativeEditorFontData(font);
        const auto nativeFamily = QRawFont(bytes, 36, QFont::PreferNoHinting).familyName();
        require(!nativeFamily.isEmpty(), "The editor fixture has no native font family");
        QFont editorFont(nativeFamily);
        editorFont.setPixelSize(36);
        require(QRawFont::fromFont(editorFont).familyName() != nativeFamily,
                "The private editor fixture was already registered with Qt");
        require(kobalt::reader::registerEditorFont(editorFont)
                        && QRawFont::fromFont(renewedFont(editorFont)).familyName() == nativeFamily,
                "A native editor must resolve the private reading face instead of a fallback");
        require(!kobalt::reader::registerEditorFont(editorFont),
                "Reopening the editor must not register its face again");
        PagedText preview;
        preview.resize(500, 150);
        preview.setMargins({});
        preview.setTextFont(font);
        preview.setHtml("iiiiWWWW");
        preview.show();
        const auto actual = preview.grab().toImage().convertToFormat(QImage::Format_RGB32);
        QImage expected(actual.size(), QImage::Format_RGB32);
        expected.fill(preview.palette().color(QPalette::Window));
        const auto glyphs = renderPrivateText(
                "iiiiWWWW", font, -1, {}, {}, PrivateTextEmphasis::FontFace, Qt::LeftToRight,
                { { 0, 8, font, preview.palette().color(QPalette::WindowText) } });
        QPainter painter(&expected);
        painter.drawImage(QPoint(0, glyphs.lineAscent) + glyphs.origin, glyphs.image);
        painter.end();
        const auto ink = [](const QImage &image) {
            QRect bounds;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qGray(image.pixel(x, y)) < 240)
                        bounds |= QRect(x, y, 1, 1);
            return bounds;
        };
        if (actual.copy(ink(actual)) != expected.copy(ink(expected))) {
            std::fprintf(stderr, "Canary actual ink %dx%d expected %dx%d font %s\n",
                         ink(actual).width(), ink(actual).height(), ink(expected).width(),
                         ink(expected).height(), qPrintable(preview.textFont().toString()));
        }
        require(!ink(expected).isEmpty()
                        && actual.copy(ink(actual)) == expected.copy(ink(expected)),
                "Dictionary definitions did not use the retained FreeType font glyphs");
    }
    kobalt::ui::PagedText definition;
    definition.resize(400, 95);
    definition.show();
    app.processEvents();
    QString paragraphs;
    for (int n = 0; n < 17; ++n)
        paragraphs += "<p style='margin:0;font-size:24px'>MMMM</p>";
    definition.setHtml(paragraphs);
    int completeLines = 0;
    for (int page = 0; page < definition.pages(); ++page) {
        definition.setPage(page);
        const QImage pixels = definition.grab().toImage();
        int inkRows = 0;
        for (int y = 0; y < pixels.height(); ++y) {
            bool ink = false;
            for (int x = 0; x < pixels.width(); ++x)
                ink |= qGray(pixels.pixel(x, y)) < 128;
            if (ink)
                ++inkRows;
            else if (inkRows) {
                require(inkRows >= 15, "Definition page clipped part of a text line");
                ++completeLines;
                inkRows = 0;
            }
        }
        require(inkRows == 0, "Definition text reaches the bottom edge");
    }
    require(completeLines == 17, "Definition paging omitted or repeated a line");
    definition.resize(980, 426);
    QString table = "<table><tr><td>";
    for (int n = 0; n < 40; ++n)
        table += "<p style='margin:0;font-size:36px'>MMMM MMMM</p>";
    table += "</td><td>";
    for (int n = 0; n < 40; ++n)
        table += "<p style='margin:0;font-size:37px;color:#ff0000'>MMMM MMMM</p>";
    table += "</td></tr></table>";
    definition.setHtml(table);
    require(definition.pages() >= 5, "Multi-column definition was compressed into too few pages");
    for (bool secondColumn : { false, true }) {
        int tableLines = 0;
        for (int page = 0; page < definition.pages(); ++page) {
            definition.setPage(page);
            const auto pixels = definition.grab().toImage();
            int inkRows = 0;
            for (int y = 0; y < pixels.height(); ++y) {
                bool ink = false;
                // Separate the columns by ink colour so either can wrap freely.
                for (int x = 0; x < pixels.width(); ++x) {
                    const auto pixel = pixels.pixel(x, y);
                    ink |= secondColumn
                            ? qRed(pixel) > 160 && qGreen(pixel) < 80 && qBlue(pixel) < 80
                            : qRed(pixel) < 128 && qGreen(pixel) < 128 && qBlue(pixel) < 128;
                }
                if (ink)
                    ++inkRows;
                else if (inkRows) {
                    require(inkRows >= 23, "Definition page shrank or clipped its text");
                    ++tableLines;
                    inkRows = 0;
                }
            }
            require(inkRows == 0, "Definition column reaches the bottom edge");
        }
        require(tableLines == 40, "Table pagination omitted or repeated a line");
    }
    {
        QString uneven = "<table><tr><td>";
        for (int line = 0; line < 14; ++line)
            uneven += "<p style='margin:0;font-size:36px;color:#ff0000'>MMMM</p>";
        uneven += "</td><td><p style='margin:0;font-size:36px'>MMMM</p></td></tr>"
                  "<tr><td><p style='margin:0;font-size:36px;color:#00aa00'>MMMM</p></td>"
                  "<td><p style='margin:0;font-size:36px;color:#0000ff'>MMMM</p></td></tr></table>";
        definition.setHtml(uneven);
        int counts[4] = {}, firstRows[4] = { -1, -1, -1, -1 };
        for (int page = 0; page < definition.pages(); ++page) {
            definition.setPage(page);
            const auto pixels = definition.grab().toImage();
            int inkRows[4] = {};
            for (int y = 0; y < pixels.height(); ++y) {
                bool ink[4] = {};
                for (int x = 0; x < pixels.width(); ++x) {
                    const auto pixel = pixels.pixel(x, y);
                    ink[0] |= qRed(pixel) > 160 && qGreen(pixel) < 80 && qBlue(pixel) < 80;
                    ink[1] |= qRed(pixel) < 80 && qGreen(pixel) < 80 && qBlue(pixel) < 80;
                    ink[2] |= qRed(pixel) < 80 && qGreen(pixel) > 120 && qBlue(pixel) < 80;
                    ink[3] |= qRed(pixel) < 80 && qGreen(pixel) < 80 && qBlue(pixel) > 160;
                }
                for (int color = 0; color < 4; ++color) {
                    if (ink[color]) {
                        if (firstRows[color] < 0)
                            firstRows[color] = page * pixels.height() + y;
                        ++inkRows[color];
                    } else if (inkRows[color]) {
                        require(inkRows[color] >= 23, "Uneven table clipped a line");
                        ++counts[color];
                        inkRows[color] = 0;
                    }
                }
            }
            for (int color = 0; color < 4; ++color)
                require(inkRows[color] == 0, "Uneven table reaches the page edge");
        }
        require(counts[0] == 14 && counts[1] == 1 && counts[2] == 1 && counts[3] == 1,
                "Uneven table lost or repeated a line");
        require(firstRows[2] == firstRows[3],
                "A table cell acquired a page break from its neighbor");
        definition.setHtml(table);
    }
    const int portraitPages = definition.pages();
    definition.resize(1100, 600);
    app.processEvents();
    definition.resize(980, 426);
    app.processEvents();
    require(definition.pages() == portraitPages,
            "Resizing retained obsolete definition page breaks");
    {
        const QString list = "<ol style='margin:0;font-size:36px'>"
                             "<li style='margin:0'>MMMM</li><li style='margin:0'>MMMM</li>"
                             "<li style='margin:0 0 12px 0'>MMMM</li><li style='margin:0'>"
                + QString("MMMM ").repeated(32) + "</li><li style='margin:0'>MMMM</li></ol>";
        kobalt::ui::PagedDocument continuous;
        continuous.setDefaultFont(kobalt::ui::interfaceFont(36));
        continuous.setDocumentMargin(0);
        auto option = continuous.defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        continuous.setDefaultTextOption(option);
        continuous.setHtml(list);
        const int margin = kobalt::ui::metrics::acknowledgments::textMargin;
        const int width = 400;
        continuous.setTextWidth(width - 2 * margin);
        continuous.size();
        QVector<QTextBlock> items;
        for (auto block = continuous.begin(); block.isValid(); block = block.next())
            if (block.text().startsWith("MMMM"))
                items.append(block);
        require(items.size() == 5 && items[3].layout()->lineCount() > 1,
                "List fixture did not wrap its fourth item");
        const auto first = items[2].layout()->lineAt(0);
        const qreal firstBottom = items[2].layout()->position().y() + first.y() + first.height();
        const qreal secondTop =
                items[3].layout()->position().y() + items[3].layout()->lineAt(0).y();
        // Put the page boundary inside the margin, so neither adjacent line
        // crosses it. A later wrapped line must not jump back to that boundary.
        const int pageHeight = qRound((firstBottom + secondTop) / 2);
        require(firstBottom < pageHeight && pageHeight < secondTop,
                "List fixture lacks a margin-only page crossing");
        auto lines = [](const QImage &pixels) {
            int count = 0, inkRows = 0;
            for (int y = 0; y < pixels.height(); ++y) {
                bool ink = false;
                for (int x = 0; x < pixels.width(); ++x)
                    ink |= qGray(pixels.pixel(x, y)) < 128;
                if (ink)
                    ++inkRows;
                else if (inkRows) {
                    require(inkRows >= 23 && inkRows <= 32,
                            "Wrapped list line was clipped or overlapped");
                    ++count;
                    inkRows = 0;
                }
            }
            require(inkRows == 0, "Wrapped list reaches the page edge");
            return count;
        };
        QImage reference(width, qCeil(continuous.size().height()) + 2 * margin,
                         QImage::Format_RGB32);
        reference.fill(Qt::white);
        {
            QPainter painter(&reference);
            painter.translate(margin, margin);
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette = definition.palette();
            continuous.documentLayout()->draw(&painter, context);
        }
        const int expected = lines(reference);
        auto checkList = [&] {
            int count = 0;
            for (int page = 0; page < definition.pages(); ++page) {
                definition.setPage(page);
                count += lines(definition.grab().toImage());
            }
            require(count == expected,
                    "Margin-only page crossing lost or repeated wrapped list lines");
        };
        definition.resize(width, pageHeight + 2 * margin);
        app.processEvents();
        definition.setHtml(list);
        checkList();
        definition.resize(width, pageHeight + 2 * margin + 17);
        app.processEvents();
        checkList();
        definition.resize(width, pageHeight + 2 * margin);
        app.processEvents();
        checkList();
    }
    definition.hide();
    for (const bool dark : { false, true }) {
        for (const auto alignment : { Qt::AlignLeft, Qt::AlignHCenter }) {
            kobalt::ui::Button button("Get");
            button.resize(220, 66);
            button.setProperty("reader_dark", dark);
            button.setPalette(kobalt::ui::readerPalette(button.palette(), dark));
            button.setContentAlignment(alignment);
            kobalt::ui::readerStyle(&button,
                                    kobalt::ui::styles::outlined()
                                            + "QPushButton{font-size:34px;padding:0;}");
            QPixmap glyph(28, 28);
            glyph.fill(Qt::magenta);
            button.setIcon(QIcon(glyph));
            button.setIconSize(glyph.size());
            button.show();
            app.processEvents();
            const auto image = button.grab().toImage();
            QRect iconInk, textInk;
            for (int y = 4; y < image.height() - 4; ++y)
                for (int x = 4; x < image.width() - 4; ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    if (pixel == QColor(Qt::magenta))
                        iconInk |= QRect(x, y, 1, 1);
                    else if (dark ? qGray(pixel.rgb()) > 192 : qGray(pixel.rgb()) < 64)
                        textInk |= QRect(x, y, 1, 1);
                }
            if (qEnvironmentVariableIsSet("KOBALT_DICTIONARY_CONTROLS_CAPTURE")) {
                const auto path = qEnvironmentVariable("KOBALT_DICTIONARY_CONTROLS_CAPTURE");
                QDir().mkpath(path);
                image.save(path + QString("/button-%1-%2.png").arg(dark).arg(int(alignment)));
            }
            require(!iconInk.isEmpty() && !textInk.isEmpty()
                            && textInk.left() - iconInk.right() >= 8,
                    "Shared button icon and text overlap or lack a horizontal gap");
            require(qAbs(iconInk.center().y() - textInk.center().y()) <= 2
                            && qAbs(textInk.center().y() - button.rect().center().y()) <= 2,
                    "Shared button text and icon are not vertically centered");
            require(alignment == Qt::AlignLeft
                            ? qAbs(iconInk.left() - kobalt::ui::px(24)) <= 1
                            : qAbs((iconInk | textInk).center().x() - button.rect().center().x())
                                    <= 3,
                    "Shared button does not honor its horizontal content alignment");
        }
    }
    {
        QWidget owner;
        owner.resize(1100, 650);
        owner.show();
        kobalt::reader::DictionariesPage page(&owner);
        page.resize(owner.size());
        page.show();
        QVector<kobalt::DownloadDictionary> entries;
        for (const auto &id :
             { "en-fr", "ar-en", "en-en", "ar-ar", "de-de", "de-en", "el-el", "cs-cs" }) {
            kobalt::DownloadDictionary entry;
            entry.id = id;
            entry.language = entry.id.section('-', 0, 0);
            entry.target = entry.id.section('-', 1, 1);
            entry.name = entry.id;
            entry.size = 3500000;
            entries.append(entry);
        }
        page.setLanguage("en-US");
        page.setCatalog(entries);
        page.setState({ "en-en", "ar-ar" }, false, {});
        page.setStorage({ { "en-en", 92000000 }, { "ar-ar", 3500000 } }, 1000000000);
        app.processEvents();
        auto *first = page.findChild<QWidget *>("rrDictionaryRow0");
        require(first && first->property("dictionary_id").toString() == "en-en",
                "Book-language monolingual dictionary is not first");
        QSet<QString> seen;
        int actionWidth = -1, pageCount = 0;
        do {
            app.processEvents();
            ++pageCount;
            QVector<QWidget *> blocks;
            for (auto *child : page.findChildren<QWidget *>()) {
                if (!child->isVisible())
                    continue;
                if (child->objectName().startsWith("rrDictionaryRow")
                    || child->objectName().startsWith("rrDictionarySection")
                    || child->objectName().startsWith("rrDictionaryLanguage"))
                    blocks.append(child);
                if (child->objectName().startsWith("rrDictionaryAction")) {
                    if (actionWidth < 0)
                        actionWidth = child->width();
                    require(child->width() == actionWidth,
                            "Dictionary actions have different widths");
                }
            }
            std::sort(blocks.begin(), blocks.end(),
                      [](QWidget *left, QWidget *right) { return left->y() < right->y(); });
            require(!blocks.isEmpty()
                            && blocks.first()->objectName().startsWith("rrDictionarySection"),
                    "Continued dictionary page lacks a section caption");
            require(blocks.last()->objectName().startsWith("rrDictionaryRow"),
                    "Dictionary page ends with an orphan caption");
            for (auto *block : blocks)
                if (block->objectName().startsWith("rrDictionaryRow")) {
                    const auto id = block->property("dictionary_id").toString();
                    require(!seen.contains(id),
                            "Dictionary repeated between installed and available sections");
                    seen.insert(id);
                }
            auto *next = page.findChild<QPushButton *>("rrDictionariesNext");
            const auto fraction =
                    page.findChild<QLabel *>("rrDictionariesPage")->text().split(" of ");
            if (fraction.first() == fraction.last())
                break;
            next->click();
        } while (pageCount < 20);
        require(seen.size() == entries.size() && pageCount > 1, "Dictionary paging lost entries");
        auto *catalogCounter = page.findChild<QLabel *>("rrDictionariesPage");
        const auto finalCatalogPage = catalogCounter->text();
        page.findChild<QPushButton *>("rrDictionariesNext")->click();
        require(catalogCounter->text().startsWith("1 of "),
                "Dictionary list did not wrap forwards");
        page.findChild<QPushButton *>("rrDictionariesPrevious")->click();
        require(catalogCounter->text() == finalCatalogPage,
                "Dictionary list did not wrap backwards");
        page.setLanguage("zz");
        page.setState({}, false, {});
        app.processEvents();
        for (auto *label : page.findChildren<QLabel *>())
            if (label->isVisible())
                require(!label->text().contains("other languages")
                                && !label->text().contains("for this book"),
                        "Unknown language retains book-specific captions");
        auto resetPage = [&] {
            auto *previous = page.findChild<QPushButton *>("rrDictionariesPrevious");
            for (int turn = 0; turn < 20
                 && !page.findChild<QLabel *>("rrDictionariesPage")->text().startsWith("1 of ");
                 ++turn)
                previous->click();
            app.processEvents();
        };
        QVector<kobalt::DownloadDictionary> english;
        for (const auto &id : { "en-fr", "en-en", "en-de", "en-ar" }) {
            kobalt::DownloadDictionary entry;
            entry.id = id;
            entry.language = "en";
            entry.target = entry.id.section('-', 1, 1);
            entry.name = entry.id == "en-en" ? "English monolingual" : "English to " + entry.target;
            entry.license = "CC-BY-SA-4.0";
            entry.size = 3500000;
            english.append(entry);
        }
        page.resize(1100, 470);
        page.setLanguage("en");
        page.setCatalog(english);
        page.setState({}, false, {});
        resetPage();
        QStringList ordered;
        do {
            for (int i = 0;
                 auto *row = page.findChild<QWidget *>(QString("rrDictionaryRow%1").arg(i)); ++i)
                ordered.append(row->property("dictionary_id").toString());
            auto *next = page.findChild<QPushButton *>("rrDictionariesNext");
            const auto fraction =
                    page.findChild<QLabel *>("rrDictionariesPage")->text().split(" of ");
            if (fraction.first() == fraction.last())
                break;
            next->click();
        } while (ordered.size() < 10);
        require(ordered == QStringList({ "en-en", "en-ar", "en-fr", "en-de" }),
                "Available dictionaries are not monolingual-first then target-language ordered");
        page.setLanguage("zz");
        resetPage();
        page.findChild<QPushButton *>("rrDictionariesNext")->click();
        app.processEvents();
        auto *continuedLanguage = dynamic_cast<kobalt::ui::ListHeader *>(
                page.findChild<QWidget *>("rrDictionaryLanguage0"));
        require(continuedLanguage && continuedLanguage->leftLabel()->text() == "English",
                "Language subcaption is not repeated on a continued page");
        page.setLanguage("en");
        resetPage();
        if (qEnvironmentVariableIsSet("KOBALT_DICTIONARY_CONTROLS_CAPTURE"))
            page.grab().save(qEnvironmentVariable("KOBALT_DICTIONARY_CONTROLS_CAPTURE")
                             + "/downloads.png");
        int starts = 0, stops = 0;
        page.installRequested = [&](const QString &id) {
            ++starts;
            page.setTransfer(id, 2000000, 3500000, false);
            page.setState({}, true, {});
        };
        page.cancelRequested = [&] {
            ++stops;
            page.setTransfer({}, 0, 0, false);
            page.setState({}, false, {});
        };
        QPointer<QPushButton> downloadAction = page.findChild<QPushButton *>("rrDictionaryAction0");
        downloadAction->click();
        auto *confirmation = dynamic_cast<kobalt::ui::ChoiceModal *>(
                owner.findChild<QWidget *>("rrDictionaryDownload"));
        app.processEvents();
        require(confirmation && confirmation->isVisible() && starts == 0,
                "Get must ask for confirmation before starting a download");
        require(confirmation->rect().contains(confirmation->card()->geometry()),
                "Download confirmation must fit a short settings viewport");
        require(confirmation->descriptionLabel()->text().contains("3.5 MB")
                        && confirmation->descriptionLabel()->text().contains("CC BY-SA 4.0"),
                "Download confirmation must show its actual size and license");
        if (qEnvironmentVariableIsSet("KOBALT_DICTIONARY_CONTROLS_CAPTURE"))
            owner.grab().save(qEnvironmentVariable("KOBALT_DICTIONARY_CONTROLS_CAPTURE")
                              + "/download-confirmation.png");
        confirmation->secondaryButton()->click();
        require(starts == 0 && !confirmation->isVisible(),
                "Cancel must dismiss confirmation without downloading");
        app.processEvents();
        downloadAction->click();
        owner.findChild<QPushButton *>("rrDictionaryDownloadConfirm")->click();
        require(starts == 1 && downloadAction
                        && downloadAction->text() == kobalt::strings::get("dictionaries.stop"),
                "Download action did not become Stop in place");
        downloadAction->click();
        require(stops == 1 && downloadAction && !downloadAction->icon().isNull(),
                "Cancelling did not restore the download icon and action in place");
        downloadAction->click();
        app.processEvents();
        owner.findChild<QPushButton *>("rrDictionaryDownloadConfirm")->click();
        require(starts == 2, "Reused dictionary action retained an outdated click handler");
        page.setState({}, true, {});
        page.setTransfer("en-en", 2000000, 3500000, false);
        app.processEvents();
        auto *activeRow = page.findChild<QWidget *>("rrDictionaryRow0");
        auto *otherRow = page.findChild<QWidget *>("rrDictionaryRow1");
        require(otherRow, "Progress test needs a second dictionary row");
        PaintCountingObserver unrelatedPaints;
        otherRow->installEventFilter(&unrelatedPaints);
        for (auto *child : otherRow->findChildren<QWidget *>())
            child->installEventFilter(&unrelatedPaints);
        for (int received : { 2100000, 2200000, 2200000 }) {
            page.setStorage({}, 1000000000);
            page.setTransfer("en-en", received, 3500000, false);
            page.setState({}, true, {});
            app.processEvents();
            require(page.findChild<QWidget *>("rrDictionaryRow0") == activeRow
                            && page.findChild<QWidget *>("rrDictionaryRow1") == otherRow,
                    "Download progress rebuilt dictionary rows");
        }
        require(unrelatedPaints.paints == 0,
                "Download progress repainted an unchanged dictionary row");
        auto *stopAction = page.findChild<QPushButton *>("rrDictionaryAction0");
        require(stopAction && !stopAction->icon().isNull()
                        && stopAction->iconSize().width() == kobalt::ui::px(28),
                "Transfer progress must use the Stop button's icon slot");
        require(page.findChild<QWidget *>("rrDictionaryRow0")->height()
                        == kobalt::ui::metrics::list::oneLine,
                "Downloading changed the dictionary row height");
        page.setTransfer({}, 0, 0, false);
        page.setState({}, false, "Download failed");
        app.processEvents();
        bool error = false;
        for (auto *label : page.findChildren<QLabel *>())
            error |= label->isVisible()
                    && label->text() == kobalt::strings::get("dictionaries.download_failed");
        require(error, "Failed transfer lacks its row error");
        page.installRequested = [](const QString &) { };
        page.findChild<QPushButton *>("rrDictionaryAction0")->click();
        app.processEvents();
        for (auto *label : page.findChildren<QLabel *>())
            require(!label->isVisible()
                            || label->text()
                                    != kobalt::strings::get("dictionaries.download_failed"),
                    "Next page action retained transfer failure");
        owner.findChild<QPushButton *>("rrDictionaryDownloadCancel")->click();
        app.processEvents();
        page.setState({ "en-en" }, false, {});
        resetPage();
        auto *installedHeader = dynamic_cast<kobalt::ui::ListHeader *>(
                page.findChild<QWidget *>("rrDictionarySection0"));
        require(installedHeader
                        && installedHeader->leftLabel()->text().startsWith(
                                "Installed for this book"),
                "Completed install did not move into the installed section");
        page.findChild<QPushButton *>("rrDictionariesPrevious")->click();
        page.setCatalog({ english[1] });
        page.setState({ "en-en" }, false, {});
        app.processEvents();
        require(!page.findChild<QPushButton *>("rrDictionariesPrevious")->isEnabled()
                        && !page.findChild<QPushButton *>("rrDictionariesNext")->isEnabled(),
                "Dictionary page did not clamp after the list shrank");
    }
    {
        const QString entry =
                "<a name='sample' /><b>sample</b> - noun</p><var><variant name='samples' /></var><p><ol><li>First meaning.</li><li>Second meaning.</li></ol></p></w>";
        QTextDocument document;
        document.setHtml(kobalt::DictionaryPanel::formatDefinition(entry.repeated(3)));
        int headings = 0;
        for (auto block = document.begin(); block.isValid(); block = block.next())
            if (block.text().startsWith("sample - noun")) {
                ++headings;
                require(!block.textList(),
                        "Later dictionary entry heading inherited a definition list");
            }
        require(headings == 3, "Dictionary formatting lost an entry heading");
        const QString ordinary = "<p>A regular definition.</p><ol><li>A meaning.</li></ol>";
        require(kobalt::DictionaryPanel::formatDefinition(ordinary) == ordinary,
                "Kobo normalization changed ordinary dictionary HTML");
    }
    {
        using kobalt::DictionaryPanel;
        require(DictionaryPanel::definitionLineHeight(100) == 0
                        && DictionaryPanel::definitionLineHeight(120) == 110
                        && DictionaryPanel::definitionLineHeight(140) == 120,
                "Default dictionary font changed its automatic leading");
        require(DictionaryPanel::definitionLineHeight(80, true) == 120
                        && DictionaryPanel::definitionLineHeight(100, true) == 120
                        && DictionaryPanel::definitionLineHeight(120, true) == 132
                        && DictionaryPanel::definitionLineHeight(140, true) == 144,
                "Custom dictionary font did not add twenty percent to its automatic leading");
    }
    {
        QWidget host;
        host.resize(800, 900);
        kobalt::ui::PagerBar outer(&host, "outer");
        QWidget nested(&host);
        nested.resize(600, 600);
        kobalt::ui::PagerBar inner(&nested, "inner");
        outer.setPage(0, 3, {}, true);
        inner.setPage(1, 2);
        int outerTurns = 0, innerTurns = 0;
        QObject::connect(outer.next, &QPushButton::clicked, [&] { ++outerTurns; });
        QObject::connect(inner.previous, &QPushButton::clicked, [&] { ++innerTurns; });
        host.show();
        app.processEvents();
        require(kobalt::ui::turnVisiblePageControl(&host, 1) && outerTurns == 0,
                "End of nested text paged the list behind it");
        require(kobalt::ui::turnVisiblePageControl(&host, -1) && innerTurns == 1,
                "Hardware paging missed the nested pager");
        nested.hide();
        require(kobalt::ui::turnVisiblePageControl(&host, 1) && outerTurns == 1,
                "Hidden nested pager intercepted a hardware turn");
    }
    {
        kobalt::library::Index index;
        for (int i = 0; i < 30; ++i)
            index.folders.append(
                    { QString("/books/folder-%1").arg(i), QString("Folder %1").arg(i) });
        for (int i = 0; i < 30; ++i) {
            kobalt::library::Book book;
            book.key = QString::number(i);
            book.title = QString("Book %1").arg(i);
            book.opened = 1;
            book.percent = 20;
            index.books.insert(book.key, book);
        }
        kobalt::library::LibraryView library(index, [](const QString &) { return QString(); });
        library.resize(1072, 1448);
        library.show();
        for (auto view : { kobalt::library::View::Home, kobalt::library::View::List,
                           kobalt::library::View::Grid }) {
            index.settings.view = view;
            library.refresh();
            app.processEvents();
            auto *counter = library.findChild<QLabel *>("libraryPage");
            require(counter && counter->isVisible(), "Multi-page Library view has no pager");
            const auto first = counter->text();
            library.pageButton(1);
            require(counter->text() != first, "Hardware buttons did not advance a Library view");
            library.pageButton(-1);
            require(counter->text() == first, "Hardware buttons did not return a Library view");
        }
        for (const QSize size : { QSize(1072, 1448), QSize(1448, 700) }) {
            library.resize(size);
            library.show();
            library.refresh();
            library.findChild<QPushButton *>("libraryMore")->click();
            library.findChild<QPushButton *>("libraryMoreFolders")->click();
            app.processEvents();
            auto *counter = library.findChild<QLabel *>("libraryFoldersPage");
            auto *folderPage = library.findChild<QWidget *>("libraryFolders");
            require(counter && counter->isVisible(), "Long folder list has no pager");
            const auto first = counter->text();
            QSet<QString> reached;
            int turns = 0;
            do {
                for (auto *widget : folderPage->findChildren<QWidget *>()) {
                    const auto name = widget->objectName();
                    if (!name.startsWith("libraryFolder") && name != "libraryCheckAtStart")
                        continue;
                    const auto center = widget->mapTo(&library, widget->rect().center());
                    auto *hit = library.childAt(center);
                    if (hit == widget || (hit && widget->isAncestorOf(hit)))
                        reached.insert(name);
                }
                library.pageButton(1);
                require(++turns < 100, "Folder paging did not wrap");
            } while (counter->text() != first);
            require(turns > 1, "Long folder list stayed on one page");
            for (int i = 0; i < index.folders.size(); ++i)
                require(reached.contains(QString("libraryFolder%1").arg(i)),
                        "Folder paging lost a row");
            require(reached.contains("libraryCheckAtStart") && reached.contains("libraryFoldersAdd")
                            && reached.contains("libraryFoldersRescan"),
                    "Folder paging lost the controls after the list");
            library.pageButton(-1);
            require(counter->text() != first, "Folder paging did not wrap backwards");
            library.pageButton(1);
            require(counter->text() == first, "Folder paging did not return to its first page");
            library.dismiss();
        }
    }
    QWidget reader;
    reader.resize(1072, 1448);
    reader.show();
    {
        kobalt::reader::SettingsPanel settings(&reader, {});
        settings.setGeometry(reader.rect());
        settings.show();
        app.processEvents();
        const auto sampleHeight = [&settings](const char *name) {
            auto *option = settings.fontDialog->findChild<QWidget *>(name);
            require(option && option->isVisible(), "Dictionary size preview is missing");
            const auto image = option->grab().toImage().convertToFormat(QImage::Format_RGB32);
            int first = image.height(), last = -1;
            for (int y = 4; y < image.height() - 4; ++y)
                for (int x = image.width() / 2; x < image.width() - 4; ++x)
                    if (qGray(image.pixel(x, y)) < 80) {
                        first = qMin(first, y);
                        last = qMax(last, y);
                    }
            return last >= first ? last - first + 1 : 0;
        };
        for (const auto &family :
             { kobalt::ui::interfaceFont().family(), kobalt::ui::brandFont(30).family() }) {
            settings.showDictionaryFontSizes(100, family, true, [](int) { });
            app.processEvents();
            const int small = sampleHeight("rrDictionaryFontSize80");
            require(small > 0 && sampleHeight("rrDictionaryFontSize140") > small,
                    "Dictionary size previews do not draw the selected text sizes");
            auto *option = settings.fontDialog->findChild<QWidget *>("rrDictionaryFontSize140");
            auto *preview = option->findChild<QWidget *>("rrDictionaryFontPreview140");
            require(preview && option->rect().contains(preview->geometry()),
                    "Extra large dictionary preview escapes its row");
            for (int percent : { 80, 100, 120, 140 }) {
                auto *choice = settings.fontDialog->findChild<QAbstractButton *>(
                        QString("rrDictionaryFontSize%1").arg(percent));
                require(choice
                                && choice->text()
                                        == kobalt::reader::SettingsPanel::dictionaryFontSizeLabel(
                                                percent),
                        "Standard interface text lost the dictionary size label");
                require(choice->accessibleName()
                                == kobalt::reader::SettingsPanel::dictionaryFontSizeLabel(percent),
                        "Dictionary size choice lost its accessible name");
                auto *sample = settings.fontDialog->findChild<QWidget *>(
                        QString("rrDictionaryFontPreview%1").arg(percent));
                auto *label = sample->findChild<QLabel *>();
                require(label, "Dictionary preview has no sample label");
                const int textWidth =
                        qCeil(QFontMetricsF(label->font()).horizontalAdvance(label->text()));
                require(label->width() >= textWidth, "Dictionary preview elides its sample text");
                const auto pixels = sample->grab().toImage().convertToFormat(QImage::Format_RGB32);
                QRect ink;
                for (int y = 0; y < pixels.height(); ++y)
                    for (int x = 0; x < pixels.width(); ++x)
                        if (qGray(pixels.pixel(x, y)) < 80)
                            ink |= QRect(x, y, 1, 1);
                require(!ink.isEmpty(), "Dictionary preview has no painted text");
                const auto bounds =
                        QRect(sample->mapTo(sample->parentWidget(), ink.topLeft()), ink.size());
                require(qAbs(bounds.top()
                             - (sample->parentWidget()->height() - bounds.bottom() - 1))
                                <= 2,
                        "Dictionary preview ink is not centered vertically in its row");
                require(ink.left() >= kobalt::ui::px(8) && ink.top() >= kobalt::ui::px(8)
                                && pixels.width() - ink.right() - 1 >= kobalt::ui::px(8)
                                && pixels.height() - ink.bottom() - 1 >= kobalt::ui::px(8),
                        "Dictionary preview ink has insufficient padding");
            }
            settings.dismissFontChoices();
        }
        const auto savedScale = kobalt::ui::textScale();
        const auto savedSize = reader.size();
        for (const auto &size : { QSize(1072, 1448), QSize(720, 1600), QSize(1600, 720) }) {
            reader.resize(size);
            const bool phone = size != QSize(1072, 1448);
            for (qreal scale : { 1.0, 1.25, 1.5 }) {
                kobalt::ui::setLargeText(scale);
                settings.showDictionaryFontSizes(100, {}, false, [](int) { });
                app.processEvents();
                const QStringList shortNames{ "S", "M", "L", "XL" };
                int index = 0;
                for (int percent : { 80, 100, 120, 140 }) {
                    auto *choice = settings.fontDialog->findChild<QAbstractButton *>(
                            QString("rrDictionaryFontSize%1").arg(percent));
                    const auto name =
                            kobalt::reader::SettingsPanel::dictionaryFontSizeLabel(percent);
                    const auto shown = scale == 1.5 ? QString() : phone ? shortNames[index] : name;
                    require(choice && choice->text() == shown,
                            "Dictionary size label does not match the screen and interface text size");
                    require(choice->accessibleName() == name,
                            "Dictionary size choice lost its full accessible name");
                    ++index;
                }
                settings.dismissFontChoices();
            }
        }
        reader.resize(savedSize);
        kobalt::ui::setLargeText(savedScale);
        int paragraphCommits = 0, chosenGap = -2, chosenIndent = -2;
        settings.showParagraphAdjustments(-1, 60, [&](int gap, int indent) {
            ++paragraphCommits;
            chosenGap = gap;
            chosenIndent = indent;
        });
        app.processEvents();
        auto *indent = static_cast<kobalt::ui::SettingSlider *>(
                settings.fontDialog->findChild<QSlider *>("rrParagraphIndentSlider"));
        auto *gap = static_cast<kobalt::ui::SettingSlider *>(
                settings.fontDialog->findChild<QSlider *>("rrParagraphSpacingSlider"));
        require(indent && gap, "Paragraph adjustment sliders are missing");
        const int inset = kobalt::ui::metrics::control::sliderInset;
        const auto xAt = [inset](kobalt::ui::SettingSlider *control, double position) {
            return inset + qRound((control->width() - 2 * inset) * position);
        };
        const auto mouse = [](kobalt::ui::SettingSlider *control, QEvent::Type type, int x) {
            const QPointF local(x, control->height() / 2),
                    global = control->mapToGlobal(local.toPoint());
            QMouseEvent event(type, local, global,
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(control, &event);
        };
        const QVector<int> indentStops{ -1, 0, 25, 50, 75, 100, 125, 150, 175, 200, 300 };
        const QVector<double> indentPositions{ 0,     .25,  .3125, .375, .4375, .5,
                                               .5625, .625, .6875, .75,  1 };
        for (int i = 0; i < indentStops.size(); ++i) {
            const int before = paragraphCommits;
            indent->setValue(60);
            const int x = xAt(indent, indentPositions[i]);
            mouse(indent, QEvent::MouseButtonPress, x);
            require(indent->value() == indentStops[i] && paragraphCommits == before,
                    "Indent drag missed a hidden quarter stop or committed before release");
            mouse(indent, QEvent::MouseButtonRelease, x);
            require(paragraphCommits == before + 1 && chosenIndent == indentStops[i]
                            && chosenGap == -1,
                    "Indent release did not commit once with the unchanged Publisher gap");
        }
        // Sweep both ways to reject extra subdivisions, including above 200%.
        for (const bool reverse : { false, true }) {
            QVector<int> visited;
            const int before = paragraphCommits;
            const int span = indent->width() - 2 * inset;
            mouse(indent, QEvent::MouseButtonPress, reverse ? indent->width() : 0);
            for (int offset = 0; offset <= span; ++offset) {
                mouse(indent, QEvent::MouseMove, inset + (reverse ? span - offset : offset));
                if (visited.isEmpty() || visited.last() != indent->value())
                    visited.append(indent->value());
            }
            auto expected = indentStops;
            if (reverse)
                std::reverse(expected.begin(), expected.end());
            require(visited == expected && paragraphCommits == before,
                    "Indent drag added stops, skipped quarter stops or committed a draft");
            mouse(indent, QEvent::MouseButtonRelease, reverse ? 0 : indent->width());
        }
        for (const bool hide : { false, true }) {
            indent->setValue(60);
            const int before = paragraphCommits;
            mouse(indent, QEvent::MouseButtonPress, xAt(indent, .375));
            if (hide) {
                indent->hide();
                indent->show();
            } else {
                QEvent cancel(QEvent::TouchCancel);
                QApplication::sendEvent(indent, &cancel);
            }
            mouse(indent, QEvent::MouseButtonRelease, xAt(indent, .375));
            require(indent->value() == 60 && paragraphCommits == before,
                    "Abandoned indent drag lost its fine value or committed");
        }
        QPointingDevice touchDevice("indentation test", 1, QInputDevice::DeviceType::TouchScreen,
                                    QPointingDevice::PointerType::Finger,
                                    QInputDevice::Capability::Position, 2, 0);
        const auto touch = [&](kobalt::ui::SettingSlider *control, QEvent::Type type,
                               QPoint position, bool secondFinger = false) {
            const auto state = type == QEvent::TouchBegin ? QEventPoint::Pressed
                    : type == QEvent::TouchEnd            ? QEventPoint::Released
                                                          : QEventPoint::Updated;
            QEventPoint point(0, state, position, control->mapToGlobal(position));
            QMutableEventPoint::setPosition(point, position);
            QList<QEventPoint> points{ point };
            if (secondFinger)
                points.append(QEventPoint(1, QEventPoint::Pressed, position,
                                          control->mapToGlobal(position)));
            QTouchEvent event(type, &touchDevice, Qt::NoModifier, points);
            QApplication::sendEvent(control, &event);
        };
        for (int i = 0; i < indentStops.size(); ++i) {
            indent->setValue(60);
            const int before = paragraphCommits;
            const QPoint point(xAt(indent, indentPositions[i]), indent->height() / 2);
            touch(indent, QEvent::TouchBegin, point);
            require(indent->value() == indentStops[i] && paragraphCommits == before,
                    "Touch indent drag differs from mouse snapping");
            touch(indent, QEvent::TouchEnd, point);
            require(paragraphCommits == before + 1 && chosenIndent == indentStops[i],
                    "Touch indent release did not commit its stop once");
        }
        indent->setValue(60);
        const int beforeTouchCancel = paragraphCommits;
        const QPoint quarter(xAt(indent, .3125), indent->height() / 2);
        const QPoint half(xAt(indent, .375), indent->height() / 2);
        touch(indent, QEvent::TouchBegin, quarter);
        touch(indent, QEvent::TouchUpdate, half);
        require(indent->value() == 50 && paragraphCommits == beforeTouchCancel,
                "Touch movement did not reach a hidden stop without committing");
        QTouchEvent touchCancel(QEvent::TouchCancel, &touchDevice);
        QApplication::sendEvent(indent, &touchCancel);
        touch(indent, QEvent::TouchEnd, half);
        require(indent->value() == 60 && paragraphCommits == beforeTouchCancel,
                "Cancelled touch drag lost its fine value or committed");
        touch(indent, QEvent::TouchBegin, quarter);
        touch(indent, QEvent::TouchUpdate, half, true);
        touch(indent, QEvent::TouchEnd, half);
        require(indent->value() == 60 && paragraphCommits == beforeTouchCancel,
                "Multi-touch indent drag lost its fine value or committed");
        indent->setValue(60);
        settings.fontDialog->findChild<QPushButton *>("rrParagraphIndentMore")->click();
        require(chosenIndent == 61, "Indent plus did not keep one-percent adjustment");
        settings.fontDialog->findChild<QPushButton *>("rrParagraphIndentLess")->click();
        require(chosenIndent == 60, "Indent minus snapped instead of adjusting one percent");
        indent->setValue(0);
        settings.fontDialog->findChild<QPushButton *>("rrParagraphIndentLess")->click();
        require(chosenIndent == -1, "Indent minus did not reach Publisher");
        settings.fontDialog->findChild<QPushButton *>("rrParagraphIndentMore")->click();
        require(chosenIndent == 0, "Indent plus did not leave Publisher at zero");
        const QVector<int> gapStops{ -1, 0, 25, 50, 75, 100, 125, 150, 175, 200 };
        const QVector<double> gapPositions{ 0, .2, .3, .4, .5, .6, .7, .8, .9, 1 };
        for (int i = 0; i < gapStops.size(); ++i) {
            const QPoint point(xAt(gap, gapPositions[i]), gap->height() / 2);
            for (const bool useTouch : { false, true }) {
                gap->setValue(60);
                const int before = paragraphCommits;
                if (useTouch)
                    touch(gap, QEvent::TouchBegin, point);
                else
                    mouse(gap, QEvent::MouseButtonPress, point.x());
                require(gap->value() == gapStops[i] && paragraphCommits == before,
                        "Paragraph gap missed a quarter stop or committed before release");
                if (useTouch)
                    touch(gap, QEvent::TouchEnd, point);
                else
                    mouse(gap, QEvent::MouseButtonRelease, point.x());
                require(chosenGap == gapStops[i] && chosenIndent == 0
                                && paragraphCommits == before + 1,
                        "Paragraph gap release did not commit once with unchanged indentation");
            }
        }
        for (const bool reverse : { false, true }) {
            QVector<int> visited;
            const int before = paragraphCommits;
            const int span = gap->width() - 2 * inset;
            mouse(gap, QEvent::MouseButtonPress, reverse ? gap->width() : 0);
            for (int offset = 0; offset <= span; ++offset) {
                mouse(gap, QEvent::MouseMove, inset + (reverse ? span - offset : offset));
                if (visited.isEmpty() || visited.last() != gap->value())
                    visited.append(gap->value());
            }
            auto expected = gapStops;
            if (reverse)
                std::reverse(expected.begin(), expected.end());
            require(visited == expected && paragraphCommits == before,
                    "Paragraph gap drag skipped quarter stops or committed a draft");
            mouse(gap, QEvent::MouseButtonRelease, reverse ? 0 : gap->width());
        }
        gap->setValue(60);
        const int beforeGapCancel = paragraphCommits;
        const QPoint gapQuarter(xAt(gap, .3), gap->height() / 2);
        touch(gap, QEvent::TouchBegin, gapQuarter);
        QTouchEvent gapCancel(QEvent::TouchCancel, &touchDevice);
        QApplication::sendEvent(gap, &gapCancel);
        touch(gap, QEvent::TouchEnd, gapQuarter);
        require(gap->value() == 60 && paragraphCommits == beforeGapCancel,
                "Cancelled paragraph gap drag lost its fine value or committed");
        settings.fontDialog->findChild<QPushButton *>("rrParagraphSpacingMore")->click();
        require(chosenGap == 61, "Paragraph gap plus did not keep one-percent adjustment");
        settings.fontDialog->findChild<QPushButton *>("rrParagraphSpacingLess")->click();
        require(chosenGap == 60, "Paragraph gap minus snapped instead of adjusting one percent");
        gap->setValue(0);
        settings.fontDialog->findChild<QPushButton *>("rrParagraphSpacingLess")->click();
        require(chosenGap == -1, "Paragraph gap minus did not reach Publisher");
        settings.fontDialog->findChild<QPushButton *>("rrParagraphSpacingMore")->click();
        require(chosenGap == 0, "Paragraph gap plus did not leave Publisher at zero");
        // Compare complete rail and label pixels with the original visible stops.
        for (auto *control : { indent, gap }) {
            for (const bool dark : { false, true }) {
                QPalette colors = control->palette();
                colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
                colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
                control->setPalette(colors);
                control->setValue(60);
                const auto withStops = control->grab().toImage();
                kobalt::ui::SettingSlider original(nullptr, -1, control->maximum());
                const bool isIndent = control == indent;
                const QVector<int> visible = isIndent ? QVector<int>{ -1, 0, 100, 200, 300 }
                                                      : QVector<int>{ -1, 0, 50, 100, 150, 200 };
                const QStringList labels = isIndent
                        ? QStringList{ kobalt::strings::get("reader.publisher_spacing"), "0",
                                       "100%", "200%", "300%" }
                        : QStringList{ kobalt::strings::get("reader.publisher_spacing"),
                                       "0",
                                       "50%",
                                       "100%",
                                       "150%",
                                       "200%" };
                original.setMarkers(visible, kobalt::ui::SettingSlider::MarkerMode::Stops);
                original.setMarkerLabels(labels);
                original.setPalette(colors);
                original.resize(control->size());
                original.setValue(60);
                require(original.grab().toImage() == withStops,
                        "Paragraph slider differs from the original visible markers and labels");
                control->setAdditionalStops({});
                require(control->grab().toImage() == withStops,
                        "Hidden paragraph stops moved the handle, visible ticks or labels, or drew extra marks");
                control->setAdditionalStops(isIndent ? QVector<int>{ 25, 50, 75, 125, 150, 175 }
                                                     : QVector<int>{ 25, 75, 125, 175 });
                const int markerY = kobalt::ui::metrics::control::sliderLabelHeight
                        + kobalt::ui::metrics::control::sliderMarkerTop
                        + kobalt::ui::metrics::control::sliderMarkerHeight / 2;
                for (int i = 0; i < visible.size(); ++i)
                    require(qGray(withStops.pixel(xAt(control, double(i) / (visible.size() - 1)),
                                                  markerY))
                                    == (dark ? 255 : 0),
                            "Paragraph visible ticks no longer use the original equal spacing");
                for (double position :
                     (isIndent ? QVector<double>{ .3125, .375, .4375, .5625, .625, .6875 }
                               : QVector<double>{ .3, .5, .7, .9 }))
                    require(qGray(withStops.pixel(xAt(control, position), markerY))
                                    == (dark ? 0 : 255),
                            "Hidden paragraph stop drew a visible tick");
            }
        }
        settings.dismissFontChoices();
    }
    {
        kobalt::ui::PagedText text(&reader);
        text.resize(300, 260);
        text.setMargins({});
        text.setTextFont(kobalt::ui::interfaceFont(30));
        text.show();
        const auto lineGap = [&text] {
            const auto image = text.grab().toImage().convertToFormat(QImage::Format_RGB32);
            QVector<int> starts;
            bool previousInk = false;
            for (int y = 0; y < image.height(); ++y) {
                bool ink = false;
                for (int x = 0; x < image.width(); ++x)
                    ink |= qGray(image.pixel(x, y)) < 80;
                if (ink && !previousInk)
                    starts.append(y);
                previousInk = ink;
            }
            require(starts.size() == 3, "Dictionary line spacing clipped or overlapped a line");
            return starts[1] - starts[0];
        };
        text.setMinimumLineHeight(100);
        text.setHtml("<p style='line-height:8px'>Hg<br>Hg<br>Hg</p>");
        const int standard = lineGap();
        text.setMinimumLineHeight(120);
        require(lineGap() > standard, "Dictionary leading did not increase with the larger size");
        text.setHtml("<p style='line-height:150%'>Hg<br>Hg<br>Hg</p>");
        const int roomy = lineGap();
        text.setMinimumLineHeight(110);
        require(lineGap() == roomy, "Automatic dictionary leading reduced existing wider spacing");
        text.setHtml("<p style='line-height:100%'>Hg<br>Hg<br>Hg</p>");
        text.setMinimumLineHeight(120);
        text.setMinimumLineHeight(0);
        require(lineGap() == standard, "Restoring the dictionary size kept added line spacing");
        text.setMinimumLineHeight(120);
        const int beforeFontChange = lineGap();
        QEvent fontChange(QEvent::FontChange);
        QApplication::sendEvent(&text, &fontChange);
        require(lineGap() == beforeFontChange, "A font refresh discarded dictionary line spacing");
    }
    {
        kobalt::DictionaryPanel panel(&reader);
        auto *definition =
                dynamic_cast<kobalt::ui::PagedText *>(panel.findChild<QWidget *>("rrDefinition"));
        require(definition, "Dictionary definition widget is missing");
        const auto body = kobalt::ui::brandFont(30).family();
        const auto otherBody = kobalt::ui::chromeFont(30).family();
        const auto overrideFamily = kobalt::ui::interfaceFont(30).family();
        panel.setBookFont(body);
        panel.setDefinitionFont({}, 120);
        require(definition->textFont().family() == body,
                "Default dictionary font did not follow the book body");
        panel.setBookFont(otherBody);
        require(definition->textFont().family() == otherBody,
                "Default dictionary font did not follow a changed book font");
        panel.setDefinitionFont(overrideFamily, 140);
        const auto overrideSize = definition->textFont().pixelSize();
        panel.setBookFont(body);
        require(definition->textFont().family() == overrideFamily
                        && definition->textFont().pixelSize() == overrideSize,
                "Changing the book font replaced the explicit dictionary override");
        panel.setDefinitionFont({}, 140);
        require(definition->textFont().family() == body
                        && definition->textFont().pixelSize() == overrideSize,
                "Resetting the dictionary face did not restore the book font at the selected dictionary size");
    }
    for (const bool dark : { false, true }) {
        QWidget host;
        host.resize(600, 800);
        host.setPalette(kobalt::ui::readerPalette(host.palette(), dark));
        host.show();
        kobalt::DictionaryPanel panel(&host);
        kobalt::DictionaryResult contrast;
        contrast.received = true;
        contrast.dictionary = "Contrast fixture";
        const QColor foreground = dark ? Qt::black : Qt::white;
        const QColor background = dark ? Qt::white : Qt::black;
        contrast.html = QString("<p><span style='color:%1;background-color:%2'>MMMM</span></p>")
                                .arg(foreground.name(), background.name());
        panel.open("word", contrast, QRect(10, 10, 40, 40));
        app.processEvents();
        auto *definition = panel.findChild<QWidget *>("rrDefinition");
        require(definition && definition->isVisible(),
                "Contrast fixture did not open a dictionary definition");
        const auto image = definition->grab().toImage().convertToFormat(QImage::Format_RGB32);
        const auto capture = qEnvironmentVariable("KOBALT_RICH_TEXT_CAPTURE");
        if (!capture.isEmpty())
            require(image.save(capture
                               + QString("/dictionary-%1.png").arg(dark ? "dark" : "light")),
                    "Cannot save dictionary contrast capture");
        QRect backdrop;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y) == background)
                    backdrop |= QRect(x, y, 1, 1);
        require(backdrop.width() > 30 && backdrop.height() > 20,
                "Dictionary definition lost its explicit background");
        int ink = 0;
        for (int y = backdrop.top(); y <= backdrop.bottom(); ++y)
            for (int x = backdrop.left(); x <= backdrop.right(); ++x)
                ink += image.pixelColor(x, y) == foreground;
        require(ink > 100, "Dictionary definition text is invisible against its background");
    }
    {
        const QRect keyboard(0, 900, 1072, 548), key(927, 1235, 95, 95), query(0, 0, 595, 112);
        require(kobalt::keyboardEditDamage(QRegion(key) + query, keyboard, query)
                        && kobalt::keyboardEditDamage(key, keyboard, query)
                        && kobalt::keyboardEditDamage(query, keyboard, query),
                "Key and field edits must share one partial keyboard update");
        require(!kobalt::keyboardEditDamage(QRegion(key).united(query).boundingRect(), keyboard,
                                            query)
                        && !kobalt::keyboardEditDamage(keyboard, keyboard, query)
                        && !kobalt::keyboardEditDamage(key, {}, query)
                        && !kobalt::keyboardEditDamage(QRegion(key) + QRect(0, 400, 20, 20),
                                                       keyboard, query),
                "Keyboard opening and unrelated panel damage must keep the original refresh policy");
        constexpr quint64 modes = 2 | 4 | 8, ordinary = (quint64(1) << 38) | 8;
        using kobalt::completeKeyboardWaveform;
        require(completeKeyboardWaveform(key, keyboard, 8, ordinary, modes) == ordinary,
                "A native mode-only key policy must receive the text waveform so field/key damage can split");
        require(completeKeyboardWaveform(key, keyboard, 4, ordinary, modes)
                        == ((ordinary & ~modes) | 4),
                "Completing a keyboard waveform must preserve partial update mode");
        require(completeKeyboardWaveform(query, keyboard, 8, ordinary, modes) == 8
                        && completeKeyboardWaveform(key, {}, 8, ordinary, modes) == 8
                        && completeKeyboardWaveform(QRect(0, 0, 1072, 1448), keyboard, 8, ordinary,
                                                    modes)
                                == 8,
                "Keyboard policy completion must not change header, hidden-keyboard or screen policies");
        require(completeKeyboardWaveform(key, keyboard, 0, ordinary, modes) == 0
                        && completeKeyboardWaveform(key, keyboard, 12, ordinary, modes) == 12
                        && completeKeyboardWaveform(key, keyboard, 0x48, ordinary, modes) == 0x48,
                "Explicit waveforms and invalid update modes must keep their original policy");
        QVector<QPair<QRect, quint64>> policies{ { keyboard, ordinary },
                                                 { keyboard, (ordinary & ~modes) | 2 } };
        const auto policy = [&](const auto &entries) {
            return kobalt::commonKeyboardPolicy(entries, modes, 8, 2);
        };
        require(policy(policies) == ordinary,
                "Mixed native keyboard modes lost the full update policy");
        std::reverse(policies.begin(), policies.end());
        require(policy(policies) == ordinary, "Keyboard cache policy depends on widget flag order");
        QImage pixels(1072, 1448, QImage::Format_ARGB32);
        pixels.fill(Qt::white);
        kobalt::KeyboardRefreshCache cache;
        require(!cache.unchanged(&pixels, keyboard, keyboard, ordinary, false)
                        && cache.unchanged(&pixels, key, keyboard, policy(policies), true),
                "Identical keyboard pixels with mixed native modes bypass the refresh cache");
        pixels.setPixelColor(key.center(), Qt::black);
        require(!cache.unchanged(&pixels, key, keyboard, policy(policies), true),
                "A changed keyboard key was incorrectly suppressed");
        const quint64 partial = (ordinary & ~modes) | 4;
        require(!cache.unchanged(&pixels, key, keyboard, partial, true)
                        && !cache.unchanged(&pixels, key, keyboard, ordinary, true),
                "A new full keyboard update was suppressed after a partial update");
        policies[0].second ^= quint64(1) << 39;
        require(policy(policies) == 0,
                "Different keyboard waveforms were combined for refresh suppression");
        policies = { { key, ordinary | 2 } };
        require(policy(policies) == 0, "An invalid keyboard mode was accepted by the pixel cache");
    }
    for (const bool dark : { false, true }) {
        const QColor paper(dark ? Qt::black : Qt::white), ink(dark ? Qt::white : Qt::black);
        for (const auto edge : { Qt::TopEdge, Qt::BottomEdge, Qt::LeftEdge, Qt::RightEdge }) {
            const bool horizontal = edge == Qt::TopEdge || edge == Qt::BottomEdge;
            for (const qreal scale : { .5, .558, .75, 1., 1.116, 1.25, 2. }) {
                for (const qreal offset : { 0., .2, .4, .6, .8 }) {
                    QImage image(QSize(qCeil(100 * scale) + 2, qCeil(100 * scale) + 2),
                                 QImage::Format_ARGB32_Premultiplied);
                    image.fill(paper);
                    {
                        QPainter painter(&image);
                        painter.translate(offset, offset);
                        painter.scale(scale, scale);
                        kobalt::ui::paintEdges(painter, QRect(0, 0, 100, 100), edge, ink, 3);
                        // An opaque child touches the inner border on half of the edge.
                        const QRect child = edge == Qt::TopEdge ? QRect(50, 3, 50, 97)
                                : edge == Qt::BottomEdge        ? QRect(50, 0, 50, 97)
                                : edge == Qt::LeftEdge          ? QRect(3, 50, 97, 50)
                                                                : QRect(0, 50, 97, 50);
                        painter.fillRect(child, paper);
                    }
                    const int exposed = qRound(offset + 25 * scale),
                              covered = qRound(offset + 75 * scale);
                    int visibleInk = 0;
                    for (int across = 0; across < image.width(); ++across) {
                        const auto bare = image.pixelColor(horizontal ? exposed : across,
                                                           horizontal ? across : exposed);
                        const auto adjacent = image.pixelColor(horizontal ? covered : across,
                                                               horizontal ? across : covered);
                        require(bare == adjacent,
                                "An opaque neighbor clips a scaled border differently from transparent content");
                        visibleInk += bare == ink;
                    }
                    require(visibleInk > 0, "Pixel alignment erased a scaled border");
                }
            }
        }
    }
    for (const bool lower : { false, true }) {
        class Keyboard final : public kobalt::KeyboardController
        {
        public:
            explicit Keyboard(QWidget *host) : host(host) { }

            bool active = false;

            bool show() override
            {
                active = true;
                host->window()->setProperty("reader_keyboard_bounds",
                                            QRect(0, host->height() - 300, host->width(), 300));
                return true;
            }

            void hide() override
            {
                active = false;
                host->window()->setProperty("reader_keyboard_bounds", QRect());
            }

            void place() override { }

            QWidget *host;
        };

        Keyboard *keyboard = nullptr;
        kobalt::DictionaryPanel panel(&reader, [&](QWidget *host, QLineEdit *) {
            require(host == &reader,
                    "The keyboard must belong to the reader, separately from the dictionary sheet");
            auto value = std::make_unique<Keyboard>(host);
            keyboard = value.get();
            return value;
        });
        panel.setBookFont(kobalt::ui::brandFont(kobalt::ui::metrics::chrome::titleText).family());
        const QRect selection(20, lower ? 20 : reader.height() - 80, 50, 40);
        kobalt::DictionaryResult missing;
        missing.received = missing.restored = true;
        missing.dictionary = "Spanish";
        missing.dictionaryId = "es";
        if (!lower)
            missing.html = "<p>A matching definition.</p>";
        panel.open("hablaban", missing, selection);
        const auto original = panel.geometry();
        auto *edit = panel.findChild<QPushButton *>("rrDictionaryEdit");
        auto *query = panel.findChild<QLineEdit *>("rrDictionaryQuery");
        auto *cancel = panel.findChild<QPushButton *>("rrDictionaryClose");
        auto *submit = panel.findChild<QPushButton *>("rrDictionarySubmit");
        auto *definition = panel.findChild<QWidget *>("rrDefinition");
        const auto editImage = edit->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        int editInk = 0;
        for (int y = 4; y < editImage.height() - 4; ++y)
            for (int x = 4; x < editImage.width() - 4; ++x)
                editInk += editImage.pixelColor(x, y) != edit->palette().color(QPalette::Window);
        require(editInk > 20, "The lookup edit button must paint its pencil icon");
        require(editImage.pixelColor(0, editImage.height() / 2).rgba()
                        == edit->palette().color(QPalette::Mid).rgba(),
                "The lookup edit cell needs a left divider");
        if (lower) {
            // Transparent text and opaque buttons must leave the same rule at
            // fractional preview positions, including Retina pixel scales.
            for (const qreal scale : { .33, .5, .558, .565, .75, 1., 1.116, 1.25, 2. }) {
                for (const qreal offset : { 0., .2, .4, .6, .8 }) {
                    QImage image(
                            QSize(qCeil(panel.width() * scale), qCeil(panel.height() * scale) + 1),
                            QImage::Format_ARGB32_Premultiplied);
                    image.fill(panel.palette().color(QPalette::Window));
                    {
                        QPainter painter(&image);
                        painter.translate(0, offset);
                        painter.scale(scale, scale);
                        panel.render(&painter);
                    }
                    const auto ink = panel.palette().color(QPalette::WindowText);
                    const auto rulePixels = [&](int x) {
                        int count = 0;
                        for (int y = 0;
                             y < qCeil(int(kobalt::ui::metrics::chrome::heavyRule) * scale) + 2;
                             ++y)
                            count += image.pixelColor(qRound(x * scale), y) == ink;
                        return count;
                    };
                    const int wordRule = rulePixels(int(kobalt::ui::metrics::chrome::inset) / 2);
                    require(wordRule > 0 && wordRule == rulePixels(edit->geometry().center().x())
                                    && wordRule == rulePixels(cancel->geometry().center().x()),
                            "Dictionary rule differs below transparent text and opaque buttons at a fractional pixel offset");
                }
            }
        }
        int requests = 0;
        QString requested;
        panel.requested = [&](const QString &value) {
            ++requests;
            requested = value;
        };
        edit->click();
        auto *heading = panel.findChild<QLabel *>("rrDictionaryWord");
        require(query->font().family() == heading->font().family()
                        && query->font().pixelSize() == heading->font().pixelSize(),
                "Editing the lookup must retain its heading font and size");
        require(query->isVisible() && keyboard->active && query->text() == "hablaban",
                "Lookup must remain editable with its current query, with or without a definition");
        require(panel.y() == 0 && panel.height() <= original.height() && definition->isVisible(),
                "Editing must move the compact dictionary sheet up and retain its definition");
        require(panel.geometry().bottom()
                        < reader.property("reader_keyboard_bounds").toRect().top(),
                "The dictionary sheet must not overlap its sibling keyboard");
        require(panel.findChild<QLabel *>("rrDictionaryEditHint")->isVisible(),
                "Editing must explain how to update the dictionary word");
        query->setText(" \n\t ");
        require(!submit->isEnabled(), "A blank lookup must not submit");
        keyboard->submitted();
        require(requests == 0 && query->isVisible(), "Keyboard submission must reject blank text");
        query->setText(QString::fromUtf8("  oír  "));
        keyboard->submitted();
        keyboard->submitted();
        require(requests == 1 && requested == QString::fromUtf8("oír") && !keyboard->active,
                "Keyboard submission must preserve accents, trim whitespace and run once");
        require(panel.geometry() == original && definition->isVisible() && !query->isVisible(),
                "Submission must restore the original dictionary sheet");
        require(panel.findChild<QPushButton *>("rrDictionarySelect")->accessibleDescription()
                        == "Spanish",
                "Editing must retain the selected dictionary");
        panel.open(requested, missing, selection);
        edit->click();
        require(query->text() == requested, "Reopening the editor must use the submitted lookup");
        query->setText("cancelled");
        cancel->click();
        require(requests == 1 && !keyboard->active && panel.geometry() == original,
                "Cancelling must retain the previous lookup without another request");
        edit->click();
        require(query->text() == requested, "Cancelled text must not replace the query");
        panel.hide();
        require(!keyboard->active && !query->hasFocus(),
                "Hiding lookup must dismiss its keyboard and focus");
        panel.open("another selection", missing, selection);
        edit->click();
        require(query->text() == "another selection",
                "A new selection must discard an unfinished edit");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(query, &escape);
        require(!keyboard->active && !query->isVisible() && panel.isVisible(),
                "Escape must cancel the edit and retain the definition");
    }
    {
        kobalt::DictionaryPanel panel(&reader);
        kobalt::DictionaryResult entry;
        entry.received = true;
        entry.dictionary = "English monolingual";
        entry.dictionaryId = "downloaded:en-en";
        entry.html = "<p>A surname from Cornwall.</p>";
        entry.source = QString::fromUtf8("WordNet 3.0 · English Wiktionary");
        panel.open("Trevelyan", entry, QRect(100, 100, 40, 40));
        app.processEvents();
        auto *body =
                dynamic_cast<kobalt::ui::PagedText *>(panel.findChild<QWidget *>("rrDefinition"));
        require(body->pages() == 1,
                "Compact dictionary sources must not add a page to a short definition");
        if (qEnvironmentVariableIsSet("KOBALT_DICTIONARY_CONTROLS_CAPTURE"))
            panel.grab().save(qEnvironmentVariable("KOBALT_DICTIONARY_CONTROLS_CAPTURE")
                              + "/source-footer.png");
        const auto before = body->grab().toImage();
        panel.place(QRect(100, 100, 40, 40));
        require(body->grab().toImage() == before,
                "Unchanged dictionary source layout changed its pixels");
        reader.resize(720, 1448);
        panel.place(QRect(100, 100, 40, 40));
        require(body->pages() == 1, "Narrow source footer must remain compact");
        reader.resize(1072, 1448);
    }
    {
        kobalt::DictionaryPanel panel(&reader);
        QVector<kobalt::DictionaryChoice> choices{ { "en", "English" },
                                                   { "en-nl", "English to Dutch" } };
        panel.setChoices(choices);
        kobalt::DictionaryResult result;
        result.received = true;
        result.html = "<p>A definition.</p>";
        result.dictionary = "English to Dutch";
        panel.open("word", result, QRect(100, 100, 40, 40));
        auto *selector = panel.findChild<QPushButton *>("rrDictionarySelect");
        const int currentWidth = selector->width();
        const auto wordBounds = panel.findChild<QLabel *>("rrDictionaryWord")->geometry();
        require(currentWidth > int(kobalt::ui::metrics::chrome::cellWidth)
                        && selector->text().isEmpty(),
                "Dictionary selection must leave room for its icon and menu arrow");
        require(selector->accessibleDescription() == result.dictionary,
                "The current dictionary name must remain available to accessibility");
        choices.append({ "long", QString(100, 'W') });
        panel.setChoices(choices);
        panel.place(QRect(100, 100, 40, 40));
        require(selector->width() == currentWidth,
                "A different dictionary's long name must not widen the current selector");
        result.dictionary = "English";
        panel.open("word", result, QRect(100, 100, 40, 40));
        require(selector->width() == currentWidth
                        && panel.findChild<QLabel *>("rrDictionaryWord")->geometry() == wordBounds,
                "Changing dictionary names must not move or resize the lookup header");
        result.dictionary = "English monolingual";
        panel.open("word", result, QRect(100, 100, 40, 40));
        require(selector->width() == currentWidth
                        && selector->accessibleDescription() == result.dictionary,
                "A long dictionary name must retain the same icon cell and update accessibility");
        if (qEnvironmentVariableIsSet("KOBALT_DICTIONARY_CONTROLS_CAPTURE"))
            panel.grab().save(qEnvironmentVariable("KOBALT_DICTIONARY_CONTROLS_CAPTURE")
                              + "/icon-selector.png");
    }
    kobalt::DictionaryPanel dictionary(&reader);
    QVector<kobalt::DictionaryChoice> dictionaries;
    for (int i = 0; i < 31; ++i)
        dictionaries.append(
                { QString::number(i), QString("Dictionary %1").arg(i), i < 7 ? "en" : "es" });
    dictionary.setChoices(dictionaries);
    dictionary.setLanguage("en");
    kobalt::DictionaryResult result;
    result.dictionary = "Dictionary 8";
    result.dictionaryId = "8";
    result.received = true;
    result.html = QString("<p>A definition long enough to require several pages.</p>").repeated(50);
    for (const auto size : { QSize(1072, 1448), QSize(1448, 1072), QSize(720, 1448) }) {
        reader.resize(size);
        dictionary.open("word", result, QRect(100, 100, 40, 40));
        app.processEvents();
        auto *definition = dynamic_cast<kobalt::ui::PagedText *>(
                dictionary.findChild<QWidget *>("rrDefinition"));
        auto *definitionPager =
                dictionary.findChild<QPushButton *>("rrDictionaryNext")->parentWidget();
        require(definitionPager && definitionPager->isVisible(), "Long definition lacks its pager");
        const int standardPages = definition->pages();
        dictionary.setDefinitionFont({}, 140);
        app.processEvents();
        require(definition->pages() > standardPages,
                "Larger dictionary text did not repaginate the definition");
        dictionary.setDefinitionFont("IBM Plex Mono", 120);
        require(definition->textFont().family() == "IBM Plex Mono"
                        && definition->textFont().pixelSize()
                                == qRound(int(kobalt::ui::metrics::chrome::labelText) * 1.2),
                "Dictionary font override was not applied to definition text");
        dictionary.setDefinitionFont({}, 100);
        app.processEvents();
        const QRect definitionGeometry = definition->geometry();
        const int definitionPages = definition->pages();
        dictionary.findChild<QPushButton *>("rrDictionarySelect")->click();
        app.processEvents();
        require(definition->geometry() == definitionGeometry
                        && definition->pages() == definitionPages,
                "Dictionary picker changed definition geometry or pagination");
        auto *choices = dictionary.findChild<QWidget *>("rrDictionaryChoices");
        auto *count = dictionary.findChild<QLabel *>("rrDictionaryChoicesPages");
        // A narrow sheet uses compact header cells.
        const int closeWidth = kobalt::isNarrowWidth(dictionary.width(), kobalt::ui::density())
                ? int(kobalt::ui::metrics::chrome::compactCellWidth)
                : int(kobalt::ui::metrics::chrome::cellWidth);
        require(dictionary.findChild<QPushButton *>("rrDictionaryClose")->size()
                        == QSize(closeWidth, kobalt::ui::metrics::chrome::actionHeight),
                "Dictionary close is not a full header cell");
        require(dictionary.x() == 0 && dictionary.width() == reader.width(),
                "Dictionary sheet does not span the screen");
        require(!definitionPager->isVisible(),
                "Definition pager remains visible under dictionary choices");
        require(count->isVisible() && count->text().contains(" of "),
                "Dictionary choices lack a page count");
        require(dictionary.rect().contains(choices->geometry()),
                "Dictionary choices exceed their panel");
        bool selected = false;
        for (auto *button : choices->findChildren<QPushButton *>()) {
            if (!button->isVisible())
                continue;
            require(choices->rect().contains(
                            QRect(button->mapTo(choices, QPoint()), button->size())),
                    "Dictionary choice or pager is clipped");
            if (button->objectName().startsWith("rrDictionaryChoice") && button->isCheckable())
                require(button->height() == kobalt::ui::metrics::lookup::choiceHeight,
                        "Dictionary choice row lost its compact height");
            selected |= button->isChecked() && button->text() == result.dictionary;
        }
        require(selected, "Dictionary picker did not open at the selected dictionary");
        if (qEnvironmentVariableIsSet("KOBALT_DICTIONARY_CONTROLS_CAPTURE"))
            dictionary.grab().save(
                    qEnvironmentVariable("KOBALT_DICTIONARY_CONTROLS_CAPTURE")
                    + QString("/chooser-%1-%2.png").arg(size.width()).arg(size.height()));

        QSet<QString> reached;
        const auto firstPage = count->text();
        int turns = 0, maximumChoices = 0;
        do {
            int visibleChoices = 0;
            QVector<QRect> cells;
            for (auto *button : choices->findChildren<QPushButton *>()) {
                if (!button->isVisible() || !button->objectName().startsWith("rrDictionaryChoice")
                    || !button->isCheckable())
                    continue;
                ++visibleChoices;
                require(!reached.contains(button->text()),
                        "Dense dictionary chooser repeats a dictionary across pages");
                reached.insert(button->text());
                for (const auto &cell : cells)
                    require(!cell.intersects(button->geometry()),
                            "Dense dictionary choices overlap");
                cells.append(button->geometry());
                if (size.width() >= kobalt::ui::px(900) && button->x() == 0) {
                    const auto image = button->grab().toImage();
                    require(image.pixelColor(image.width() - 1, image.height() / 2)
                                    == kobalt::ui::dividerColor(button),
                            "Dictionary columns lack their vertical separator");
                }
                require(choices->rect().contains(button->geometry()),
                        "Dense dictionary choice escapes the sheet");
            }
            maximumChoices = qMax(maximumChoices, visibleChoices);
            dictionary.findChild<QPushButton *>("rrDictionaryChoicesNext")->click();
            require(++turns < 40, "Dense dictionary chooser did not wrap");
        } while (count->text() != firstPage);
        require(reached.size() == dictionaries.size(),
                "Dense dictionary chooser lost installed dictionaries");
        if (size.width() >= kobalt::ui::px(900))
            require(maximumChoices >= 4,
                    "Two-column dictionary chooser did not increase visible options");

        dictionary.findChild<QPushButton *>("rrDictionarySelect")->click();
        app.processEvents();
        require(definitionPager->isVisible() && definition->geometry() == definitionGeometry
                        && definition->pages() == definitionPages,
                "Closing dictionary choices did not restore the definition pager without reflow");
    }
    // The header names the word before the dictionary, in the book face. A
    // word the face cannot draw in full is drawn in the interface sans. The
    // Latin-only title serif stands in for the book face here.
    {
        auto *word = dictionary.findChild<QLabel *>("rrDictionaryWord");
        auto *select = dictionary.findChild<QPushButton *>("rrDictionarySelect");
        const auto serif = kobalt::ui::brandFont(kobalt::ui::metrics::chrome::titleText).family();
        dictionary.setBookFont(serif);
        const auto familyFor = [&](const QString &text) {
            dictionary.open(text, result, QRect(100, 100, 40, 40));
            app.processEvents();
            word->ensurePolished();
            return word->font().family();
        };
        require(familyFor("sister") == serif && word->text() == "sister",
                "The dictionary header does not show the word in the book face");
        require(word->geometry().right() < select->x(),
                "The word overlaps the dictionary selector");
        require(familyFor(QString::fromUtf8("αδελφή")) != serif,
                "A Greek word used a Latin-only book face");
        require(familyFor(QString::fromUtf8("tiếng")) != serif,
                "A partly covered word mixed the book face with fallback glyphs");
        familyFor(QString(200, 'm'));
        require(word->text().endsWith(QString::fromUtf8("…"))
                        && word->geometry().right() < select->x(),
                "A long word was not cut before the selector");
    }
    reader.hide();
    kobalt::ui::Panel paper;
    paper.resize(200, 160);
    for (const auto color : { Qt::white, Qt::black }) {
        QPalette palette = paper.palette();
        palette.setColor(QPalette::Window, color);
        paper.setPalette(palette);
        QImage pixels(paper.size(), QImage::Format_RGB32);
        pixels.fill(Qt::red);
        // Opaque panels must fill their own pixels even when Qt does not
        // prepare a background. Include a partial repaint over old content.
        const QRect damage(20, 30, 100, 90);
        paper.render(&pixels, damage.topLeft(), QRegion(damage), QWidget::DrawChildren);
        require(QColor::fromRgb(pixels.pixel(damage.center())) == QColor(color),
                "Opaque panel left stale book pixels in its background");
        require(QColor::fromRgb(pixels.pixel(0, 0)) == QColor(Qt::red),
                "Panel painted outside its dirty region");
    }
    QWidget window;
    kobalt::ui::PageNavigation navigation(&window, "pages");
    for (const auto size : { QSize(1072, 1448), QSize(1448, 1072), QSize(920, 600) }) {
        window.resize(size);
        navigation.layout(size, size.height() == 600);
        const QRect previous(navigation.previous->mapTo(&window, QPoint()),
                             navigation.previous->size());
        const QRect next(navigation.next->mapTo(&window, QPoint()), navigation.next->size());
        require(!previous.intersects(next) && window.rect().contains(next),
                "Pager buttons overlap or leave the screen");
        require(navigation.title->geometry().center().x() == window.rect().center().x(),
                "Heading is not centered on screen");
        require(navigation.back->isHidden() == (size.height() == 600),
                "Font dropdown shows a Back button");
    }
    QWidget panel;
    new QVBoxLayout(&panel);
    panel.resize(1000, 120);
    panel.show();
    app.processEvents();
    kobalt::ui::SampleLabel sample(&panel);
    sample.optical = true;
    sample.setGeometry(0, 0, 400, 68);
    sample.show();
    kobalt::ui::readerStyle(&sample, "background:white;color:black;font-size:32px;");
    {
        sample.ensurePolished();
        const int wordWidth = sample.fontMetrics().boundingRect("Ancizar").width();
        sample.setGeometry(0, 0, wordWidth + 8, 64);
        sample.setAlignment(Qt::AlignCenter);
        sample.setText("Ancizar Serif");
        sample.setWordWrap(false);
        const auto singleLine = sample.grab().toImage();
        sample.setWordWrap(true);
        const auto wrapped = sample.grab().toImage();
        require(wrapped != singleLine, "A multiword font name still used a single elided line");
        int bands = 0;
        bool previous = false;
        for (int y = 0; y < wrapped.height(); ++y) {
            bool ink = false;
            for (int x = 0; x < wrapped.width(); ++x)
                ink |= qGray(wrapped.pixel(x, y)) < 128;
            if (ink && !previous)
                ++bands;
            previous = ink;
        }
        require(bands == 2, "A multiword font name did not paint two complete lines");
        sample.setText(QString(60, 'M'));
        const auto longWord = sample.grab().toImage();
        sample.setWordWrap(false);
        require(sample.grab().toImage() == longWord,
                "An oversized single word wrapped instead of using an ellipsis");
        sample.setText("Libron");
        const auto shortName = sample.grab().toImage();
        sample.setWordWrap(true);
        require(sample.grab().toImage() == shortName,
                "Wrapping changed a short font name's appearance");
        sample.setWordWrap(false);
        sample.setGeometry(0, 0, 400, 68);
    }
    for (const auto *name : { "Libron", "Georgia", "gyp", "ÅÉê" }) {
        sample.setText(QString::fromUtf8(name));
        app.processEvents();
        const auto image = sample.grab().toImage();
        QRect ink;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qGray(image.pixel(x, y)) < 128)
                    ink = ink.united(QRect(x, y, 1, 1));
        require(!ink.isEmpty() && qAbs(ink.center().y() - sample.rect().center().y()) <= 2,
                "Font sample ink is not centered");
    }
    // The sample changes actual glyph ink, and Bionic modes suppress stored
    // overall weight. Leaving the mode restores the original sample pixels.
    sample.setText("Reading office");
    sample.setFont(kobalt::ui::fontSample("Libron"));
    const auto normalSample = sample.grab().toImage();
    auto inkAmount = [](const QImage &image) {
        double total = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                total += (255 - qGray(image.pixel(x, y))) / 255.0;
        return total;
    };
    sample.setAppearance({ 8, "off" });
    const auto heavierSample = sample.grab().toImage();
    require(inkAmount(heavierSample) > inkAmount(normalSample) * 1.08,
            "Weight did not thicken the font preview");
    for (const auto *mode : { "bold" }) {
        sample.setAppearance({ 0, mode });
        const auto emphasized = sample.grab().toImage();
        require(emphasized != normalSample && inkAmount(emphasized) > inkAmount(normalSample),
                "Bionic mode did not emphasize the font preview");
        sample.setAppearance({ 8, mode });
        require(sample.grab().toImage() == emphasized,
                "Bionic font preview stacked stored weight on its prefixes");
    }
    sample.setAppearance({ 0, "off" });
    require(sample.grab().toImage() == normalSample, "Font preview did not restore normal ink");
    const auto prefixes =
            kobalt::ui::samplePrefixes(QString::fromUtf8("Reading αλφα שלום a\xcc\x81"));
    require((prefixes == QVector<QPair<int, int>>{ { 0, 4 }, { 8, 2 }, { 13, 2 }, { 18, 2 } }),
            "Sample prefixes lost Unicode letters or combining marks");
    for (const auto *word :
         { u8"abⱠcd", u8"abᴫcd", u8"abꞳcd", u8"abᲀcd", u8"ＡＢＣＤＥ", u8"ab\U0001df00cd" }) {
        const auto text = QString::fromUtf8(word);
        const auto letter = text.toUcs4().at(2);
        // Qt5's Unicode tables predate some letters supported by the engine.
        if (!QChar::isLetter(letter))
            continue;
        const auto prefix = kobalt::ui::samplePrefixes(text);
        const int length = letter > 0xffff ? 4 : 3;
        require((prefix == QVector<QPair<int, int>>{ { 0, length } }),
                "An extended Latin or Cyrillic letter split a font-preview prefix");
    }
    require(kobalt::ui::samplePrefixes(QString::fromUtf8("العربية 中文 한국어")).isEmpty(),
            "Sample emphasis changed contextual or unsegmented scripts");
    require((kobalt::ui::samplePrefixes(QString::fromUtf8("rea\xc2\xad"
                                                          "d"))
             == QVector<QPair<int, int>>{ { 0, 2 } }),
            "Soft hyphen changed the sample prefix");
    sample.hide();
    kobalt::ui::Button action(&panel);
    action.setGeometry(0, 0, 92, 68);
    kobalt::ui::iconAction(&action, kobalt::ui::ControlIcon::Layout, "Page layout");
    action.show();
    app.processEvents();
    const auto idle = action.grab().toImage();
    action.setDown(true);
    const auto pressed = action.grab().toImage();
    require(idle != pressed, "Icon action has no pressed feedback");
    require(qGray(pressed.pixel(8, 8)) < 128, "Pressed icon action does not fill its background");
    action.setDown(false);
    action.hide();
    {
        using namespace kobalt::ui;
        const int savedDensity = density();
        for (const int ppi : { 167, 212, 300 })
            for (const bool dark : { false, true }) {
                setDensity(ppi);
                QWidget host;
                host.resize(px(200), px(140));
                host.setProperty("reader_dark", dark);
                auto colors = originalPalette;
                const QColor ink = dark ? Qt::white : Qt::black,
                             paper = dark ? Qt::black : Qt::white;
                colors.setColor(QPalette::Window, paper);
                colors.setColor(QPalette::WindowText, ink);
                colors.setColor(QPalette::Mid, QColor("#999999"));
                colors.setColor(QPalette::Midlight, dark ? QColor("#333333") : QColor("#dddddd"));
                host.setPalette(colors);
                readerStyle(&host, styles::reader());
                host.show();
                PaintCountingButton button(&host);
                button.setGeometry(px(10), px(10), px(132), px(80));
                button.setCheckable(true);
                button.setIconSize(QSize(metrics::control::iconSize, metrics::control::iconSize));
                button.setIconConfiguration({ ControlIcon::Close, IconRendition::Action });
                int clicks = 0, toggles = 0;
                QObject::connect(&button, &QPushButton::clicked, &host, [&] { ++clicks; });
                QObject::connect(&button, &QPushButton::toggled, &host, [&] { ++toggles; });
                button.show();
                app.processEvents();
                const auto iconKey = button.icon().cacheKey();
                const auto geometry = button.geometry();
                for (const auto mode :
                     { ButtonPaintMode::Native, ButtonPaintMode::OutlinedAction,
                       ButtonPaintMode::ToolbarSquare, ButtonPaintMode::ToolbarCell }) {
                    button.setPaintMode(mode);
                    app.processEvents();
                    require(button.paintMode() == mode && button.icon().cacheKey() == iconKey
                                    && button.geometry() == geometry,
                            "Button paint mode changes its icon asset or geometry");
                    const int paints = button.paints;
                    button.setPaintMode(mode);
                    button.setIconConfiguration({ ControlIcon::Close, IconRendition::Action });
                    refreshButtonIcon(&button);
                    app.processEvents();
                    require(button.paints == paints && button.icon().cacheKey() == iconKey,
                            "Unchanged button configuration regenerates or repaints its icon");
                    require(clicks == 0 && toggles == 0,
                            "Programmatic button configuration emits user intent");
                }
                for (const bool enabled : { true, false })
                    for (const bool checked : { false, true })
                        for (const bool down : { false, true }) {
                            button.setEnabled(enabled);
                            button.setChecked(checked);
                            button.setDown(down);
                            const bool active = enabled && (checked || down);
                            button.setPaintMode(ButtonPaintMode::ToolbarSquare);
                            const auto square = button.grab().toImage();
                            button.setPaintMode(ButtonPaintMode::ToolbarCell);
                            const auto cell = button.grab().toImage();
                            const QPoint outer(px(2), button.height() / 2);
                            require(QColor::fromRgba(square.pixel(outer)) == paper
                                            && QColor::fromRgba(cell.pixel(outer))
                                                    == (active ? ink : paper),
                                    "Toolbar square and full-cell feedback have incorrect bounds");
                            require(active ? square != cell : square == cell,
                                    "Toolbar placement changes idle or disabled pixels, or loses active full-cell feedback");
                            button.setPaintMode(ButtonPaintMode::OutlinedAction);
                            const auto outlined = button.grab().toImage();
                            const QPoint interior(px(8), px(8));
                            require(QColor::fromRgba(outlined.pixel(interior))
                                            == (active ? ink : paper),
                                    "Outlined action background does not follow enabled, checked and pressed state");
                        }
                button.setEnabled(true);
                button.setChecked(false);
                button.setDown(false);
                button.setPaintMode(ButtonPaintMode::ToolbarCell);
                buttonIcon(&button, ControlIcon::Close);
                require(button.paintMode() == ButtonPaintMode::ToolbarCell
                                && button.iconConfiguration()->rendition
                                        == IconRendition::OriginalAsset,
                        "Original icon configuration changes the button painter");
                const auto originalClose =
                        button.icon()
                                .pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas)
                                .toImage();
                button.setIconConfiguration({ ControlIcon::Close, IconRendition::Action });
                require(button.paintMode() == ButtonPaintMode::ToolbarCell
                                && button.icon().pixmap(metrics::control::iconCanvas,
                                                        metrics::control::iconCanvas)
                                                .toImage()
                                        != originalClose,
                        "Action rendition does not change the asset independently of the painter");
                button.setPaintMode(ButtonPaintMode::Native);
                button.setIconConfiguration(
                        { ControlIcon::Portrait, IconRendition::OriginalAsset, false });
                const auto plainRotation =
                        button.icon()
                                .pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas)
                                .toImage();
                button.setIconConfiguration(
                        { ControlIcon::Portrait, IconRendition::OriginalAsset, true });
                const auto sideRotation =
                        button.icon()
                                .pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas)
                                .toImage();
                require(plainRotation != sideRotation,
                        "Original orientation icon ignores physical side buttons");
                button.setIconConfiguration(
                        { ControlIcon::Portrait, IconRendition::Action, false });
                const auto actionRotation =
                        button.icon()
                                .pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas)
                                .toImage();
                button.setIconConfiguration({ ControlIcon::Portrait, IconRendition::Action, true });
                require(button.icon().pixmap(metrics::control::iconCanvas,
                                             metrics::control::iconCanvas)
                                        .toImage()
                                == actionRotation,
                        "Action cropping changes its accepted standard orientation artwork");
                button.setIconConfiguration({ ControlIcon::Close });
                const auto beforeReplay =
                        button.icon()
                                .pixmap(metrics::control::iconCanvas, metrics::control::iconCanvas)
                                .toImage();
                host.setProperty("reader_dark", !dark);
                refreshButtonIcon(&button);
                require(button.icon().pixmap(metrics::control::iconCanvas,
                                             metrics::control::iconCanvas)
                                        .toImage()
                                != beforeReplay,
                        "Theme replay retains the previous icon ink");
                host.setProperty("reader_dark", dark);
                refreshButtonIcon(&button);
                require(button.icon().pixmap(metrics::control::iconCanvas,
                                             metrics::control::iconCanvas)
                                        .toImage()
                                == beforeReplay,
                        "Returning to a theme changes original icon geometry");
            }
        setDensity(savedDensity);
    }
    panel.setProperty("reader_dark", true);
    kobalt::ui::readerStyle(&action, "color:palette(window-text);background:palette(window);");
    require(action.styleSheet().contains("color:#ffffff;background:#000000"),
            "Dark component colors are wrong");
    {
        QWidget nativeWindow;
        nativeWindow.setProperty("reader_dark", false);
        QWidget reader(&nativeWindow);
        reader.setProperty("reader_dark", true);
        kobalt::ui::Button nested("Read", &reader);
        nested.resize(180, 80);
        kobalt::ui::readerStyle(&nested, "color:palette(window-text);background:palette(window);");
        require(nested.styleSheet().contains("color:#ffffff;background:#000000"),
                "Embedded reader uses the native window's light theme");
        require(kobalt::ui::noteColor(&nested) == kobalt::ui::themeColors(true).secondaryInk,
                "Embedded reader caption uses light-theme ink");
        const auto image = nested.grab().toImage();
        require(qGray(image.pixel(4, 4)) == 0,
                "Embedded reader button has white paper in dark mode");
        require(!nativeWindow.property("reader_dark").toBool(),
                "Reader changed its host window's theme");
    }
    require(app.palette() == originalPalette, "Component changed the native application's palette");
    PaintCountingButton button("Open panel", &panel);
    button.setGeometry(0, 0, 150, 60);
    button.show();
    app.processEvents();
    int paintsAtClick = -1;
    QObject::connect(&button, &QPushButton::clicked, &panel, [&] {
        paintsAtClick = button.paints;
        button.setText("Done");
    });
    auto sendMouse = [&](QEvent::Type type, QWidget *target) {
        const QPointF local(30, 30), global = target->mapToGlobal(local.toPoint());
        QMouseEvent event(type, local, global, Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };
    sendMouse(QEvent::MouseButtonPress, &button);
    app.processEvents();
    button.paints = 0;
    sendMouse(QEvent::MouseButtonRelease, &button);
    require(paintsAtClick == 0, "Button repainted released state before its click handler");
    app.processEvents();
    require(button.paints > 0, "Button lost its final repaint");
    for (auto indicator : { kobalt::ui::DescribedOption::Indicator::Toggle,
                            kobalt::ui::DescribedOption::Indicator::Square,
                            kobalt::ui::DescribedOption::Indicator::Radio }) {
        PaintCountingOption option("Refresh", "", &panel);
        auto presentation = option.presentation();
        presentation.indicator = indicator;
        option.setPresentation(presentation);
        option.setGeometry(0, 0, 260, 120);
        option.show();
        app.processEvents();
        int clicks = 0, paintsAtOptionClick = -1;
        QObject::connect(&option, &QCheckBox::clicked, &panel, [&] {
            ++clicks;
            paintsAtOptionClick = option.paints;
        });
        sendMouse(QEvent::MouseButtonPress, &option);
        app.processEvents();
        option.paints = 0;
        sendMouse(QEvent::MouseButtonRelease, &option);
        require(clicks == 1 && option.isChecked() && paintsAtOptionClick == 0,
                "Option paints synchronously before its click handler");
        app.processEvents();
        require(option.paints > 0, "Option loses its selected-state repaint");
        option.hide();
        PaintCountingPanel optionHost;
        optionHost.resize(300, 150);
        optionHost.show();
        auto *closingOption = new PaintCountingOption("Refresh", "", &optionHost);
        closingOption->setGeometry(10, 10, 260, 120);
        closingOption->show();
        QPointer<PaintCountingOption> optionGuard(closingOption);
        QObject::connect(closingOption, &QCheckBox::clicked, &optionHost,
                         [closingOption] { delete closingOption; });
        app.processEvents();
        sendMouse(QEvent::MouseButtonPress, closingOption);
        app.processEvents();
        optionHost.painted = QRegion();
        sendMouse(QEvent::MouseButtonRelease, closingOption);
        app.processEvents();
        require(!optionGuard && optionHost.painted.contains(QRect(10, 10, 260, 120)),
                "Closing option leaves its pressed background behind");
    }
    PaintCountingPanel destination;
    destination.resize(300, 150);
    destination.show();
    kobalt::ui::Button row("Read license", &destination);
    row.setGeometry(10, 10, 260, 80);
    row.show();
    QObject::connect(&row, &QPushButton::clicked, &destination, [&] { row.hide(); });
    app.processEvents();
    sendMouse(QEvent::MouseButtonPress, &row);
    app.processEvents();
    destination.painted = QRegion();
    sendMouse(QEvent::MouseButtonRelease, &row);
    app.processEvents();
    require(destination.painted.contains(row.geometry()),
            "Hidden pressed row leaves its background behind");
    destination.hide();
    auto *closing = new PaintCountingButton("Close", &panel);
    closing->setGeometry(0, 0, 150, 60);
    closing->show();
    QPointer<PaintCountingButton> guard(closing);
    QObject::connect(closing, &QPushButton::clicked, &panel, [closing] { delete closing; });
    sendMouse(QEvent::MouseButtonPress, closing);
    sendMouse(QEvent::MouseButtonRelease, closing);
    require(!guard, "Button click did not delete its target");
    for (auto presentation : { kobalt::ui::CaptionButton::Presentation::Caption,
                               kobalt::ui::CaptionButton::Presentation::ListAction }) {
        using namespace kobalt::ui;
        PaintCountingCaptionButton action(presentation, &panel);
        action.setText("Action");
        action.setGeometry(0, 0, 260, 120);
        action.show();
        app.processEvents();
        int clicks = 0, paintsAtAction = -1;
        QObject::connect(&action, &QPushButton::clicked, &panel, [&] {
            ++clicks;
            paintsAtAction = action.paints;
        });
        action.setCaption("Page 4");
        action.setRule(Qt::BottomEdge);
        action.setChevron(false);
        action.setIconVisible(false);
        app.processEvents();
        require(clicks == 0 && action.caption() == "Page 4"
                        && action.presentation() == presentation,
                "Action presentation setters emit user intent or lose their value");
        action.paints = 0;
        action.setCaption("Page 4");
        action.setRule(Qt::BottomEdge);
        action.setChevron(false);
        action.setIconVisible(false);
        app.processEvents();
        require(action.paints == 0, "Unchanged action presentation schedules another paint");
        sendMouse(QEvent::MouseButtonPress, &action);
        app.processEvents();
        action.paints = 0;
        sendMouse(QEvent::MouseButtonRelease, &action);
        require(clicks == 1 && paintsAtAction == 0, "Action bypasses release paint batching");
        app.processEvents();
        require(action.paints > 0, "Action loses its released repaint");
        sendMouse(QEvent::MouseButtonPress, &action);
        QMouseEvent move(QEvent::MouseMove, QPointF(-10, -10), action.mapToGlobal(QPoint(-10, -10)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&action, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(-10, -10),
                            action.mapToGlobal(QPoint(-10, -10)), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(&action, &release);
        require(clicks == 1 && !action.isDown(),
                "Dragging outside an action still clicks or stays pressed");
        sendMouse(QEvent::MouseButtonPress, &action);
        action.setEnabled(false);
        sendMouse(QEvent::MouseButtonRelease, &action);
        require(clicks == 1 && !action.isDown(), "Disabled action still clicks or stays pressed");
        action.hide();
        PaintCountingPanel actionHost;
        actionHost.resize(300, 150);
        actionHost.show();
        auto *hidden = new CaptionButton(presentation, &actionHost);
        hidden->setGeometry(10, 10, 260, 120);
        hidden->show();
        QObject::connect(hidden, &QPushButton::clicked, &actionHost, [hidden] { hidden->hide(); });
        app.processEvents();
        sendMouse(QEvent::MouseButtonPress, hidden);
        app.processEvents();
        actionHost.painted = QRegion();
        sendMouse(QEvent::MouseButtonRelease, hidden);
        app.processEvents();
        require(actionHost.painted.contains(hidden->geometry()),
                "Hidden action leaves its pressed background behind");
        auto *removed = new CaptionButton(presentation, &actionHost);
        removed->setGeometry(10, 10, 260, 120);
        removed->show();
        QPointer<CaptionButton> retained(removed);
        QObject::connect(removed, &QPushButton::clicked, &actionHost,
                         [removed] { delete removed; });
        sendMouse(QEvent::MouseButtonPress, removed);
        sendMouse(QEvent::MouseButtonRelease, removed);
        require(!retained, "Action cannot be deleted during its click callback");
    }
    {
        using namespace kobalt::ui;
        QWidget actionHost;
        auto *actions = new QVBoxLayout(&actionHost);
        int clicks = 0;
        auto *action = makeCaptionButton(&actionHost, actions, "typedAction", "Continue",
                                         [&] { ++clicks; });
        require(action->parentWidget() == &actionHost && actions->indexOf(action) == 0
                        && action->focusPolicy() == Qt::NoFocus
                        && action->objectName() == "typedAction"
                        && action->presentation() == CaptionButton::Presentation::ListAction,
                "Action factory loses its layout, ownership, identity or focus policy");
        action->click();
        require(clicks == 1, "Action factory does not connect its callback");
    }
    {
        // A list action with a note centres both lines on their letters: from
        // the label's capitals to the note's baseline, as Restore defaults shows.
        using namespace kobalt::ui;
        QWidget actionHost;
        actionHost.setStyleSheet("background:white;");
        auto *actions = new QVBoxLayout(&actionHost);
        auto *action = makeCaptionButton(&actionHost, actions, "notedAction",
                                         "Restore appearance defaults", [] { });
        action->setCaption("ASKS FIRST");
        action->setChevron(true);
        action->setIconVisible(false);
        action->setFont(interfaceFont(metrics::text::secondary));
        action->setFixedSize(900, metrics::about::actionHeight);
        actionHost.show();
        app.processEvents();
        const auto image = action->grab().toImage();
        const int textRight = image.width() - int(metrics::chrome::actionRight);
        QVector<int> inkRows;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < textRight; ++x)
                if (qGray(image.pixel(x, y)) < 128) {
                    inkRows.append(y);
                    break;
                }
        require(!inkRows.isEmpty(), "List action drew no text");
        // The note is the last group of rows; its baseline is the commonest column bottom.
        int noteTop = inkRows.last();
        for (int i = inkRows.size() - 1; i > 0 && inkRows[i] - inkRows[i - 1] <= 2; --i)
            noteTop = inkRows[i - 1];
        QHash<int, int> bottoms;
        for (int x = 0; x < textRight; ++x)
            for (int y = image.height() - 1; y >= noteTop; --y)
                if (qGray(image.pixel(x, y)) < 128) {
                    ++bottoms[y];
                    break;
                }
        int baseline = noteTop, most = 0;
        for (auto it = bottoms.cbegin(); it != bottoms.cend(); ++it)
            if (it.value() > most) {
                most = it.value();
                baseline = it.key();
            }
        const qreal offset = (inkRows.first() + baseline) / 2.0 - (image.height() - 1) / 2.0;
        require(qAbs(offset) <= 1.5, "List action text is not centred on its letters");
    }
    {
        using namespace kobalt::ui;
        QWidget owner;
        auto *row = makeCaptionButton(&owner, nullptr, "typefacePreview", "Typeface", [] { });
        row->setCaption("SELECTED FACE");
        row->setFixedSize(900, metrics::about::actionHeight);
        owner.show();
        row->show();
        for (bool dark : { false, true }) {
            QPalette colors = originalPalette;
            colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
            colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
            row->setPalette(colors);
            row->setTypefacePreview({});
            app.processEvents();
            const auto plain = row->grab().toImage();
            const QRect preview(row->width() - metrics::chrome::actionRight - px(88), 0, px(88),
                                row->height());
            const auto empty = plain.copy(preview);
            row->setTypefacePreview(interfaceFont().family());
            const auto sans = row->grab().toImage();
            require(sans.copy(preview) != empty, "Typeface row did not draw its Aa preview");
            row->setTypefacePreview(chromeFont().family());
            const auto mono = row->grab().toImage();
            require(mono.copy(preview) != sans.copy(preview),
                    "Typeface row reused the previous face");
            require(mono.copy(0, 0, row->width() - metrics::chrome::actionRight - px(108),
                              row->height())
                            == plain.copy(0, 0,
                                          row->width() - metrics::chrome::actionRight - px(108),
                                          row->height()),
                    "Typeface preview moved the row label or caption");
            row->setTypefacePreview({});
            require(row->grab().toImage() == plain,
                    "Clearing a typeface preview left sample pixels behind");
        }
    }
    {
        using namespace kobalt::ui;
        PageScrubber scrubber(nullptr);
        scrubber.resize(1000, 700);
        int imageOpens = 0, imageDismissals = 0, returns = 0, stays = 0;
        scrubber.imageRequested = [&] { ++imageOpens; };
        scrubber.imageDismissed = [&] { ++imageDismissals; };
        scrubber.returnRequested = [&] { ++returns; };
        scrubber.stayRequested = [&] { ++stays; };
        scrubber.setImageAction("Zoom image");
        auto *imageAction = scrubber.findChild<QPushButton *>("rrScrubberImage");
        require(imageAction && !imageAction->isHidden(), "Image suggestion is missing");
        imageAction->click();
        scrubber.findChild<QPushButton *>("rrScrubberImageDismiss")->click();
        require(imageOpens == 1 && imageDismissals == 1 && !returns && !stays,
                "Image suggestion dispatched return actions");
        scrubber.setReturnAction("Return to page 8", "Chapter");
        auto *returnAction = scrubber.findChild<QPushButton *>("rrScrubberReturn");
        require(returnAction && returnAction->text() == "Return to page 8",
                "Image suggestion displaced return action");
        require(!imageAction->isHidden() && !returnAction->isHidden(),
                "Roomy layout did not show both actions");
        require(imageAction->geometry().right() < returnAction->geometry().left(),
                "Image and return actions overlap");
        imageAction->click();
        scrubber.findChild<QPushButton *>("rrScrubberImageDismiss")->click();

        returnAction->click();
        scrubber.findChild<QPushButton *>("rrScrubberStay")->click();
        require(returns == 1 && stays == 1 && imageOpens == 2 && imageDismissals == 2,
                "Return action dispatched image actions");
        scrubber.show();
        scrubber.resize(400, 700);
        app.processEvents();
        require(imageAction->isHidden() && !returnAction->isHidden(),
                "Narrow layout hid Return or squeezed both actions");
        scrubber.resize(1000, 700);
        app.processEvents();
        require(!imageAction->isHidden() && !returnAction->isHidden(),
                "Widening did not restore both actions");
        // Every shown action keeps its full label; resizing across the threshold is reversible.
        for (int width = 400; width <= 1100; width += 10) {
            scrubber.resize(width, 700);
            app.processEvents();
            if (!imageAction->isHidden()) {
                require(imageAction->width() >= imageAction->sizeHint().width(),
                        "Image label was squeezed");
                require(returnAction->width() >= returnAction->sizeHint().width(),
                        "Return label was squeezed");
                const auto *dismiss = scrubber.findChild<QPushButton *>("rrScrubberImageDismiss");
                require(dismiss->geometry().right() < returnAction->x(),
                        "Image close overlaps Return");
            }
        }
        scrubber.setReturnAction({}, {});
        require(!imageAction->isHidden(), "Clearing return action lost image suggestion");
        scrubber.setImageAction({});
        require(!scrubber.hasActions(), "Dismissed actions left a floating button");
        QVector<QPair<int, QString>> chapters{
            { 0, "Start" }, { 200, "Middle" }, { 900, "Later" }, { 1500, "End" }
        };
        const auto checkMarkers = [&] {
            auto *actual = scrubber.slider();
            SettingSlider expected(nullptr, actual->minimum(), actual->maximum());
            expected.setPalette(actual->palette());
            require(actual->isEnabled() == (actual->maximum() > actual->minimum()),
                    "Page slider enablement does not match its page range");
            expected.setEnabled(actual->isEnabled());
            expected.resize(actual->size());
            expected.setDotMarkers(true);
            expected.setValue(actual->value());
            QVector<int> markers;
            for (const auto &chapter : chapters)
                markers.append(chapter.first);
            expected.setMarkers(markers);
            require(actual->grab().toImage() == expected.grab().toImage(),
                    "Scrubber chapter markers changed after a page, chapter or range update");
        };
        scrubber.setBook(0, 2000, chapters, { 1072, 1448 });
        checkMarkers();
        scrubber.setBook(20, 2000, chapters, { 1072, 1448 });
        checkMarkers();
        scrubber.setBook(1600, 2000, chapters, { 1072, 1448 });
        checkMarkers();
        scrubber.findChild<QPushButton *>("rrScrubberZoomIn")->click();
        checkMarkers();
        scrubber.findChild<QPushButton *>("rrScrubberZoomOut")->click();
        checkMarkers();
        chapters = { { 1300, "Replacement" }, { 1700, "Last" } };
        scrubber.setBook(1600, 2000, chapters, { 1072, 1448 });
        checkMarkers();
        chapters.clear();
        scrubber.setBook(1600, 2000, chapters, { 1072, 1448 });
        checkMarkers();
        chapters = { { 0, "Only" } };
        scrubber.setBook(0, 1, chapters, { 1072, 1448 });
        checkMarkers();
    }
    kobalt::ui::SettingSlider slider(&panel, 0, 100);
    slider.resize(300, 72);
    slider.show();
    slider.setValue(20);
    slider.setMarkers({ -1, 0, 50, 50, 100, 101 });
    for (const bool dark : { false, true }) {
        QPalette sliderPalette = originalPalette;
        sliderPalette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        sliderPalette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        slider.setPalette(sliderPalette);
        app.processEvents();
        const auto sliderImage = slider.grab().toImage();
        for (const int x : { int(kobalt::ui::metrics::control::sliderInset), 150,
                             slider.width() - kobalt::ui::metrics::control::sliderInset })
            require(qGray(sliderImage.pixel(
                            x,
                            kobalt::ui::metrics::control::sliderMarkerTop
                                    + kobalt::ui::metrics::control::sliderMarkerHeight / 2))
                            == (dark ? 255 : 0),
                    "Slider did not draw a bounded marker above its track");
        require(qGray(sliderImage.pixel(100,
                                        kobalt::ui::metrics::control::sliderMarkerTop
                                                + kobalt::ui::metrics::control::sliderMarkerHeight
                                                        / 2))
                        == (dark ? 0 : 255),
                "Slider drew an out-of-range marker");
    }
    int commits = 0, lastValue = -1;
    slider.committed = [&](int value) {
        ++commits;
        lastValue = value;
    };
    auto slide = [&](QEvent::Type type, int x) {
        const QPointF local(x, 36), global = slider.mapToGlobal(local.toPoint());
        QMouseEvent event(
                type, local, global, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&slider, &event);
    };
    slide(QEvent::MouseButtonPress, 25);
    slide(QEvent::MouseMove, 200);
    require(commits == 0 && slider.value() == 70, "Slider applied its draft before release");
    slide(QEvent::MouseButtonRelease, 275);
    require(commits == 1 && lastValue == 100, "Slider did not apply its released target once");
    slide(QEvent::MouseButtonPress, 25);
    slider.hide();
    slider.show();
    slide(QEvent::MouseButtonRelease, 25);
    require(commits == 1 && slider.value() == 100, "Hidden slider applied an abandoned gesture");
    slide(QEvent::MouseButtonPress, 25);
    QEvent cancel(QEvent::TouchCancel);
    QApplication::sendEvent(&slider, &cancel);
    slide(QEvent::MouseButtonRelease, 25);
    require(commits == 1 && slider.value() == 100, "Cancelled slider applied an abandoned gesture");
    slider.setRange(0, 0);
    slide(QEvent::MouseButtonPress, 25);
    slide(QEvent::MouseButtonRelease, 275);
    require(commits == 1 && slider.value() == 0, "Single-page slider emitted a jump");
    slider.setRange(23, 48);
    slider.setMarkers({ 23, 24, 26, 28, 48 }, kobalt::ui::SettingSlider::MarkerMode::Checkpoints);
    slider.resize(450, 72);
    for (const auto point : { QPoint(25, 23), QPoint(125, 24), QPoint(225, 26), QPoint(325, 28),
                              QPoint(375, 38), QPoint(425, 48) }) {
        slide(QEvent::MouseButtonPress, point.x());
        slide(QEvent::MouseButtonRelease, point.x());
        require(slider.value() == point.y(),
                "Checkpoint drag did not interpolate sizes beyond Large");
    }
    slider.nudge(-1);
    require(slider.value() == 47, "Checkpoint minus skipped a fine size");
    slider.setValue(24);
    slider.nudge(-1);
    require(slider.value() == 23, "Minus from Small did not reach Off");
    slider.nudge(1);
    require(slider.value() == 24, "Plus from Off did not enable Small");
    slider.setRange(100, 300);
    const QVector<int> spacingStops{ 100, 110, 120, 130, 140, 150, 175, 200, 250, 300 };
    slider.setMarkers(spacingStops, kobalt::ui::SettingSlider::MarkerMode::Stops);
    const int inset = kobalt::ui::metrics::control::sliderInset;
    slider.resize(360 + 2 * inset, 72);
    for (int index = 0; index < spacingStops.size(); ++index) {
        const int x = inset + 40 * index;
        slide(QEvent::MouseButtonPress, x + (index == spacingStops.size() - 1 ? -12 : 12));
        require(slider.value() == spacingStops[index],
                "Pointer did not snap to equally spaced line-height stops");
        slide(QEvent::MouseButtonRelease, x);
        slider.nudge(1);
        require(slider.value() == qMin(300, spacingStops[index] + 1),
                "Plus skipped a granular line-height value");
        slider.nudge(-1);
        require(slider.value() == (index == spacingStops.size() - 1 ? 299 : spacingStops[index]),
                "Minus snapped instead of changing line height by one percent");
    }
    slider.setRange(14, 96);
    slider.setSingleStep(2);
    const QVector<int> sizeStops{ 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 48, 56, 64, 80, 96 };
    slider.setMarkers(sizeStops, kobalt::ui::SettingSlider::MarkerMode::Stops);
    slider.resize(560 + 2 * inset, 72);
    for (int index = 0; index < sizeStops.size(); ++index) {
        const int x = inset + 40 * index;
        slide(QEvent::MouseButtonPress, x);
        slide(QEvent::MouseButtonRelease, x);
        require(slider.value() == sizeStops[index], "Font size did not snap to its nonlinear stop");
    }
    slider.nudge(-1);
    require(slider.value() == 94, "Minus did not change 48 pt to 47 pt");
    slider.nudge(-1);
    require(slider.value() == 92, "Minus snapped instead of changing to 46 pt");
    slider.setRange(100, 300);
    slider.setSingleStep(1);
    slider.setMarkers(spacingStops, kobalt::ui::SettingSlider::MarkerMode::Stops);
    slider.resize(360 + 2 * inset, 72);
    for (const bool dark : { false, true }) {
        QPalette colors = originalPalette;
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
        slider.setPalette(colors);
        slider.setValue(151);
        const auto image = slider.grab().toImage();
        const int markerY = kobalt::ui::metrics::control::sliderMarkerTop
                + kobalt::ui::metrics::control::sliderMarkerHeight / 2;
        for (int index = 0; index < spacingStops.size(); ++index) {
            require(qGray(image.pixel(inset + 40 * index, markerY)) == (dark ? 255 : 0),
                    "Nonlinear slider marks were not equally spaced");
        }
        require(qGray(image.pixel(inset + 20, markerY)) == (dark ? 0 : 255),
                "Nonlinear slider drew a marker between its stops");
    }
    slider.setValue(151);
    const int beforeSnap = commits;
    slide(QEvent::MouseButtonPress, inset + 240);
    QApplication::sendEvent(&slider, &cancel);
    slide(QEvent::MouseButtonRelease, inset + 240);
    require(slider.value() == 151 && commits == beforeSnap,
            "Cancelling a snapped gesture lost the granular value");
    kobalt::ui::FontButton font(&panel);
    font.ensurePolished();
    font.resize(500, kobalt::ui::metrics::choices::fontButtonHeight);
    font.setFace("Sans");
    kobalt::ui::Panel dropdown(&panel,
                               kobalt::ui::Edges(Qt::LeftEdge) | Qt::RightEdge | Qt::BottomEdge);
    dropdown.resize(500, 200);
    kobalt::ui::PageNavigation dropdownNavigation(&dropdown, "dropdown");
    dropdownNavigation.layout(dropdown.size(), true);
    dropdownNavigation.setPage(0, 2);
    for (const bool dark : { false, true }) {
        QPalette colors = originalPalette;
        const QColor ink = dark ? Qt::white : Qt::black;
        colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
        colors.setColor(QPalette::WindowText, ink);
        font.setPalette(colors);
        dropdown.setPalette(colors);
        QImage selector(font.size(), QImage::Format_RGB32),
                picker(dropdown.size(), QImage::Format_RGB32);
        selector.fill(Qt::red);
        picker.fill(Qt::red);
        font.render(&selector);
        dropdown.render(&picker);
        for (int y = 0; y < selector.height(); ++y)
            require(QColor::fromRgb(selector.pixel(0, y)) == ink
                            && QColor::fromRgb(selector.pixel(499, y)) == ink,
                    "Font selector has a broken border");
        for (int y = 0; y < picker.height(); ++y)
            require(QColor::fromRgb(picker.pixel(0, y)) == ink
                            && QColor::fromRgb(picker.pixel(499, y)) == ink,
                    "Font dropdown has a broken border");
        require(QColor::fromRgb(selector.pixel(250, selector.height() - 1)) == ink
                        && QColor::fromRgb(picker.pixel(250, 0)) == colors.color(QPalette::Window),
                "Font selector and dropdown doubled their shared border");
    }
    for (const int ppi : { 212, 227, 300 }) {
        kobalt::ui::setDensity(ppi);
        QTextDocument excerpt;
        excerpt.setHtml(QString("<div style=\"line-height:%1%\">First<br>Second</div>")
                                .arg(kobalt::ui::metrics::search::excerptLineHeight));
        const auto format = excerpt.begin().blockFormat();
        require(format.lineHeightType() == QTextBlockFormat::ProportionalHeight
                        && format.lineHeight() == 130,
                "Search excerpt line spacing changed with screen density");
    }
    for (const bool dark : { false, true }) {
        using namespace kobalt::ui;
        setDensity(designPpi);
        QWidget host;
        host.setProperty("reader_dark", dark);
        host.resize(1000, 1400);
        QPalette colors = originalPalette;
        const QColor paper = dark ? Qt::black : Qt::white, ink = dark ? Qt::white : Qt::black;
        colors.setColor(QPalette::Window, paper);
        colors.setColor(QPalette::WindowText, ink);
        host.setPalette(colors);
        readerStyle(&host, styles::reader());
        host.show();
        {
            // An action menu: header parts are optional, and a choice closes the menu first.
            QStringList calls;
            QImage cover(60, 90, QImage::Format_RGB32);
            cover.fill(Qt::darkBlue);
            ActionMenu menu(&host, { cover, "A book", "An author", "42% · 1 MB" },
                            { actionItem("Open", "open", ControlIcon::Dictionary,
                                         [&calls] { calls.append("open"); }),
                              { "Without icon", "plain", false, ControlIcon::More, {} } });
            menu.dismissed = [&calls] { calls.append("dismissed"); };
            menu.show();
            app.processEvents();
            require(menu.actions().size() == 2 && menu.action("open") && menu.action("plain")
                            && !menu.action("x"),
                    "Action menu rows are missing");
            auto *header = menu.findChild<QWidget *>("rrActionMenuHeader");
            require(header && header->accessibleName() == "A book An author 42% · 1 MB",
                    "Action menu header is incomplete");
            // The header is as tall as its thumbnail plus margins, without spare space.
            const auto margins = QMargins(metrics::notice::cardMargins);
            require(header->height()
                            == margins.top() + metrics::actionMenu::thumbnailHeight
                                    + margins.bottom(),
                    "Action menu header has spare height");
            require(menu.action("open")->width() >= menu.card()->width() - 4,
                    "Action menu rows do not span the card");
            menu.action("open")->click();
            require(calls == QStringList({ "dismissed", "open" }),
                    "Choosing an action did not close the menu first");
            ActionMenu plain(&host, {}, { { "Only", "only", false, ControlIcon::More, {} } });
            require(!plain.findChild<QWidget *>("rrActionMenuHeader"), "An empty header was shown");
        }
        {
            // A row measured before polishing used the application font, not
            // its style sheet font. On Kobo, Nickel's application font made
            // option rows too short and clipped the card around them.
            // The bundled monospace font wraps differently from the application font.
            const auto styled = QFontInfo(kobalt::ui::chromeFont()).family();
            require(styled != QApplication::font().family(),
                    "Option font check needs a font other than the application font");
            QWidget parent;
            parent.setStyleSheet(QString("*{font-family:\"%1\";}").arg(styled));
            const QString description =
                    "A description that is long enough to wrap onto several lines.";
            auto *reference = new DescribedOption("Option", description, &parent);
            reference->ensurePolished();
            require(reference->font().family() == styled,
                    "Option did not take its font from the style sheet");
            auto *early = new DescribedOption("Option", description, &parent);
            require(early->heightForWidth(320) == reference->heightForWidth(320),
                    "Option was measured with the application font before polishing");
        }
        ChoiceModal choice(&host, { "A required description", "A title", {} },
                           { "Cancel", "cancel" }, { "Continue", "continue" });
        choice.card()->fitToWidth(760);
        choice.show();
        app.processEvents();
        require(choice.primaryButton()->y() == choice.secondaryButton()->y(),
                "Short modal actions were not side by side");
        require(choice.secondaryButton()->x() < choice.primaryButton()->x(),
                "Secondary modal action was not on the left");
        require(choice.primaryButton()->property("reader_primary_action").toBool()
                        && !choice.secondaryButton()->property("reader_primary_action").toBool(),
                "Modal action appearances were reversed");
        require(!styles::primary().contains("#bb3333") && !styles::outlined().contains("#bb3333")
                        && !styles::textAction().contains("#bb3333"),
                "Dialog styles contain a destructive colour");
        for (auto *button : { choice.primaryButton(), choice.secondaryButton() }) {
            const auto fill = paper;
            const auto hasLabel = [](const QImage &image, const QColor &background) {
                int inkPixels = 0;
                for (int y = 10; y < image.height() - 10; ++y)
                    for (int x = 10; x < image.width() - 10; ++x)
                        if (QColor::fromRgba(image.pixel(x, y)) != background)
                            ++inkPixels;
                return inkPixels > 20;
            };
            const auto normal = button->grab().toImage();
            require(QColor::fromRgba(normal.pixel(5, 5)) == fill,
                    "Modal action has the wrong background");
            require(hasLabel(normal, fill), "Modal action label disappeared into its background");
            button->setDown(true);
            app.processEvents();
            require(QColor::fromRgba(button->grab().toImage().pixel(5, 5))
                            == QColor(dark ? "#333333" : "#dddddd"),
                    "Pressed modal action has the wrong background");
            require(hasLabel(button->grab().toImage(), QColor(dark ? "#333333" : "#dddddd")),
                    "Pressed modal action label disappeared");
            button->setDown(false);
            button->setEnabled(false);
            app.processEvents();
            require(hasLabel(button->grab().toImage(), paper),
                    "Disabled modal action label disappeared");
            button->setEnabled(true);
        }
        // Async descriptions must resize nested headings after the initial short message.
        choice.descriptionLabel()->setText(
                "On this device · 12.3%\nLocal chapter\n\nSynced position · 45.6%\nRemote "
                "chapter\nDevice: Test reader\nSynced: 8 Nov 2026 09:00\n\nThe synced position "
                "is ahead.");
        choice.fitCard();
        app.processEvents();
        auto *body = choice.descriptionLabel();
        require(body->height() >= body->heightForWidth(body->width()),
                "Updated modal description is clipped inside its heading");
        const int expandedHeight = choice.card()->height();
        body->setText("A short result");
        choice.fitCard();
        app.processEvents();
        require(choice.card()->height() < expandedHeight
                        && body->height() >= body->heightForWidth(body->width()),
                "Updated modal kept the previous description's height");
        choice.primaryButton()->setText("Use this font as my default for all books");
        choice.card()->fitToWidth(400);
        app.processEvents();
        require(choice.primaryButton()->y() < choice.secondaryButton()->y(),
                "Long modal actions were not stacked");
        require(choice.primaryButton()->height() >= choice.secondaryButton()->height(),
                "Long modal action lost its wrapping space");
        require(choice.card()->rect().contains(choice.primaryButton()->geometry()),
                "Modal action escaped its card");
        choice.primaryButton()->setText("Yes");
        choice.card()->fitToWidth(760);
        app.processEvents();
        require(choice.primaryButton()->y() == choice.secondaryButton()->y(),
                "Modal actions did not return to one row");
        {
            PopupCard resizing(&host);
            auto *body = resizing.message(
                    "This description wraps at a narrow width and must shrink when the card grows wider.",
                    "resizingDescription");
            resizing.fitToWidth(400);
            const int narrowHeight = body->height();
            resizing.fitToWidth(760);
            require(body->height() < narrowHeight,
                    "Modal description kept its narrow wrapping after widening");
            readerStyle(body, styles::font(48));
            resizing.fitToWidth(760);
            const int largeHeight = body->height();
            readerStyle(body, styles::font(24));
            resizing.fitToWidth(760);
            require(body->height() < largeHeight,
                    "Modal description kept its previous font height");
        }
        QImage image(320, 120, QImage::Format_RGB32);
        image.fill(Qt::green);
        NoticeModal notice(&host, { {}, "Title without a body", image }, { "OK", "ok" });
        notice.card()->fitToWidth(400);
        notice.show();
        app.processEvents();
        require(notice.card()->findChildren<QLabel *>().size() == 2,
                "Optional body left an empty label");
        require(!notice.descriptionLabel(), "Notice without a body created a description");
        require(choice.descriptionLabel()->textFormat() == Qt::PlainText,
                "Modal description interpreted markup");
        require(notice.card()->findChildren<QPushButton *>().size() == 1,
                "Notice did not have exactly one action");
        require(!notice.card()->grab().isNull(), "Modal with an image could not be painted");
        host.resize(380, 1000);
        app.processEvents();
        require(choice.card()->width() <= host.width() - 2 * int(metrics::notice::sideClearance),
                "Modal ignored the screen side clearance");
        require(choice.rect().contains(choice.card()->geometry()),
                "Resized modal card escaped its screen");
    }
    {
        // Export destinations include the full path, including long profile paths.
        kobalt::ui::setDensity(kobalt::ui::designPpi);
        kobalt::ui::setLargeText(1.5);
        QWidget landscape;
        landscape.resize(1448, 1072);
        const QString description =
                "Export all annotations from Trevelyan's Type Tester as a compressed HTML file? "
                "Unarchive the ZIP to open the HTML file.\n\nSave to /"
                + QString("a-long-worktree-directory/").repeated(12) + "annotations/book-html.zip.";
        kobalt::ui::ChoiceModal confirmation(&landscape, { description, "Export annotations?", {} },
                                             { "Cancel", "cancel" }, { "Export", "export" });
        confirmation.card()->fitToWidth(760);
        landscape.show();
        confirmation.show();
        app.processEvents();
        require(landscape.rect().contains(confirmation.card()->geometry()),
                "A long export destination pushed the large-text landscape dialog outside the screen");
        kobalt::ui::setLargeText(1);
    }
    kobalt::ui::setDensity(kobalt::ui::designPpi);
    std::puts(
            "PASS: shared navigation, release-only sliders, continuous picker borders, actions and dark styling");
}
