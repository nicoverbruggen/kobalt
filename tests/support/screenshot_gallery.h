#pragma once
#include <QFile>
#include <algorithm>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDebug>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QSet>

namespace kobalt::test {
inline QString screenshotGroup(const QString &scene)
{
    if (scene.startsWith("international-"))
        return "International books";
    if (scene.startsWith("library-") || scene.startsWith("widget-keyboard"))
        return "Library";
    if (scene == "dictionaries" || scene.startsWith("dictionary-"))
        return "Dictionary";
    if (scene.startsWith("links-") || scene.startsWith("footnotes-")
        || scene.startsWith("tappable-links-"))
        return "Links and footnotes";
    if (scene.startsWith("search"))
        return "Search";
    if (scene.startsWith("kosync-"))
        return "Settings and About";
    if (scene.startsWith("beautifier-"))
        return "Typography";
    if (scene.startsWith("page-turn-") || scene.startsWith("settings") || scene == "about"
        || scene == "acknowledgments" || scene.startsWith("license"))
        return "Settings and About";
    if (scene.startsWith("annotation") || scene == "annotating" || scene.startsWith("bookmark")
        || scene.startsWith("pinned-bookmark"))
        return "Bookmarks and annotations";
    if (scene.startsWith("vertical-") || scene == "layout" || scene.startsWith("margin-preview")
        || scene.startsWith("header-footer"))
        return "Layout";
    if (scene.startsWith("fonts") || scene.startsWith("text") || scene.startsWith("bionic")
        || scene.startsWith("embedded-"))
        return "Typography";
    if (scene.startsWith("maths"))
        return "Mathematics";
    if (scene.startsWith("stats"))
        return "Statistics";
    return "Reading";
}

// The gallery viewports, in the order of the desktop preview presets 1 to 5.
inline const QStringList &screenshotDevices()
{
    static const QStringList devices{ "six-inch", "seven-inch", "phone", "six-inch-landscape",
                                      "phone-landscape" };
    return devices;
}

// Every viewport has light and dark captures. The e-reader viewports also have
// light captures with 150% interface text, "large". Phone mode supports 100%
// interface text only.
inline QStringList screenshotThemes(const QString &device)
{
    if (device.startsWith("phone"))
        return { "light", "dark" };
    return { "light", "dark", "large" };
}

// Japanese, Korean and Chinese books also have a vertical-writing tour.
inline bool verticalWritingTour(const QString &language)
{
    return QStringList{ "ja", "ko", "zh-Hans", "zh-Hant" }.contains(language);
}

// The card title of every gallery scene other than the international books.
// A capture of a scene without a title fails, so this map declares the scenes.
inline const QMap<QString, QString> &screenshotTitles()
{
    static const QMap<QString, QString> titles{
        { "about", "About Kobalt" },
        { "viewport-transition", "Updating page layout" },
        { "kosync-login-feedback", "Sync sign in: idle" },
        { "kosync-login-feedback-pressed", "Sync sign in: pressed" },
        { "kosync-login-feedback-released", "Sync sign in: released" },
        { "annotation-color-feedback", "Annotation color: idle" },
        { "annotation-color-feedback-pressed", "Annotation color: pressed" },
        { "annotation-color-feedback-released", "Annotation color: released" },
        { "library-option-feedback", "Library checkbox: idle" },
        { "library-option-feedback-pressed", "Library checkbox: pressed" },
        { "library-option-feedback-released", "Library checkbox: released" },
        { "page-turn-animation", "Android page turn animation setting" },
        { "page-turn-sweep", "Android banded sweep: mid-turn" },
        { "settings-light-dark-mode", "Dark-mode shortcut setting" },
        { "light-dark-mode", "Dark-mode shortcut in Light" },
        { "settings-light-feedback", "Frontlight gesture feedback" },
        { "settings-light-feedback-hidden", "Frontlight gesture feedback disabled" },
        { "light-feedback-hidden", "Frontlight adjustment without feedback" },
        { "brightness-gesture-feedback", "Brightness gesture popup" },
        { "warmth-gesture-feedback", "Warmth gesture popup" },
        { "acknowledgments", "Acknowledgments" },
        { "alice-reading", "Alice in Wonderland" },
        { "annotating", "Annotation mode" },
        { "annotation-actions", "Selection actions" },
        { "annotation-edit", "Edit a highlight" },
        { "annotation-export-confirmation", "Confirm annotation export" },
        { "annotation-export-list", "Annotations ready to export" },
        { "annotation-export-menu", "Export annotations" },
        { "annotation-export-pressed", "Export action pressed" },
        { "annotation-export-saved", "Export complete" },
        { "annotation-saved", "Saved highlight" },
        { "annotation-start", "Start a selection" },
        { "annotations", "Empty annotation list" },
        { "bionic-reading", "Bold reading" },
        { "bionic-setting", "Bold reading setting" },
        { "bookmark-remove", "Remove a bookmark" },
        { "bookmark-page-add-menu", "Bookmark the current page" },
        { "bookmark-page-remove-menu", "Remove the current page bookmark" },
        { "pinned-bookmark-remove", "Remove a pinned bookmark" },
        { "bookmarked", "Bookmarked page" },
        { "bookmarks", "Saved bookmarks" },
        { "pinned-bookmark-editor", "Choose a bookmark name and icon" },
        { "pinned-bookmark-edit", "Modify a pinned bookmark" },
        { "pinned-bookmark-icons-page2", "Bookmark icons: page 2" },
        { "pinned-bookmark-icons-page3", "Bookmark icons: page 3" },
        { "pinned-bookmark-icons-page4", "Bookmark icons: page 4" },
        { "pinned-bookmark-name", "Edit the bookmark name" },
        { "pinned-bookmark-list", "Pinned and ordinary bookmarks" },
        { "pinned-bookmark-single", "Single bookmark shortcut" },
        { "pinned-bookmark-background", "Bookmark shortcut over contrasting content" },
        { "pinned-bookmark-eight", "Eight bookmark shortcuts" },
        { "pinned-bookmark-four", "Four bookmark shortcuts" },
        { "pinned-bookmark-actions", "Bookmark actions" },
        { "pinned-bookmark-limit", "Bookmark shortcut limit" },
        { "chapters", "Chapter list" },
        { "button-controls-settings", "Controls: button mapping entry" },
        { "button-controls-unavailable", "Controls: no supported buttons" },
        { "button-controls", "Button controls: physical buttons" },
        { "button-actions", "Button controls: actions" },
        { "button-controls-none", "Button controls: Do nothing" },
        { "settings-chapter-navigation", "Controls: navigation and links" },
        { "chapter-groups-preference", "Collapsible chapters preference" },
        { "chapters-no-collapse", "Chapters: collapsing disabled" },
        { "chapters-groups", "Chapter groups: current chapter expanded" },
        { "chapters-groups-collapsed", "Chapter groups: collapsed" },
        { "cover", "Book cover" },
        { "image-viewer-entry", "Image page: zoom and return actions" },
        { "image-viewer-fit", "Image viewer" },
        { "image-viewer-zoomed", "Image viewer: zoom and pan" },
        { "dictionary-downloading", "Dictionary download in progress" },
        { "dictionaries", "Manage dictionaries" },
        { "dictionary-choices", "Choose a dictionary" },
        { "dictionary-definition", "Dictionary definition" },
        { "dictionary-definition-page-2", "Dictionary definition · Page 2" },
        { "dictionary-edit", "Edit a lookup" },
        { "dictionary-font-sizes", "Dictionary text size" },
        { "dictionary-long-definition", "Long dictionary definition" },
        { "dictionary-no-definition", "No definition found" },
        { "dictionary-ui", "Dictionary appearance settings" },
        { "dictionary-unavailable", "Dictionary unavailable" },
        { "dictionary-search-action", "Search in book from a definition" },
        { "dictionary-search-results", "Book results for the lookup term" },
        { "embedded-fonts", "Publisher font picker" },
        { "embedded-reading", "Reading with a publisher font" },
        { "finish-book", "Finish reading confirmation" },
        { "fonts", "Font picker" },
        { "fonts-kf-variants", "Font picker with KF variants" },
        { "fonts-kf-default", "Default font with KF variants" },
        { "publisher-font-default", "Publisher Fonts as the default" },
        { "default-font-choices", "Default global font" },
        { "footnotes-long-preview", "Long footnote" },
        { "footnotes-page-2", "Footnote · Page 2" },
        { "footnotes-preview", "Footnote preview" },
        { "header-footer-fields", "Choose a header or footer field" },
        { "header-footer", "Headers and footers" },
        { "header-footer-large", "Headers and footers: enlarged text" },
        { "header-footer-off", "Headers and footers: disabled" },
        { "layout", "Page layout" },
        { "margin-preview-horizontal", "Horizontal margin preview" },
        { "margin-preview-vertical", "Vertical margin preview" },
        { "beautifier-off", "Recommended book normalization" },
        { "beautifier-rules", "Custom book normalization" },
        { "beautifier-full", "Full book normalization" },
        { "beautifier-settings", "Book normalization in Settings" },
        { "library-book-menu", "Book actions" },
        { "library-empty", "Welcome to the library" },
        { "library-folders", "Book folders" },
        { "library-folders-empty", "No book folders" },
        { "library-grid", "All books" },
        { "library-grid-large", "Large covers" },
        { "library-grid-last", "All books · Last page" },
        { "library-grid-new", "Books not yet started" },
        { "library-grid-small", "Small covers" },
        { "library-grid-sort", "Sort the cover grid" },
        { "library-home", "Continue reading" },
        { "library-home-empty", "No books in progress" },
        { "library-list", "Book list" },
        { "library-list-next", "Book list · Next page" },
        { "library-list-series", "Books by series" },
        { "library-more", "Library options" },
        { "library-about", "About Kobalt from the Library" },
        { "library-acknowledgments", "Acknowledgments from the Library" },
        { "library-license", "License from the Library" },
        { "library-new-book-menu", "New book actions" },
        { "library-opening-error", "Book could not be opened" },
        { "library-pressed", "Pressed book cover" },
        { "library-reading-book-menu", "Reading book actions" },
        { "library-remove-confirmation", "Remove a book" },
        { "library-scanning", "Scanning for books" },
        { "library-search-empty", "No matching books" },
        { "library-search-results", "Matching books" },
        { "library-sort", "Sort the book list" },
        { "license", "License text" },
        { "links-website", "Open a website" },
        { "maths-equations", "Equations" },
        { "maths-reading", "Calculus Made Easy" },
        { "menu", "Reading menu" },
        { "menu-return-pressed", "Return button pressed" },
        { "reading", "Reading" },
        { "loading-cover", "Opening a book with its cover" },
        { "loading-cover-slow", "Opening a book: delayed hint" },
        { "loading-handoff-cover", "Opening in Nickel with the book cover" },
        { "loading-no-cover", "Opening a book without a cover" },
        { "save-failed", "Saving failed" },
        { "save-retry-failed", "Saving retry failed" },
        { "search", "Search results" },
        { "search-empty", "Search this book" },
        { "search-no-results", "No matching passages" },
        { "settings", "Reader settings" },
        { "settings-controls", "Controls settings" },
        { "settings-defaults", "Default settings" },
        { "settings-display", "Display settings" },
        { "settings-animations-supported", "Page animations on MTK" },
        { "settings-animations-uncertain", "Page animations with compatibility warning" },
        { "settings-animations-unsupported", "Page animations unavailable" },
        { "settings-kosync", "Sync settings" },
        { "kosync-manual-toolbar", "Manual sync toolbar" },
        { "kosync-manual-prompt", "Manual sync explanation" },
        { "kosync-manual-settings", "Manual sync settings" },
        { "kosync-failure", "Sync failure guidance" },
        { "kosync-no-connection", "Sync without a connection" },
        { "kosync-automatic-off", "Automatic sync off" },
        { "kosync-automatic", "Automatic sync" },
        { "kosync-interval", "Sync while reading" },
        { "kosync-password-masked", "Sync password entry" },
        { "kosync-password-shown", "Sync password revealed" },
        { "kosync-syncing", "Sync in progress" },
        { "kosync-completed", "Sync complete" },
        { "settings-styling", "Styling settings" },
        { "styling-widows", "Widows and orphans preview" },
        { "styling-widows-avoid", "Widows and orphans: avoid both" },
        { "styling-widows-allow", "Widows and orphans: allow both" },
        { "styling-word-spacing", "Word spacing preview" },
        { "styling-hanging-punctuation", "Hanging punctuation: off" },
        { "styling-hanging-punctuation-on", "Hanging punctuation: on" },
        { "settings-system", "System settings" },
        { "settings-standby-confirmation", "Disable reader standby confirmation" },
        { "session-timeout", "New session after sleep" },
        { "settings-time-remaining-format", "Time remaining format" },
        { "stats", "Reading statistics" },
        { "stats-dense-graph", "Dense chapter graph" },
        { "stats-dense-reading", "Moby Dick" },
        { "stats-dense-setting", "Dense graph setting" },
        { "tappable-links-all", "Direct links and footnotes" },
        { "tappable-links-choices", "Direct link choices" },
        { "tappable-links-footnotes", "Direct footnotes only" },
        { "footnotes-tap-targets", "Footnote tap targets" },
        { "footnotes-circle-pressed", "Pressed footnote circle" },
        { "links-rectangle-pressed", "Pressed link rectangle" },
        { "tappable-links-settings", "Direct link settings" },
        { "text", "Text controls" },
        { "text-size-preview", "Text size preview" },
        { "text-spacing-preview", "Line spacing preview" },
        { "styling-paragraph-publisher", "Publisher paragraph adjustments" },
        { "styling-paragraph-row", "Paragraph adjustments in Settings" },
        { "styling-paragraph-custom", "Custom paragraph spacing" },
        { "styling-paragraph-indent", "Custom paragraph indentation" },
        { "styling-paragraph-gaps", "Paragraph gaps with hatching" },
        { "text-weight-preview", "Text weight preview" },
        { "vertical-center", "Centered page text" },
        { "vertical-choices", "Page spacing choices" },
        { "vertical-choices-center", "Page spacing: center illustration" },
        { "vertical-choices-expand", "Page spacing: expand illustration" },
        { "vertical-columns-center", "Centered text in columns" },
        { "vertical-columns-expand", "Expanded text in columns" },
        { "vertical-columns-off", "Columns without page spacing" },
        { "vertical-expand", "Expanded page text" },
        { "vertical-off", "Page spacing off" },
        { "vertical-scope", "Page spacing scope" },
        { "vertical-scope-protection", "Page spacing: protection gaps only" },
        { "vertical-settings", "Page spacing settings" },
        { "widget-keyboard", "Library search keyboard" },
        { "widget-keyboard-300ppi", "Keyboard at native resolution" },
        { "widget-keyboard-accents", "Keyboard accented letters" },
        { "widget-keyboard-libron-300ppi", "Keyboard with Libron" },
        { "widget-keyboard-mono-300ppi", "Keyboard with interface font" },
        { "widget-keyboard-symbols", "Keyboard symbols" },
        { "widget-keyboard-shift", "Keyboard one-shot Shift" },
        { "widget-keyboard-caps-lock", "Keyboard Caps Lock" },
        { "widget-keyboard-qwertz", "German QWERTZ keyboard" },
        { "widget-keyboard-azerty", "French AZERTY keyboard" },
    };
    return titles;
}

inline QString screenshotTitle(const QString &scene, const QJsonObject &book)
{
    if (scene.startsWith("international-")) {
        auto language = book["language_name"].toString();
        if (book["language"] == "zh-Hans")
            language = "Simplified Chinese";
        if (book["language"] == "zh-Hant")
            language = "Traditional Chinese";
        const bool vertical = scene.contains("-vertical-");
        const auto panel = scene.endsWith("-reading") ? "Reading"
                : scene.endsWith("-chapters")         ? "Chapter list"
                                                      : "Font picker";
        return language + " · " + panel + (vertical ? " (vertical)" : "");
    }
    const auto &titles = screenshotTitles();
    if (titles.contains(scene))
        return titles[scene];
    const auto page = QRegularExpression("^(.+)-(\\d+)$").match(scene);
    if (page.hasMatch() && titles.contains(page.captured(1)))
        return titles[page.captured(1)] + " · Page " + page.captured(2);
    return {};
}

inline QPair<QString, int> screenshotIdentity(const QString &scene)
{
    if (scene == "dictionary-definition-page-2")
        return { "dictionary-long-definition", 2 };
    if (scene == "footnotes-page-2")
        return { "footnotes-long-preview", 2 };
    const auto page = QRegularExpression("^(.+)-(\\d+)$").match(scene);
    if (page.hasMatch() && !screenshotTitle(page.captured(1), {}).isEmpty())
        return { page.captured(1), page.captured(2).toInt() };
    return { scene, 1 };
}

// The screens of the full gallery: every titled scene, and the reading, chapter
// list and font picker of each book in the international catalog.
inline QSet<QString> galleryScreens(const QJsonArray &catalog)
{
    QSet<QString> screens;
    const auto &titles = screenshotTitles();
    for (auto it = titles.keyBegin(); it != titles.keyEnd(); ++it)
        screens.insert(screenshotIdentity(*it).first);
    for (const auto &value : catalog) {
        const auto language = value.toObject()["language"].toString();
        for (const bool vertical : { false, true }) {
            if (vertical && !verticalWritingTour(language))
                continue;
            for (const auto *panel : { "reading", "chapters", "fonts" })
                screens.insert("international-" + language + (vertical ? "-vertical-" : "-")
                               + panel);
        }
    }
    return screens;
}

// Lists what the captures lack for the expected screens, or nothing when they
// are complete. Each expected screen needs every viewport and theme, with
// pages numbered from 1 without a gap. Light and dark have the same pages at
// each viewport; 150% text may add pages. A capture of another screen, or of
// a viewport or theme that the gallery does not declare, is also a problem.
// A paged screen that loses its last page in every theme at once still passes
// here; the paging helpers fail an inventory that does not complete its cycle.
inline QStringList screenshotCoverage(const QJsonArray &captures, const QSet<QString> &screens)
{
    QStringList problems;
    QMap<QString, QMap<QString, QSet<int>>> pages;
    for (const auto &value : captures) {
        const auto shot = value.toObject();
        const auto screen = shot["screen"].toString();
        const auto device = shot["device"].toString();
        const auto theme = shot["theme"].toString();
        if (!screens.contains(screen))
            problems.append("Capture of an undeclared screen: " + shot["file"].toString());
        else if (!screenshotDevices().contains(device) || !screenshotThemes(device).contains(theme))
            problems.append("Capture of an undeclared viewport or variant: "
                            + shot["file"].toString());
        else
            pages[screen][device + '/' + theme].insert(shot["capture_page"].toInt());
    }
    QStringList expected(screens.cbegin(), screens.cend());
    expected.sort();
    for (const auto &screen : expected) {
        const auto variants = pages.value(screen);
        for (const auto &device : screenshotDevices()) {
            const auto themed =
                    variants.value(device + "/light") | variants.value(device + "/dark");
            for (const auto &theme : screenshotThemes(device)) {
                const auto captured = variants.value(device + '/' + theme);
                const auto wanted = theme == "large" ? captured : themed;
                const int last =
                        wanted.isEmpty() ? 1 : *std::max_element(wanted.cbegin(), wanted.cend());
                for (int page = 1; page <= last; ++page)
                    if (!captured.contains(page))
                        problems.append(
                                QString("Missing capture: screen %1, viewport %2, variant %3, page %4")
                                        .arg(screen, device, theme)
                                        .arg(page));
            }
        }
    }
    return problems;
}

inline QByteArray screenshotGallery(const QString &directory, const QJsonArray &captures,
                                    const QJsonObject &manifest)
{
    QFile source(":/screenshot-gallery.html");
    if (!source.open(QIODevice::ReadOnly))
        return {};
    const auto page = QString::fromUtf8(source.readAll());
    const auto escape = [](const QString &value) { return value.toHtmlEscaped(); };
    QMap<QString, QJsonObject> scenes, books;
    QMap<QString, QMap<QString, QSet<int>>> variants;
    QString images;
    for (const auto &value : captures) {
        const auto shot = value.toObject();
        const auto file = shot["file"].toString();
        const auto scene = shot["scene"].toString();
        const auto screen = shot["screen"].toString();
        const int capturePage = shot["capture_page"].toInt();
        const auto device = shot["device"].toString();
        const auto theme = shot["theme"].toString();
        if (QFileInfo(file).fileName() != file || !file.endsWith(".png")
            || QFileInfo(directory + '/' + file).isSymLink()
            || !screenshotDevices().contains(device) || !screenshotThemes(device).contains(theme)
            || shot["capture"] != "screen" || shot["ppi"].toInt() != 300 || screen.isEmpty()
            || capturePage < 1) {
            qCritical() << "Invalid screenshot metadata:" << file;
            return {};
        }
        const auto variant = device + '/' + theme;
        if (variants[screen][variant].contains(capturePage)) {
            qCritical() << "Duplicate screenshot page:" << file;
            return {};
        }
        variants[screen][variant].insert(capturePage);
        scenes[screen] = shot;
        const auto book = shot["book"].toObject();
        const auto title = book["title"].toString();
        if (!title.isEmpty())
            books[title + book["writing_mode"].toString()] = book;
        QFile image(directory + '/' + file);
        if (!image.open(QIODevice::ReadOnly))
            return {};
        const auto bytes = image.readAll();
        const auto checksum = QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const auto decoded = QImage::fromData(bytes, "PNG");
        if (checksum != shot["sha256"].toString() || decoded.isNull()
            || decoded.width() != shot["width"].toInt()
            || decoded.height() != shot["height"].toInt() || decoded.dotsPerMeterX() != 11811
            || decoded.dotsPerMeterY() != 11811) {
            qCritical().noquote()
                    << "Screenshot bytes, dimensions or density differ from the manifest:" << file;
            return {};
        }
        images +=
                QString("<img data-scene=\"%1\" data-page=\"%9\" data-device=\"%2\" data-theme=\"%3\" data-file=\"%4\" data-sha256=\"%5\" src=\"data:image/png;base64,%6\" width=\"%7\" height=\"%8\" alt=\"\">")
                        .arg(escape(screen), escape(device), escape(theme), escape(file),
                             escape(shot["sha256"].toString()),
                             QString::fromLatin1(bytes.toBase64()),
                             QString::number(shot["width"].toInt()),
                             QString::number(shot["height"].toInt()), QString::number(capturePage));
    }
    // Every screen in the set needs all its variants and pages.
    const auto problems =
            screenshotCoverage(captures, QSet<QString>(scenes.keyBegin(), scenes.keyEnd()));
    for (const auto &problem : problems)
        qCritical().noquote() << problem;
    if (!problems.isEmpty())
        return {};
    QMap<QString, QString> sections;
    for (auto it = scenes.cbegin(); it != scenes.cend(); ++it) {
        const auto shot = it.value();
        const auto book = shot["book"].toObject();
        const auto title = screenshotTitle(it.key(), book);
        if (title.isEmpty()) {
            qCritical().noquote() << "Screenshot has no gallery title:" << it.key();
            return {};
        }
        const auto group = shot["group"].toString();
        const auto subtitle =
                it.key().startsWith("international-") ? book["title"].toString() : QString();
        const auto search =
                (title + ' ' + it.key() + ' ' + group + ' ' + book["title"].toString()).toLower();
        sections[group] +=
                QString("<figure data-group=\"%1\" data-scene=\"%2\" data-title=\"%3\" data-search=\"%4\">"
                        "<button class=\"capture\" type=\"button\" aria-label=\"Inspect %3\"><img loading=\"lazy\" alt=\"%3\"></button>"
                        "<div class=\"caption\"><figcaption>%3<small>%5</small></figcaption><span class=\"tag\"></span></div></figure>")
                        .arg(escape(group), escape(it.key()), escape(title), escape(search),
                             escape(subtitle));
    }
    QString groups, options, credits;
    for (const auto &group :
         QStringList{ "Reading", "International books", "Mathematics", "Typography", "Layout",
                      "Links and footnotes", "Dictionary", "Bookmarks and annotations",
                      "Statistics", "Search", "Settings and About", "Library" }) {
        if (!sections.contains(group))
            continue;
        groups += "<section class=\"group\"><div class=\"group-title\"><h2>" + escape(group)
                + "</h2></div><div class=\"screens\">" + sections[group] + "</div></section>";
        options += "<option value=\"" + escape(group) + "\">" + escape(group) + "</option>";
    }
    for (const auto &book : books) {
        auto title = escape(book["title"].toString());
        const auto source = book["source"].toString();
        if (source.startsWith("https://"))
            title = "<a href=\"" + escape(source) + "\">" + title + "</a>";
        credits += "<p>" + title + " · " + escape(book["author"].toString()) + "<br>"
                + escape(book["rights"].toString()) + " " + escape(book["license"].toString())
                + "</p>";
        for (const auto &chapter : book["chapters"].toArray()) {
            const auto item = chapter.toObject();
            const auto url = item["source"].toString();
            if (url.startsWith("https://"))
                credits += "<p><a href=\"" + escape(url) + "\">" + escape(item["title"].toString())
                        + "</a></p>";
        }
    }
    const QMap<QString, QString> values{
        { "SUITE",
          manifest["suite"] == "light"            ? "Light"
                  : manifest["suite"] == "scenes" ? "Selected scenes"
                  : manifest["suite"] == "cases"  ? "Selected cases"
                                                  : "Full" },
        { "GROUPS", groups },
        { "OPTIONS", options },
        { "IMAGES", images },
        { "COUNT", QString::number(scenes.size()) },
        { "CAPTURES", QString::number(captures.size()) },
        { "BOOKS", credits },
        { "BUILD",
          escape("Source " + manifest["commit"].toString()
                 + (manifest["modified"].toBool() ? " (modified)" : "") + " · Qt "
                 + manifest["qt"].toString() + " · " + manifest["version"].toString()) }
    };
    QString result;
    const QRegularExpression token("@@([A-Z]+)@@");
    auto matches = token.globalMatch(page);
    qsizetype last = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!values.contains(match.captured(1)))
            return {};
        result += page.mid(last, match.capturedStart() - last) + values[match.captured(1)];
        last = match.capturedEnd();
    }
    return (result + page.mid(last)).toUtf8();
}
} // namespace kobalt::test
