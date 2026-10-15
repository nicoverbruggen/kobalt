#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "interfaces/reader/panels/dictionary_panel.h"
#include "interfaces/reader/settings/acknowledgments.h"
#include "interfaces/reader/panels/chapter_graph.h"
#include "interfaces/reader/ui/controls/paged_text.h"
#include "interfaces/reader/panels/search_panel.h"
#include "interfaces/reader/ui/style/styles.h"
#include "host_services.h"
#include "reader_host.h"
#include "host_presentation.h"
#include "platform/android/start_screen.h"
#include "interfaces/reader/views/opening.h"
#include "dictionaries/manager.h"
#include "dictionaries/stardict.h"
#include "reader_canvas.h"
#include "host_platform.h"
#include "host_style.h"
#include "host_fixture.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "interfaces/reader/views/reading_page.h"
#include "interfaces/reader/settings/dictionaries_page.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "reading/reading_density.h"
#include "storage/reader_storage.h"
#include "support/strings.h"
#include <QApplication>
#include <QCheckBox>
#include <QBuffer>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QFileInfo>
#include <cstdio>
#include <stdexcept>

using namespace kobalt;
namespace {
void require(bool ok, const QString &message) {
    if (!ok)
        throw std::runtime_error(message.toStdString());
}
void waitFor(const std::function<bool()> &done, const QString &message, int timeout = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QTest::qWait(10);
    }
    require(done(), message);
}
struct LookupState {
    QString mode = "definition", word, selected;
    int selections = 0, lookups = 0;
};
class ReferenceDictionary final : public DictionaryService {
public:
    explicit ReferenceDictionary(std::shared_ptr<LookupState> state) : state(std::move(state)) {}
    QVector<DictionaryChoice> choices() const override {
        return {{"en-en", "English monolingual"}};
    }
    bool select(const QString &, const QString &id, QString *) override {
        state->selected = id;
        ++state->selections;
        return id == "en-en";
    }
    DictionaryResult lookup(const QString &, const QString &word) override {
        state->word = word;
        ++state->lookups;
        DictionaryResult result;
        result.word = word;
        result.received = true;
        result.restored = true;
        result.dictionaryId = "en-en";
        result.dictionary = "English monolingual";
        if (state->mode == "missing")
            return result;
        if (state->mode == "unavailable") {
            result.received = false;
            result.error = "The dictionary service is unavailable.";
            return result;
        }
        if (!ready) {
            const auto archive = qEnvironmentVariable("KOBALT_SCREENSHOT_DICTIONARY");
            require(!archive.isEmpty(), "The screenshot requires a real reference dictionary archive");
            QString error;
            for (const auto &entry : DictionaryManager::catalog())
                if (entry.id == "en-en")
                    require(DictionaryManager::installArchive(directory.path(), entry, archive, &error), error);
            require(dictionary.open(directory.path() + "/en-en", "en-en", &error), error);
            ready = true;
        }
        result = dictionary.lookup(word);
        result.dictionaryId = "en-en";
        result.dictionary = "English monolingual";
        result.source = "Princeton WordNet and Wiktionary · koreader-dicts v1.2.0 · CC BY-SA 4.0";
        return result;
    }

private:
    std::shared_ptr<LookupState> state;
    QTemporaryDir directory;
    StarDict dictionary;
    bool ready = false;
};

class Tour {
public:
    Tour(const QString &book, const QString &output, const QString &group, const QString &theme,
         const EngineFontInputs &fonts, const QString &placeholder, QSize size)
        : output(output), group(group), theme(theme), size(size), paths{temporary.path()},
          host(paths, fonts, placeholder), lookup(std::make_shared<LookupState>()) {
        try {
            require(temporary.isValid(), "Cannot create isolated reader state");
            QDir().mkpath(output);
            require(storage::writeJson(paths.preferences(), {{"dark_mode", theme == "dark"},
                                                             {"show_clock", true},
                                                             {"dense_stats_chart", group == "stats-dense"}}),
                    "Cannot write fixture preferences");
            if (group == "sync") {
                host.progress = std::make_shared<test::HostProgress>();
                host.positions = {[](const ReaderModel &, const QString &position, QString *) {
                                      return position;
                                  },
                                  [](const ReaderModel &model, QString *) {
                                      return model.position();
                                  },
                                  [](const QString &position) {
                                      return position;
                                  }};
                host.progress->current = {
                    QUrl::fromLocalFile(QFileInfo(book).absoluteFilePath()).toString(), {}, 100, 1, true};
            }
            host.configureServices = [this, state = lookup, desktop = group == "native-ui",
                                      catalogue = discoverReadingFonts(fonts.sources)](ReaderHostServices &services) {
                services.desktopUi = desktop;
                services.systemRotation = this->group == "layout-system-rotation";
                if (this->group == "bluetooth")
                    services.observeBluetooth = [this](QObject *, std::function<void(bool)> changed) {
                        bluetoothChanged = std::move(changed);
                    };
                if (desktop) {
                    services.lightAvailable = [] {
                        return false;
                    };
                    services.animationsAvailable = [] {
                        return false;
                    };
                }
                services.fontFamilies = [catalogue] {
                    QStringList families;
                    for (const auto &font : catalogue)
                        if (font.origin == FontOrigin::BuiltIn)
                            families.append(font.family);
                    return families;
                };
                services.dictionary = [state] {
                    return std::make_unique<ReferenceDictionary>(state);
                };
                services.browserAvailable = [] {
                    return true;
                };
                services.openBrowser = [](const QUrl &, QWidget *) {
                    return true;
                };
            };
            QSignalSpy opened(&host, &test::ReaderHost::opened), failed(&host, &test::ReaderHost::failed);
            host.open(book, size);
            waitFor(
                [&] {
                    return !opened.isEmpty() || !failed.isEmpty();
                },
                "Book opening timed out", 60000);
            require(failed.isEmpty(), failed.isEmpty() ? QString() : failed.first().first().toString());
            canvas = std::make_unique<test::ReaderCanvas>(host.window(), size);
            canvas->resize(size);
            canvas->show();
            settled();
        } catch (...) {
            close();
            throw;
        }
    }
    void close() noexcept {
        host.shutdown();
        try {
            waitFor(
                [&] {
                    return host.idle();
                },
                "Reader did not close");
        } catch (const std::exception &error) {
            qFatal("%s", error.what());
        }
    }
    ~Tour() {
        close();
    }
    QWidget *widget(const QString &name) const {
        auto *result = host.window()->findChild<QWidget *>(name);
        require(result, "Missing widget: " + name);
        return result;
    }
    bool visible(const QString &name) const {
        auto *result = host.window()->findChild<QWidget *>(name);
        return result && result->isVisible();
    }
    void settled() {
        QCoreApplication::processEvents();
        waitFor(
            [&] {
                auto *settings = host.window()->findChild<QWidget *>("rrSettings");
                return !host.session()->busy() && (!settings || settings->isEnabled());
            },
            "Renderer busy", 60000);
        QCoreApplication::processEvents();
    }
    void tap(const QString &name) {
        QPointer<QWidget> target;
        // A preview refresh can replace a font choice while input is waiting.
        waitFor(
            [&] {
                target = host.window()->findChild<QWidget *>(name);
                return target && target->isVisible() && target->isEnabled();
            },
            "Unavailable control: " + name);
        clickAt(target->mapTo(canvas->surface(), target->rect().center()), target);
        settled();
    }
    void clickAt(QPoint point, QWidget *expected = nullptr) {
        canvas->ensureVisible(point.x(), point.y(), 30, 30);
        QCoreApplication::processEvents();
        const QPoint local = canvas->surface()->mapTo(canvas->viewport(), point);
        require(canvas->viewport()->rect().contains(local), "Input point is clipped by the canvas viewport");
        auto *hit = canvas->viewport()->childAt(local);
        require(hit && (!expected || hit == expected || expected->isAncestorOf(hit)),
                "Input point is intercepted by another control or overlay");
        require(canvas->windowHandle(), "Canvas input window is unavailable");
        QTest::mouseClick(canvas->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                          canvas->surface()->mapTo(canvas.get(), point));
    }
    bool lastListPage(const QString &prefix) {
        auto *counter = qobject_cast<QLabel *>(widget(prefix + "Page"));
        require(counter, "List page counter is missing");
        const auto parts = counter->text().split(" of ");
        require(parts.size() == 2, "List page counter has an unexpected format");
        return parts.first() == parts.last();
    }
    int checkPagerEnds(const QString &prefix, QWidget *content = nullptr) {
        auto *next = widget(prefix + "Next");
        auto *previous = widget(prefix + "Previous");
        auto *counter = qobject_cast<QLabel *>(widget(prefix + "Page"));
        require(counter, "Pager counter is not a label");
        const QString initial = counter->text();
        const auto firstContent = content ? content->grab().toImage() : QImage();
        int turns = 0;
        while (next->isVisible() && next->isEnabled() && !lastListPage(prefix)) {
            require(turns < 128, "Pager did not reach its end within 128 pages");
            const QString before = counter->text();
            tap(prefix + "Next");
            require(counter->text() != before, "Next did not advance the page counter");
            require(previous->isVisible() && previous->isEnabled(), "Previous unavailable after advancing");
            ++turns;
        }
        require(turns > 0, "Long-content fixture has only one reachable page");
        if (content) {
            require(content->isVisible() && content->grab().toImage() != firstContent,
                    "Last page did not expose different content");
            const QRect bounds(content->mapTo(canvas->surface(), QPoint()), content->size());
            require(canvas->surface()->rect().contains(bounds), "Paged content extends beyond the reader surface");
        }
        if (auto *license = dynamic_cast<LicenseText *>(content))
            require(license->page == license->pages() - 1, "License pager did not reach the final content page");
        if (auto *graph = dynamic_cast<ChapterGraph *>(content))
            require(graph->graphPage() == graph->graphPages() - 1, "Graph pager did not reach the final chapters");
        for (int n = 0; n < turns; ++n) {
            const QString before = counter->text();
            tap(prefix + "Previous");
            require(counter->text() != before, "Previous did not restore an earlier page");
        }
        require(counter->text() == initial, "Pager did not restore its first page");
        return turns;
    }
    void pageTap() {
        auto *page = widget("rrPage");
        clickAt(page->mapTo(canvas->surface(), QPoint(size.width() / 2, size.height() / 2)));
        settled();
    }
    void menu() {
        if (!visible("rrControlsTop"))
            pageTap();
        require(visible("rrControlsTop"), "Reading menu did not open");
    }
    void capture(const QString &name, const QString &required = "rrPage", QWidget *surface = nullptr,
                 qreal surfaceDpr = 2) {
        if (!qEnvironmentVariable("KOBALT_SCREENSHOT_ONLY").isEmpty() &&
            !qEnvironmentVariable("KOBALT_SCREENSHOT_ONLY").split(',').contains(name))
            return;
        settled();
        waitFor(
            [&] {
                return surface ? (surface->findChild<QWidget *>(required) &&
                                  surface->findChild<QWidget *>(required)->isVisible())
                               : visible(required);
            },
            "Capture target is hidden: " + required);
        QImage previous, image;
        int stable = 0;
        QElapsedTimer timeout;
        timeout.start();
        while (stable < 3 && timeout.elapsed() < 10000) {
            QCoreApplication::processEvents();
            if (surface) {
                QPixmap frame(size);
                frame.setDevicePixelRatio(surfaceDpr);
                frame.fill(Qt::white);
                surface->render(&frame);
                image = frame.toImage();
                image.setDevicePixelRatio(1);
            } else
                image = canvas->capture();
            stable = image == previous ? stable + 1 : 0;
            previous = image;
            QTest::qWait(20);
        }
        require(stable == 3 && !image.isNull(), "Image did not settle: " + name);
        const QString filename = name + ".png";
        require(image.save(output + "/" + filename), "Could not save " + filename);
        QJsonArray widgets;
        auto *root = surface ? surface : host.window();
        for (auto *item : root->findChildren<QWidget *>()) {
            if (!item->isVisible() || item->objectName().isEmpty())
                continue;
            const auto point = item->mapTo(root, QPoint{});
            QJsonObject value{{"name", item->objectName()}, {"x", point.x()},           {"y", point.y()},
                              {"width", item->width()},     {"height", item->height()}, {"enabled", item->isEnabled()}};
            if (auto *label = qobject_cast<QLabel *>(item))
                value["text"] = label->text();
            if (auto *button = qobject_cast<QAbstractButton *>(item))
                value["text"] = button->text();
            widgets.append(value);
        }
        QFile assetManifest(QStringLiteral(KOBALT_ASSET_ROOT) + "/manifest.json");
        require(assetManifest.open(QIODevice::ReadOnly), "Asset manifest unavailable");
        const auto assetsHash =
            QString::fromLatin1(QCryptographicHash::hash(assetManifest.readAll(), QCryptographicHash::Sha256).toHex());
        QJsonObject metadata{{"assets_sha256", assetsHash},
                             {"group", group},
                             {"theme", theme},
                             {"name", name},
                             {"file", filename},
                             {"width", size.width()},
                             {"height", size.height()},
                             {"clock", "09:00"},
                             {"battery_percent", 100},
                             {"required_widget", required},
                             {"selected_word", lookup->word},
                             {"page", host.session()->page() + 1},
                             {"chapter", host.session()->chapterTitle()},
                             {"chapter_count", host.session()->chapters().size()},
                             {"widgets", widgets},
                             {"device_fixture", test::fixtureProvenance()},
                             {"qt", qVersion()},
                             {"platform", QGuiApplication::platformName()},
                             {"ppi", ui::density()},
                             {"reading_ppi", readingPpi.load()},
                             {"dpr", image.devicePixelRatio()},
                             {"style", test::hostStyleProfile},
                             {"font_profile", test::hostFontProvenance()}};
        if (group == "dictionary")
            metadata["dictionary_reference"] = "koreader-dicts v1.2.0/en-en";
        if (group == "dictionary" && lookup->mode != "definition")
            metadata["dictionary_fixture"] = "emulated-error-state";
        if (dictionaryManagementFixture)
            metadata["dictionary_fixture"] = "emulated-management-state";
        require(storage::writeJson(output + "/" + name + ".json", metadata), "Could not save metadata");
        std::printf("CAPTURE %s/%s/%s\n", theme.toUtf8().constData(), group.toUtf8().constData(),
                    name.toUtf8().constData());
        std::fflush(stdout);
    }
    SearchHit find(const QString &text) {
        struct Result {
            QVector<SearchHit> hits;
            bool done = false;
        };
        auto result = std::make_shared<Result>();
        host.session()->search(text, [result](SearchBatch batch) {
            result->hits += batch.hits;
            result->done = batch.done;
        });
        waitFor(
            [&] {
                return result->done;
            },
            "Search did not finish: " + text, 60000);
        require(!result->hits.isEmpty(), "Fixture text not found: " + text);
        const auto hit = result->hits.first();
        require(host.session()->goTo(hit.start), "Cannot go to fixture text");
        settled();
        return hit;
    }
    QPoint selection(const SearchHit &hit) {
        struct Result {
            QVector<QRect> rects;
            bool done = false;
        };
        auto result = std::make_shared<Result>();
        host.session()->rangeRects(hit.start, hit.end, [result](QVector<QRect> rects) {
            result->rects = rects;
            result->done = true;
        });
        waitFor(
            [&] {
                return result->done;
            },
            "Selection rectangles unavailable");
        require(!result->rects.isEmpty(), "Empty selection");
        return result->rects.first().center();
    }
    void hold(QPoint point) {
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        point += QPoint(0, page->topHeight());
        QTest::mousePress(page, Qt::LeftButton, Qt::NoModifier, point);
        QTest::qWait(ui::metrics::lookup::holdMilliseconds + 50);
        QTest::mouseRelease(page, Qt::LeftButton, Qt::NoModifier, point);
    }
    void dictionary(const QString &word) {
        const auto hit = find(word);
        const auto at = selection(hit);
        capture("selection-page");
        auto selected = std::make_shared<WordSelection>();
        auto done = std::make_shared<bool>(false);
        host.session()->wordAt(at, [selected, done](WordSelection value) {
            *selected = std::move(value);
            *done = true;
        });
        waitFor(
            [&] {
                return *done;
            },
            "Word selection timed out");
        require(!selected->text.isEmpty(),
                QString("No word at fixture point %1,%2 for %3").arg(at.x()).arg(at.y()).arg(word));
        hold(at);
        waitFor(
            [&] {
                return visible("rrDictionary");
            },
            "Dictionary did not open");
        capture("definition", "rrDictionary");
        auto *panel = dynamic_cast<DictionaryPanel *>(widget("rrDictionary"));
        require(panel, "Dictionary lookup panel missing");
        panel->setLanguage("en");
        panel->setChoices({{"en-en", "English monolingual", "en", true},
                           {"downloaded:en-fr", "English to French", "en", false},
                           {"downloaded:ar-ar", "Arabic monolingual", "ar", false}});
        tap("rrDictionarySelect");
        capture("lookup-choices", "rrDictionaryChoices");
        const int selections = lookup->selections, lookups = lookup->lookups;
        QString currentChoice;
        for (auto *button : panel->findChildren<QPushButton *>())
            if (button->objectName().startsWith("rrDictionaryChoice") && button->isChecked())
                currentChoice = button->objectName();
        require(!currentChoice.isEmpty(), "Current dictionary choice is missing");
        tap(currentChoice);
        // DictionaryManager selects the fallback in select(), then again in
        // lookup() before invoking the real reference dictionary.
        require(lookup->selections == selections + 2 && lookup->selected == "en-en" && lookup->lookups == lookups + 1,
                QString("Dictionary choice callback counts: selections %1 -> %2, lookups %3 -> %4, selected %5")
                    .arg(selections)
                    .arg(lookup->selections)
                    .arg(lookups)
                    .arg(lookup->lookups)
                    .arg(lookup->selected));
        require(!visible("rrDictionaryChoices"), "Dictionary choices stayed open after selection");
        tap("rrDictionaryClose");
        const auto longHit = find("set");
        const auto longAt = selection(longHit);
        hold(longAt);
        waitFor(
            [&] {
                return visible("rrDictionary");
            },
            "Long reference entry did not open");
        capture("long-definition", "rrDictionary");
        auto *definition = dynamic_cast<ui::PagedText *>(widget("rrDefinition"));
        require(definition && definition->pages() > 1 && definition->page() == 0, "Long definition did not paginate");
        const auto firstDefinition = definition->grab().toImage();
        tap("rrDictionaryNext");
        capture("definition-page-2", "rrDictionary");
        require(definition->page() == 1 && definition->grab().toImage() != firstDefinition,
                "Next did not change the definition page and rendered content");
        tap("rrDictionaryPrevious");
        require(definition->page() == 0 && definition->grab().toImage() == firstDefinition,
                "Previous did not restore the first definition page");
        tap("rrDictionaryClose");
        lookup->mode = "missing";
        hold(longAt);
        waitFor(
            [&] {
                return visible("rrDictionary");
            },
            "Missing definition did not open");
        capture("no-definition", "rrDictionary");
        tap("rrDictionaryClose");
        lookup->mode = "unavailable";
        hold(longAt);
        waitFor(
            [&] {
                return visible("rrDictionary");
            },
            "Dictionary error did not open");
        capture("unavailable", "rrDictionary");
    }
    void supplementalFonts() {
        if (supplementalFontFamily(host.session()->state()["language"].toString()).isEmpty())
            return;
        pageTap();
        menu();
        tap("rrNavText");
        tap("rrFont");
        for (int turn = 0; !lastListPage("rrFontPager"); ++turn) {
            require(turn < 128, "Font pager did not reach its final page");
            tap("rrFontPagerNext");
        }
        auto *section = qobject_cast<QLabel *>(widget("rrFontSection"));
        require(section && section->text() == "SUPPLEMENTAL FONTS", "Language font section did not appear");
        bool matchingFont = false;
        const auto family = supplementalFontFamily(host.session()->state()["language"].toString());
        for (auto *label : host.window()->findChildren<QLabel *>())
            if (label->isVisible() && label->objectName().endsWith("Title") && label->text() == family)
                matchingFont = true;
        require(matchingFont, "Supplemental page does not offer the document's font: " + family);
        capture("supplemental-fonts", "rrChoices");
    }
    void run() {
        if (group == "android-startup") {
            android::StartScreen screen([](QWidget *) {
                return std::unique_ptr<WindowPresentation>{};
            });
            // Android startup uses logical dimensions, independently of reader density.
            screen.resize(size / 2);
            screen.show();
            capture("no-last-book", "androidOpenBook", &screen);
            screen.setLastBook(
                {{"path", "/alice.epub"}, {"title", host.session()->title()}, {"author", host.session()->author()}});
            capture("last-book-no-cover", "androidLastBook", &screen);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            host.session()->coverThumbnail().save(&buffer, "PNG");
            screen.setLastBook({{"path", "/alice.epub"},
                                {"title", host.session()->title()},
                                {"author", host.session()->author()},
                                {"cover", QString::fromLatin1(png.toBase64())}});
            capture("last-book", "androidLastBook", &screen);
            return;
        }

        const bool international = group.startsWith("international-");
        if (international)
            group.remove(0, 14);
        if (group == "native-ui") {
            find("Alice was beginning");
            capture("reading-defaults");
            menu();
            capture("menu-defaults", "rrBottomNavigation");
            tap("rrNavChapters");
            capture("chapters-defaults", "rrChoices");
            pageTap();
            menu();
            tap("rrSettings");
            capture("display-defaults", "rrDarkMode");
            tap("rrSettingsAdvanced");
            capture("advanced-defaults", "rrSmoothImages");
            tap("rrSettingsUI");
            capture("ui-defaults", "rrHideStatusBar");
            tap("rrHideStatusBar");
            tap("rrHideDeviceOptions");
            pageTap();
            menu();
            capture("menu-device-chrome", "rrMenuClock");
            tap("rrSettings");
            capture("display-device-options", "rrScreenRefresh");
            tap("rrSettingsUI");
            capture("ui-device-chrome", "rrHideStatusBar");
            return;
        }
        if (international) {
            const auto chapters = host.session()->chapters();
            require(!chapters.isEmpty(), "Public-domain book has no chapters");
            require(host.session()->goTo(chapters.first().position), "First literary chapter did not open");
            const bool vertical = group == "writing-modes" || group.endsWith("-vertical");
            settled();
            capture(vertical ? "vertical-chapter" : "reading");
            if (group == "writing-modes") {
                require(chapters.size() >= 3 && host.session()->goTo(chapters[1].position),
                        "Horizontal chapter did not open");
                settled();
                require(host.session()->goTo(chapters[2].position), "Next vertical chapter did not open");
                settled();
            }
            // These three views cover LTR, RTL and vertical chapter titles.
            if (group == "greek" || group == "arabic" || group == "writing-modes") {
                menu();
                tap("rrNavChapters");
                capture("chapters", "rrChoices");
                supplementalFonts();
            } else {
                menu();
                supplementalFonts();
            }
            return;
        }
        if (group == "stats-dense") {
            require(host.session()->chapters().size() >= 100, "Long stats fixture has fewer than one hundred chapters");
            find("But look! here come more crowds");
            menu();
            tap("rrNavStats");
            capture("dense-chapter-graph", "rrChapterGraph");
            auto *graph = dynamic_cast<ChapterGraph *>(widget("rrChapterGraph"));
            auto *bars = widget("rrChapterBars");
            require(graph && graph->rect().contains(QRect(bars->mapTo(graph, QPoint()), bars->size())),
                    "Dense chart is clipped by its panel");
            // Dense mode fits all chapters; sparse mode exposes the supported pager.
            tap("rrSettings");
            tap("rrSettingsStats");
            tap("rrDenseGraph");
            tap("rrSettings");
            pageTap();
            menu();
            tap("rrNavStats");
            require(graph->graphPages() > 1 && graph->graphPage() == 0,
                    "Sparse hundred-chapter chart did not paginate");
            const int graphTurns = checkPagerEnds("rrGraph", graph);
            require(graphTurns == graph->graphPages() - 1 && graph->graphPage() == 0,
                    "Not every chart page was reachable");
            return;
        }
        if (group == "maths") {
            // The pinned Gutenberg edition contains raster and SVG equations.
            find("IN this chapter we go to work finding solutions");
            for (int i = 0; i < 4; ++i) {
                capture(QString("equations-%1").arg(i + 1));
                host.session()->go(host.session()->page() + 1);
                settled();
            }
            return;
        }
        if (group == "footnotes") {
            find("small quantity of the second order");
            QPoint target;
            for (int n = 0; n < 4 && target.isNull(); ++n) {
                for (const auto &link : host.session()->pageLinks()) {
                    auto details = std::make_shared<LinkDetails>();
                    auto done = std::make_shared<bool>(false);
                    host.session()->linkDetails(link.source, [details, done](LinkDetails value) {
                        *details = std::move(value);
                        *done = true;
                    });
                    waitFor(
                        [&] {
                            return *done;
                        },
                        "Footnote details unavailable");
                    if (details->kind == LinkKind::Footnote) {
                        target = link.rectangles.first().center();
                        break;
                    }
                }
                if (target.isNull()) {
                    host.session()->go(host.session()->page() + 1);
                    settled();
                }
            }
            require(!target.isNull(), "No footnote near the pinned reference");
            const QString origin = host.session()->state().value("position").toString();
            const int originPage = host.session()->page();
            require(!origin.isEmpty(), "Footnote origin position is missing");
            capture("reference");
            hold(target);
            capture("actions", "rrLinkActions");
            tap("rrLinkGo");
            capture("preview", "rrLinkPanel");
            tap("rrLinkOpen");
            capture("passage");
            menu();
            capture("return-row", "rrNavLinkReturn");
            tap("rrNavLinkReturn");
            waitFor(
                [&] {
                    return !host.session()->busy() && host.session()->page() == originPage &&
                           host.session()->state().value("position").toString() == origin;
                },
                "Footnote return did not restore the original passage");
            return;
        }
        if (group == "dictionary") {
            dictionary("sister");
            return;
        }
        find(group == "embedded-fonts" ? "Embedded family text" : "sister");
        if (group == "sync") {
            const auto origin = host.session()->state().value("position").toString();
            find("Queen of Hearts");
            auto progress = host.progress->current;
            progress.location = host.session()->state().value("position").toString();
            host.session()->goTo(origin);
            settled();
            progress.modified = 200;
            host.progress->publish(progress);
            host.window()->progressTick();
            capture("new-position", "rrSyncedPosition");
            tap("rrKeepPosition");
            progress.modified = 300;
            progress.location = "unavailable-fixture-position";
            host.progress->publish(progress);
            host.window()->progressTick();
            capture("unavailable-position", "rrSyncedPosition");
            tap("rrGoToSyncedPosition");
            waitFor(
                [&] {
                    return widget("rrKeepPosition")->isEnabled();
                },
                "Failed sync did not finish");
            capture("position-error", "rrSyncedPosition");
            return;
        }
        if (group == "bionic") {
            find("Alice was beginning");
            capture("off");
            require(host.session()->setWeight(8), "Could not set font-preview weight");
            settled();
            menu();
            tap("rrNavText");
            tap("rrFont");
            capture("weighted-fonts", "rrChoices");
            tap("rrFontClose");
            pageTap();
            require(host.session()->setWeight(0), "Could not reset font-preview weight");
            settled();
            for (const auto *mode : {"bold"}) {
                menu();
                tap("rrSettings");
                tap("rrBionicReading");
                capture(QString(mode) + "-setting", "rrBionicReading");
                // The settings sheet covers the page center; use its toolbar action to apply.
                tap("rrSettings");
                waitFor(
                    [&] {
                        return !host.session()->busy() &&
                               host.session()->state().value("bold_reading").toString() == mode;
                    },
                    "Bionic apply did not finish", 60000);
                require(host.session()->state().value("bold_reading").toString() == mode,
                        "Bionic choice was not applied");
                if (visible("rrControlsTop"))
                    pageTap();
                find("Alice was beginning");
                capture(mode);
                menu();
                tap("rrNavText");
                capture(QString(mode) + "-weight", "rrFontWeight");
                tap("rrFont");
                capture(QString(mode) + "-fonts", "rrChoices");
                tap("rrFontClose");
                pageTap();
            }
            return;
        }
        if (group == "bluetooth") {
            menu();
            capture("off", "rrBottomNavigation");
            require(bool(bluetoothChanged), "Bluetooth subscription missing");
            bluetoothChanged(true);
            settled();
            capture("on", "rrMenuBluetooth");
            pageTap();
            require(!visible("rrMenuBluetooth"), "Bluetooth visible over the reading page");
            capture("reading");
            menu();
            bluetoothChanged(false);
            settled();
            require(!visible("rrMenuBluetooth"), "Disabled Bluetooth remains visible");
            capture("disabled", "rrBottomNavigation");
            return;
        }
        if (group == "reading") {
            capture("reading");
            menu();
            capture("menu", "rrBottomNavigation");
            supplementalFonts();
            return;
        }
        if (group == "links") {
            host.session()->go(0);
            settled();
            QPoint target;
            for (int n = 0; n < 15 && target.isNull(); ++n) {
                for (const auto &link : host.session()->pageLinks()) {
                    auto details = std::make_shared<LinkDetails>();
                    auto done = std::make_shared<bool>(false);
                    host.session()->linkDetails(link.source, [details, done](LinkDetails value) {
                        *details = std::move(value);
                        *done = true;
                    });
                    waitFor(
                        [&] {
                            return *done;
                        },
                        "Link details unavailable");
                    if (details->kind == LinkKind::Passage) {
                        target = link.rectangles.first().center();
                        break;
                    }
                }
                if (target.isNull()) {
                    host.session()->go(host.session()->page() + 1);
                    settled();
                }
            }
            require(!target.isNull(), "No internal link found in Alice front matter");
            const int contentsPage = host.session()->page();
            capture("contents");
            menu();
            capture("links-controls", "rrLinkDiscovery");
            tap("rrRevealLinks");
            capture("links-revealed", "rrLinkDiscoveryExit");
            pageTap();
            menu();
            tap("rrSettings");
            tap("rrSettingsAdvanced");
            auto *indicatorOption = qobject_cast<QCheckBox *>(widget("rrShowLinkIndicator"));
            require(indicatorOption, "Link indicator setting missing");
            indicatorOption->setChecked(true);
            tap("rrSettings");
            settled();
            pageTap();
            capture("link-indicator", "rrLinkIndicator");
            hold(target);
            waitFor(
                [&] {
                    return visible("rrLinkActions");
                },
                "Link actions did not open");
            capture("actions", "rrLinkActions");
            tap("rrLinkGo");
            capture("destination");
            menu();
            capture("return-row", "rrNavLinkReturn");
            // Back on the contents page by page turns, both rows share the menu.
            pageTap();
            host.session()->go(contentsPage);
            settled();
            menu();
            require(visible("rrLinkDiscovery"), "Links row missing beside the return row");
            capture("links-with-return", "rrNavLinkReturn");
            tap("rrNavLinkReturn");
            capture("returned");
            host.session()->go(0);
            settled();
            for (int n = 0; n < 15 && host.session()->pageLinks().isEmpty(); ++n) {
                host.session()->go(host.session()->page() + 1);
                settled();
            }
            require(!host.session()->pageLinks().isEmpty(), "Alice website link missing");
            hold(host.session()->pageLinks().first().rectangles.first().center());
            tap("rrLinkGo");
            capture("website-link", "rrBrowserConfirmation");
            return;
        }
        menu();
        if (group == "settings") {
            tap("rrNavText");
            tap("rrFont");
            tap("rrFontClose");
            tap("rrSettings");
            for (const auto &tab : {"Display", "Advanced", "Stats", "About"}) {
                tap("rrSettings" + QString(tab));
                capture(QString(tab).toLower(), "rrGeneralSettings");
            }
            tap("rrSettingsDictionaries");
            auto *dictionaries = dynamic_cast<reader::DictionariesPage *>(widget("rrDictionaries"));
            require(dictionaries, "Dictionary management page missing");
            const auto catalog = DictionaryManager::catalog();
            dictionaryManagementFixture = true;
            dictionaries->setLanguage("en");
            dictionaries->setCatalog(catalog);
            dictionaries->setState({"en-en", "ar-ar"}, true, {});
            dictionaries->setStorage({{"en-en", 92000000}, {"ar-ar", 3500000}}, 1000000000);
            dictionaries->setTransfer("en-fr", 3100000, 6900000, false);
            dictionaries->installRequested = [](const QString &) {};
            dictionaries->removeRequested = [](const QString &) {};
            dictionaries->cancelRequested = [] {};
            capture("dictionaries-book", "rrDictionaries");
            tap("rrDictionaryAction0");
            capture("dictionary-remove-confirmation", "rrDictionaryRemove");
            tap("rrDictionaryRemoveCancel");
            for (int i = 0; i < 3 && widget("rrDictionariesNext")->isEnabled(); ++i)
                tap("rrDictionariesNext");
            capture("dictionaries-other", "rrDictionaries");
            dictionaryManagementFixture = false;
            tap("rrSettingsAdvanced");
            tap("rrRestoreDefaults");
            capture("restore-confirmation", "rrConfirmDefaults");
            tap("rrKeepDefaults");
            tap("rrSettingsDisplay");
            tap("rrScreenRefresh");
            capture("screen-refresh", "rrScreenRefreshPopup");
            return;
        }
        if (group == "embedded-fonts") {
            tap("rrNavText");
            tap("rrFont");
            capture("detected", "rrChoices");
            tap("rrFontChoice0");
            capture("publisher-selected", "rrChoices");
            return;
        }
        if (group == "typography") {
            tap("rrNavText");
            capture("text", "rrFont");
            auto *loading = dynamic_cast<ui::ModalOverlay *>(widget("rrUpdatingText")->parentWidget()->parentWidget());
            require(loading, "Appearance loading modal missing");
            loading->setDelayedProgress(true, 999, 37);
            capture("applying-before-delay", "rrFont");
            loading->setDelayedProgress(true, 1000, 37, strings::get("reader.applying_appearance_changes"));
            capture("applying-progress", "rrApplyingProgress");
            loading->setDelayedProgress(true, 1000, 37, strings::get("reader.enabling_bold_reading"));
            capture("enabling-bionic", "rrApplyingProgress");
            loading->setDelayedProgress(true, 1000, 37, strings::get("reader.disabling_bold_reading"));
            capture("disabling-bionic", "rrApplyingProgress");
            loading->setDelayedProgress(false, 0, 0);
            tap("rrFont");
            const QStringList sections{"RECOMMENDED FONTS", "BUILT-IN FONTS", "SIDELOADED FONTS"};
            for (int i = 0; i < sections.size(); ++i) {
                auto *section = qobject_cast<QLabel *>(widget("rrFontSection"));
                require(section && section->text() == sections[i], "Missing font section: " + sections[i]);
                capture(QString("fonts-%1").arg(i + 1), "rrChoices");
                if (i + 1 < sections.size())
                    tap("rrFontPagerNext");
            }
            while (!qobject_cast<QLabel *>(widget("rrFontPagerPage"))->text().startsWith("1 of "))
                tap("rrFontPagerPrevious");
            // Synthetic names exercise wrapping without installing extra fonts.
            const QStringList longNames{"Atkinson Hyperlegible Next", "Source Serif Pro Semibold",
                                        "OneExtremelyLongUnbrokenFontName"};
            QVector<QPair<ui::SampleLabel *, QString>> originalNames;
            for (int i = 0; i < longNames.size(); ++i) {
                auto *label = dynamic_cast<ui::SampleLabel *>(widget(QString("rrFontChoice%1Title").arg(i + 2)));
                require(label, "Missing long-name font sample");
                originalNames.append({label, label->text()});
                label->setText(longNames[i]);
            }
            capture("fonts-long-names", "rrChoices");
            for (const auto &entry : originalNames)
                entry.first->setText(entry.second);
            tap("rrFontDefaultSlot");
            capture("default-instructions", "rrFontDefaultDialog");
            tap("rrFontDefaultConfirm");
            QWidget *candidate = nullptr;
            for (auto *label : host.window()->findChildren<QLabel *>())
                if (label->objectName().endsWith("Title") && label->text() == "Ancizar Serif")
                    candidate = label->parentWidget();
            require(candidate, "Missing default font candidate");
            QContextMenuEvent event(QContextMenuEvent::Keyboard, candidate->rect().center());
            QApplication::sendEvent(candidate, &event);
            capture("default-confirmation", "rrFontDefaultDialog");
            tap("rrFontDefaultConfirm");
            capture("custom-default", "rrChoices");
            tap("rrFontDefaultSlot");
            capture("custom-default-instructions", "rrFontDefaultDialog");
            tap("rrFontDefaultConfirm");
            tap("rrFontClose");
            tap("rrLarger");
            capture("size-preview", "rrFont");
            tap("rrHeavier");
            capture("weight-preview", "rrFont");
            tap("rrLooser");
            capture("spacing-preview", "rrFont");
            return;
        }
        if (group == "layout" || group == "layout-system-rotation") {
            tap("rrNavLayout");
            capture("layout", "rrMargins");
            for (const auto &part : {"Header", "Footer"}) {
                capture(QString(part).toLower() + "-sizes", "rr" + QString(part) + "TextSize");
                tap("rr" + QString(part) + "TextSize_small");
            }
            tap("rrWider");
            tap("rrVerticalMarginsMore");
            capture("margin-preview", "rrMargins");
            return;
        }
        if (group == "chapters") {
            tap("rrNavChapters");
            for (int i = 0; i < 100; ++i) {
                capture(QString("chapters-%1").arg(i + 1, 2, 10, QChar('0')), "rrContents");
                if (lastListPage("rrContents"))
                    return;
                tap("rrContentsNext");
            }
            throw std::runtime_error("Chapter picker did not reach its last page");
        }
        if (group == "light") {
            tap("rrNavLight");
            capture("frontlight", "rrLightPanel");
            auto *slider = qobject_cast<QSlider *>(widget("rrBrightness"));
            require(slider, "Brightness slider missing");
            const int initial = slider->value();
            tap("rrNavLight");
            pageTap();
            require(!visible("rrControlsTop") && !visible("rrLightPanel"), "Light controls did not close before swipe");
            const auto position = host.session()->state().value("position").toString();
            require(!position.isEmpty(), "Reader position missing before brightness swipe");
            auto *page = widget("rrPage");
            const QPoint start(page->width() / 20, page->height() * 3 / 4);
            const QPoint end(start.x(), page->height() / 4);
            QTest::mousePress(page, Qt::LeftButton, Qt::NoModifier, start);
            QMouseEvent move(QEvent::MouseMove, end, page->mapToGlobal(end), Qt::NoButton, Qt::LeftButton,
                             Qt::NoModifier);
            QApplication::sendEvent(page, &move);
            require(visible("rrBrightnessNotice"), "Brightness swipe indicator did not appear");
            auto *valueLabel = qobject_cast<QLabel *>(widget("rrLightSwipeValue"));
            require(valueLabel, "Brightness percentage missing");
            bool valid = false;
            const int value = valueLabel->text().chopped(1).toInt(&valid);
            require(valid && value > initial && value <= 100,
                    "Brightness swipe did not change the displayed percentage");
            // Keep the pointer down so the hide timer cannot expire during capture.
            capture("brightness-swipe", "rrBrightnessNotice");
            QTest::mouseRelease(page, Qt::LeftButton, Qt::NoModifier, end);
            settled();
            require(host.session()->state().value("position").toString() == position,
                    "Brightness swipe changed reader position");
            menu();
            tap("rrNavLight");
            require(slider->value() > initial, "Brightness swipe did not update the light service");
            tap("rrNavLight");
            pageTap();
            const QPoint warmStart(page->width() * 19 / 20, page->height() / 4);
            const QPoint coolEnd(warmStart.x(), page->height() * 3 / 4);
            QTest::mousePress(page, Qt::LeftButton, Qt::NoModifier, warmStart);
            QMouseEvent coolMove(QEvent::MouseMove, coolEnd, page->mapToGlobal(coolEnd), Qt::NoButton, Qt::LeftButton,
                                 Qt::NoModifier);
            QApplication::sendEvent(page, &coolMove);
            require(visible("rrBrightnessNotice") && valueLabel->text() == "0%",
                    "Warmth swipe did not reach its cool endpoint");
            capture("warmth-swipe-cool", "rrBrightnessNotice");
            QTest::mouseRelease(page, Qt::LeftButton, Qt::NoModifier, coolEnd);
            settled();
            const QPoint warmEnd(coolEnd.x(), coolEnd.y() - page->height() * 8 / 15);
            QTest::mousePress(page, Qt::LeftButton, Qt::NoModifier, coolEnd);
            QMouseEvent warmMove(QEvent::MouseMove, warmEnd, page->mapToGlobal(warmEnd), Qt::NoButton, Qt::LeftButton,
                                 Qt::NoModifier);
            QApplication::sendEvent(page, &warmMove);
            require(visible("rrBrightnessNotice") && valueLabel->text() == "80%",
                    "Warmth swipe did not reach the thick-ray threshold");
            capture("warmth-swipe", "rrBrightnessNotice");
            QTest::mouseRelease(page, Qt::LeftButton, Qt::NoModifier, warmEnd);
            settled();
            require(host.session()->state().value("position").toString() == position,
                    "Warmth swipe changed reader position");
            return;
        }
        if (group == "stats") {
            tap("rrNavStats");
            capture("chapter-graph", "rrChapterGraph");
            return;
        }
        if (group == "search") {
            tap("rrNavSearch");
            capture("empty", "rrSearch");
            auto *query = qobject_cast<QLineEdit *>(widget("rrSearchQuery"));
            require(query, "Search editor missing");
            query->setText("Alice");
            capture("query", "rrSearch");
            tap("rrSearchSubmit");
            waitFor(
                [&] {
                    return visible("rrSearchResult0") && !widget("rrSearchStatus")
                                                              ->property("text")
                                                              .toString()
                                                              .contains("searching", Qt::CaseInsensitive);
                },
                "Search has not finished");
            capture("results", "rrSearch");
            if (widget("rrSearchNext")->isEnabled()) {
                tap("rrSearchNext");
                capture("results-page-2", "rrSearch");
                tap("rrSearchPrevious");
            }
            tap("rrSearchResult0");
            menu();
            tap("rrNavSearch");
            capture("return-and-edit", "rrSearch");
            require(visible("rrSearchOriginal"), "Search original-position action missing");
            query->setText("zzzzmissingword");
            tap("rrSearchSubmit");
            waitFor(
                [&] {
                    return !visible("rrSearchResult0") &&
                           widget("rrSearchStatus")->property("text").toString().contains("No matches");
                },
                "No-result search did not finish");
            capture("no-results", "rrSearch");
            // Reader's first refresh replays icons before its search panel opens.
            // Construct this separate panel to retain the pixels before that replay.
            QWidget surface;
            surface.resize(size);
            surface.setProperty("reader_dark", theme == "dark");
            surface.setPalette(host.window()->palette());
            surface.setFont(ui::interfaceFont());
            ui::readerStyle(&surface, ui::styles::reader());
            SearchPanel probe(&surface, test::hostReaderServices().keyboard);
            probe.setGeometry(surface.rect());
            surface.show();
            probe.open(false, 0);
            auto *probeQuery = probe.findChild<QLineEdit *>("rrSearchQuery");
            auto *clear = probe.findChild<QPushButton *>("rrSearchClear");
            require(probeQuery && clear, "Search clear probe controls missing");
            probeQuery->setText("Alice");
            // Search uses physical reader metrics, unlike the logical Android surface.
            capture("clear-construction", "rrSearchClear", &surface, 1);
            ui::refreshButtonIcon(clear);
            capture("clear-replayed", "rrSearchClear", &surface, 1);
            const auto replayed = clear->grab().toImage();
            ui::refreshButtonIcon(clear);
            require(clear->grab().toImage() == replayed, "Repeated search clear replay changes pixels");
            return;
        }
        if (group == "bookmarks") {
            tap("rrBookmarks");
            capture("empty", "rrBookmarkList");
            tap("rrBookmarkListClose");
            pageTap();
            auto *page = host.window()->findChild<ReadingPage *>("rrPage");
            clickAt(page->mapTo(canvas->surface(), page->bookmarkCorner().center()), page);
            settled();
            capture("ribbon");
            menu();
            tap("rrBookmarks");
            waitFor(
                [&] {
                    return visible("rrBookmark0");
                },
                "Bookmark was not added");
            capture("list", "rrBookmarkList");
            tap("rrBookmarkRemove0");
            capture("remove-confirmation", "rrRemoveBookmark");
            auto savedCount = [&] {
                QDirIterator files(paths.annotations(), {"bookmarks.json"}, QDir::Files, QDirIterator::Subdirectories);
                int count = 0, stores = 0;
                while (files.hasNext()) {
                    const auto saved = storage::readJsonFile(files.next(), 1024 * 1024);
                    require(saved.status == storage::ReadStatus::Ok, "Saved bookmarks are unreadable");
                    count += saved.object.value("bookmarks").toArray().size();
                    ++stores;
                }
                require(stores == 1, "Bookmark tour did not create exactly one isolated store");
                return count;
            };
            require(savedCount() == 1, "Added bookmark was not persisted");
            tap("rrKeepBookmark");
            require(!visible("rrRemoveBookmark") && visible("rrBookmark0") && savedCount() == 1,
                    "Canceling removal changed the bookmark");
            tap("rrBookmarkRemove0");
            tap("rrConfirmRemoveBookmark");
            waitFor(
                [&] {
                    return !visible("rrRemoveBookmark") && !visible("rrBookmark0");
                },
                "Confirmed bookmark removal did not refresh the list");
            require(savedCount() == 0, "Removed bookmark remained in persistent storage");
            tap("rrBookmarkListClose");
            menu();
            tap("rrBookmarks");
            require(!visible("rrBookmark0"), "Removed bookmark returned when reopening the list");
            return;
        }
        if (group == "about") {
            tap("rrSettings");
            tap("rrSettingsAbout");
            tap("rrOpenAcknowledgments");
            capture("acknowledgments", "rrAcknowledgments");
            tap("rrAcknowledgment0");
            capture("license", "rrLicenseText");
            auto *license = dynamic_cast<LicenseText *>(widget("rrLicenseText"));
            require(license && license->pages() > 1 && license->page == 0, "License fixture did not paginate");
            const auto firstLicense = license->grab().toImage();
            const int licenseTurns = checkPagerEnds("rrAcknowledgments", license);
            require(licenseTurns == license->pages() - 1 && license->page == 0 &&
                        license->grab().toImage() == firstLicense,
                    "License paging did not restore the first content");
            tap("rrAcknowledgmentsBack");
            checkPagerEnds("rrAcknowledgments");
            return;
        }
        if (group == "dialogs") {
            pageTap();
            host.session()->go(host.session()->pages() - 1);
            settled();
            auto *page = widget("rrPage");
            QTest::mouseClick(page, Qt::LeftButton, Qt::NoModifier, {size.width() - 80, size.height() / 2});
            waitFor(
                [&] {
                    return visible("rrFinishBook");
                },
                "Finish prompt did not open");
            capture("finish-book", "rrFinishBook");
            tap("rrStillReading");
            settled();
            auto opening = createEmbeddedOpening(host.window(), test::makeHostPresentation);
            opening->window()->resize(size);
            opening->showFallback(OpeningFallback::Failure);
            opening->window()->show();
            capture("book-cannot-open", "rrOpeningFailure");
            return;
        }
        throw std::runtime_error("Unknown group: " + group.toStdString());
    }

private:
    bool dictionaryManagementFixture = false;
    QString output, group, theme;
    QSize size;
    QTemporaryDir temporary;
    storage::Paths paths;
    test::ReaderHost host;
    std::unique_ptr<test::ReaderCanvas> canvas;
    std::shared_ptr<LookupState> lookup;
    std::function<void(bool)> bluetoothChanged;
};
}
int main(int argc, char **argv) {
    test::configureHostPlatform(argc, argv, true);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QApplication::setCursorFlashTime(0);
    test::applyHostStyle();
    QCommandLineParser parser;
    parser.addHelpOption();
    for (const auto &name : {"book", "output", "group", "theme", "width", "height", "only", "ppi"})
        parser.addOption(QCommandLineOption(name, name, "value"));
    parser.process(app);
    qputenv("KOBALT_SCREENSHOT_ONLY", parser.value("only").toUtf8());
    AssetBundle assets(QStringLiteral(KOBALT_ASSET_ROOT), QStringLiteral(KOBALT_VERSION));
    FontCopies copies;
    QTemporaryDir systemFonts, sideloadFonts;
    QTimer::singleShot(0, &app, [&] {
        try {
            require(assets.available() && ui::initializeInterfaceFonts(assets), "Font assets unavailable");
            for (const auto &file : bundledFontFiles(assets))
                QFontDatabase::addApplicationFont(file);
            auto fonts = engineFontInputs(assets, {}, {}, {});
            if (parser.value("group") == "typography") {
                require(systemFonts.isValid() && sideloadFonts.isValid(), "Cannot create isolated font directories");
                // Use real bundled faces outside the recommended collection to exercise each source.
                require(QFile::copy(assets.file("fonts/NotoSerif-Regular.ttf"),
                                    systemFonts.filePath("NotoSerif-Regular.ttf")),
                        "Cannot copy built-in font fixture");
                require(QFile::copy(assets.file("fonts/InstrumentSerif-Regular.ttf"),
                                    sideloadFonts.filePath("InstrumentSerif-Regular.ttf")),
                        "Cannot copy sideloaded font fixture");
                fonts.sources.systemDirectory = systemFonts.path();
                fonts.sources.sideloadDirectory = sideloadFonts.path();
                const auto catalogue = discoverReadingFonts(fonts.sources);
                bool builtIn = false, sideloaded = false;
                for (const auto &font : catalogue) {
                    builtIn |= font.origin == FontOrigin::BuiltIn && font.family == "Noto Serif";
                    sideloaded |= font.origin == FontOrigin::Sideloaded && font.family == "Instrument Serif";
                }
                require(builtIn && sideloaded, "Font fixture sources were not discovered");
            }
            bool densityValid = true;
            const int ppi = parser.isSet("ppi") ? parser.value("ppi").toInt(&densityValid) : 300;
            require(densityValid && ppi > 0, "Density must be positive");
            ui::setDensity(ppi);
            readingPpi.store(ppi);
            QApplication::setFont(ui::interfaceFont());
            {
                Tour tour(parser.value("book"), parser.value("output"), parser.value("group"), parser.value("theme"),
                          fonts, placeholderFontFamily(assets, copies),
                          {parser.value("width").toInt(), parser.value("height").toInt()});
                tour.run();
            }
            app.exit(0);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "CAPTURE FAILED: %s\n", error.what());
            app.exit(1);
        }
    });
    return app.exec();
}
