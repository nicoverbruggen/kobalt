#include <QtGui/private/qeventpoint_p.h>
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/keyboard.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "interfaces/reader/ui/style/scale.h"
#include "interfaces/reader/ui/style/theme.h"
#include "interfaces/reader/views/reading_page.h"
#include "model/page_refresh_regions.h"
#include "interfaces/reader/views/kosync_notice.h"
#include "platforms/desktop/window.h"
#include "platforms/desktop/services.h"
#include "interfaces/reader/views/opening.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "platforms/android/start_screen.h"
#include "platforms/android/canvas.h"
#include "platforms/android/reader.h"
#include "platforms/android/window_geometry.h"
#include <QStackedLayout>
#include <QProgressBar>
#include "platforms/android/boox_frame_queue.h"
#include "interfaces/library/library_view.h"
#include "library/library_store.h"
#include "reading/reading_density.h"
#include "reading/appearance_settings.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include "storage/reading_history.h"
#include "storage/annotation_database.h"
#include <QSqlQuery>
#include <QSqlDatabase>
#include "storage/bookmark_store.h"
#include "storage/highlight_store.h"
#include "publication/zip_resource.h"
#include "build_info.h"
#include "support/book_fixtures.h"
#include "support/engine_fixtures.h"
#include "support/literary_book.h"
#include "support/screenshot_gallery.h"
#include "support/screenshot_runner.h"
#include "support/strings.h"
#include "engine/crengine/reader_model.h"
#include "interfaces/reader/ui/controls/page_scrubber.h"
#include "interfaces/reader/ui/controls/loading.h"
#include "interfaces/reader/settings/dictionaries_page.h"
#include "interfaces/reader/panels/dictionary_panel.h"
#include "interfaces/reader/panels/link_panel.h"
#include "interfaces/reader/panels/choice_screen.h"
#include "interfaces/reader/panels/bookmark_list.h"
#include "interfaces/reader/panels/search_panel.h"
#include "interfaces/reader/panels/chapter_graph.h"
#include "interfaces/reader/settings/header_footer_panel.h"
#include "interfaces/reader/settings/settings_panel.h"
#include "interfaces/reader/settings/acknowledgments.h"
#include "interfaces/reader/ui/controls/paged_text.h"
#include <lvtinydom.h>

#include <QApplication>
#include <QAccessible>
#include <csignal>
#include <cerrno>
#include <QAction>
#include <QCheckBox>
#include <QBuffer>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDynamicPropertyChangeEvent>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGraphicsView>
#include <QLabel>
#include <QLineEdit>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QPainter>
#include <QSaveFile>
#include <QSignalSpy>
#include <QSignalBlocker>
#include <QSet>
#include <QStyleHints>
#include <QMutex>
#include <QTemporaryDir>
#include <QTest>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <QTimer>

#include "engine/crengine/engine_activity.h"
#include "reading/engine_worker.h"
#include "support/qt_compat.h"
#include <QScopeGuard>
#include <QSemaphore>
#include <QThread>
#include <atomic>
#include <memory>

using namespace kobalt;

namespace {
AssetBundle *assets;
FontCopies *copies;
const QString book = QStringLiteral(KOBALT_TEST_BOOK);
QString captureDirectory;
QJsonArray captures;
QJsonObject captureBook;
// Scenes a scene-filtered gallery worker captures; empty captures every scene.
QStringList captureOnly;

EngineFontInputs fixtureFonts(QString sideloadDirectory = {}) {
    // Bundled assets are immutable fixtures. Only each profile's optional
    // sideload directory changes between rows of a test matrix.
    static const auto bundled = engineFontInputs(*assets, {}, {});
    auto inputs = bundled;
    inputs.sources.sideloadDirectory = std::move(sideloadDirectory);
    return inputs;
}
QString fixturePlaceholder() {
    static const auto family = placeholderFontFamily(*assets, *copies);
    return family;
}

class BlockedFile {
public:
    explicit BlockedFile(QString path) : path(std::move(path)), backup(this->path + ".saved") {
        // Kobalt keeps the profile database open. Close it so the next
        // operation meets the blocked path, as it would after USB export.
        storage::closeProfile();
        moved = QFile::rename(this->path, backup);
        blocked = moved && QDir().mkdir(this->path);
    }
    ~BlockedFile() { restore(); }
    bool active() const { return blocked; }
    bool restore() {
        if (blocked && !QDir().rmdir(path))
            return false;
        blocked = false;
        if (moved && !QFile::rename(backup, path))
            return false;
        moved = false;
        return true;
    }
private:
    QString path, backup;
    bool moved = false, blocked = false;
};

using test::writeBook;

bool createBook(const QString &directory, const QString &filename = "text.epub") {
    // A small, offline EPUB with the same text used by the former desktop tests.
    const QString paragraph = "A reader keeps the chapter ready. Changing the size of the words should preserve "
                              "the passage. We can turn the page, visit another chapter, and return to the same place. ";
    QMap<QString, QByteArray> files;
    files["mimetype"] = "application/epub+zip";
    files["META-INF/container.xml"] =
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    QString manifest, spine, navigation;
    const bool illustrated = filename == "later-images.epub";
    const bool nonlinear = filename == "publisher-non-linear.epub";
    const bool vertical = filename == "formatting-unsupported-writing.epub";
    if (illustrated) {
        QImage image(64, 80, QImage::Format_RGB32);
        image.fill(Qt::red);
        QBuffer buffer(&files["image.png"]);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return false;
    }
    for (int chapter = 1; chapter <= 3; ++chapter) {
        const auto name = QString("c%1.xhtml").arg(chapter);
        QString body = QString("<h1>Chapter %1</h1>").arg(chapter);
        if (nonlinear && chapter == 2)
            body += "<p>Nonlinearspine</p>";
        if (illustrated && chapter == 2)
            body += "<p><img src=\"image.png\" alt=\"A red image\"/></p>";
        for (int index = 0; index < 30; ++index) {
            body += "<p>" + paragraph.repeated(3) + "</p>";
        }
        const QString style = vertical ? " style=\"writing-mode:vertical-lr\"" : "";
        files[name] = ("<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chapter</title></head><body" + style + ">" +
                       body + "</body></html>").toUtf8();
        manifest += QString("<item id=\"c%1\" href=\"%2\" media-type=\"application/xhtml+xml\"/>").arg(chapter).arg(name);
        spine += QString("<itemref idref=\"c%1\"%2/>").arg(chapter).arg(nonlinear && chapter == 2 ? " linear=\"no\"" : "");
        navigation += QString("<navPoint id=\"n%1\" playOrder=\"%1\"><navLabel><text>Chapter %1</text>"
                              "</navLabel><content src=\"%2\"/></navPoint>").arg(chapter).arg(name);
    }
    if (illustrated)
        manifest += "<item id=\"image\" href=\"image.png\" media-type=\"image/png\"/>";
    files["book.opf"] =
        ("<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
         "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"uid\">kobalt-fixture</dc:identifier>"
         "<dc:title>Native reader fixture</dc:title><dc:language>en</dc:language><dc:creator>Kobalt</dc:creator></metadata>"
         "<manifest>" + manifest + "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/></manifest>"
         "<spine toc=\"ncx\">" + spine + "</spine></package>").toUtf8();
    files["toc.ncx"] =
        ("<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head>"
         "<meta name=\"dtb:uid\" content=\"kobalt-fixture\"/></head><docTitle><text>Native reader fixture</text>"
         "</docTitle><navMap>" + navigation + "</navMap></ncx>").toUtf8();
    return writeBook(directory, filename, files);
}

// Gallery rows named "-large" capture the light theme with 150% interface
// text, as Large Print or a system font size sets it.
bool largeTextVariant() {
    const char *tag = QTest::currentDataTag();
    return tag && QByteArray(tag).endsWith("-large");
}

// Exercise the reader's production light handlers without platform hardware.
struct GestureLightState {
    bool available = true, warmthSupported = true, failChanges = false, active = false;
    bool automatic = true, originalAutomatic = true;
    int brightness = 40, warmth = 30, maximumWarmth = 100, changes = 0;
    int savedBrightness = -1, savedWarmth = -1, initialBrightness = 40;
};
class GestureLight final : public LightControls {
public:
    explicit GestureLight(std::shared_ptr<GestureLightState> state) : state(std::move(state)) {}
    bool initialize() override { return state->available; }
    bool hasWarmth() const override { return state->warmthSupported; }
    int warmth() const override { return state->warmth; }
    int warmthMinimum() const override { return 0; }
    int warmthMaximum() const override { return state->maximumWarmth; }
    int warmthStep() const override { return 1; }
    bool automaticWarmth() const override { return state->automatic; }
    int setWarmth(int value) override {
        ++state->changes;
        if (state->failChanges) return -1;
        state->automatic = false;
        return state->warmth = value;
    }
    bool saveWarmth() override { state->savedWarmth = state->warmth; return true; }
    bool setAutomaticWarmth(bool value) override { state->automatic = value; return true; }
    bool beginGesture() override {
        if (!initialize()) return false;
        state->active = true;
        state->initialBrightness = state->brightness;
        return true;
    }
    int setGestureLevel(int value) override {
        ++state->changes;
        if (state->failChanges) return -1;
        return state->brightness = qBound(0, value, 100);
    }
    void finishGesture(bool save) override {
        if (save) state->savedBrightness = state->brightness;
        else state->brightness = state->initialBrightness;
        state->active = false;
    }
    int brightness() const override { return state->brightness; }
    bool gesturing() const override { return state->active; }
private:
    std::shared_ptr<GestureLightState> state;
};

class LightFeedbackEvents final : public QObject {
public:
    explicit LightFeedbackEvents(QWidget *reader, QWidget *notice) : notice(notice) {
        reader->installEventFilter(this);
        notice->installEventFilter(this);
    }
    int shows = 0, hides = 0, flashes = 0;
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == notice) {
            shows += event->type() == QEvent::Show;
            hides += event->type() == QEvent::Hide;
        } else if (event->type() == QEvent::DynamicPropertyChange &&
                   static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "reader_full_flash" &&
                   object->property("reader_full_flash").toBool()) {
            ++flashes;
        }
        return false;
    }
private:
    QWidget *notice;
};

struct Fixture {
    QTemporaryDir directory{QDir::tempPath() + "/kobalt-profile-XXXXXX"};
    storage::Paths paths{directory.path()};
    bool prepared = false;
    std::atomic_int clockReads{0};
    const EngineFontInputs fontInputs;
    desktop::Window window;

    // `library` fills the profile with a synthetic library; `reading` marks one book in progress.
    explicit Fixture(bool dark = false, bool variableFont = false, bool library = false, bool reading = true,
                     bool annotationExports = false, bool standaloneSettings = false,
                     std::function<void(ReaderHostServices &)> configure = {})
        : fontInputs(fixtureFonts(variableFont ? directory.path() + "/fonts" : QString())),
          window(prepare(dark, variableFont, library, reading), fontInputs, fixturePlaceholder(),
                 [this] {
                     ++clockReads;
                     return QStringLiteral("09:00");
                 }, [this, annotationExports, standaloneSettings, configure](ReaderHostServices &services) {
                     if (annotationExports)
                         services.annotationExportDirectory = [this] { return paths.directory + "/exports/annotations"; };
                     // Dictionary management belongs to standalone hosts. Its
                     // gallery rows use the same reader sizes without Kobo chrome.
                     if (standaloneSettings) services.devicePreview = [] { return false; };
                     if (configure) configure(services);
                 }) {
        window.resize(920, 800);
        window.show();
        if (largeTextVariant()) {
            auto *textSize = window.findChild<QComboBox *>("desktopTextSizePicker");
            textSize->setCurrentIndex(textSize->findData(150));
        }
    }
    storage::Paths prepare(bool dark, bool variableFont, bool library, bool reading) {
        paths.stateReport = directory.path() + "/state.json";
        if (library)
            prepareLibrary(reading);
        prepared = directory.isValid() && storage::writeJson(paths.preferences(), {{"dark_mode", dark}, {"highlight_help_seen", true}});
        if (variableFont)
            prepared = prepared && QDir().mkpath(directory.path() + "/fonts") &&
                       QFile::copy(QStringLiteral(KOBALT_TEST_VARIABLE_FONT), directory.path() + "/fonts/Amstelvar.ttf");
        return paths;
    }
    // Invented books and authors exercise series, sorting and cover layouts.
    // The folder does not exist, so
    // the start-up check keeps these books instead of rescanning them.
    void prepareLibrary(bool reading) {
        struct Run {
            const char *series, *author;
            QStringList titles;
            QColor colour;
            double megabytes;
        };
        const QVector<Run> runs{
            {"The Neralis Cycle", "Aven Teral",
             {"Lanterns over Neralis", "The Glass Orchard", "A Map of Quiet Rivers", "The Seventh Weather",
              "When the Copper Moons Return"}, QColor("#1e3a5a"), 9},
            {"The Clockwork Marsh", "Mira Vennik",
             {"The Clockwork Marsh", "The Brass Heron", "An Invitation from the Tides", "The Silent Ferry",
              "Twelve Keys for Winter", "The Cartographer's Bell", "Where the Moss Remembers", "A Door beneath the Rain"},
             QColor("#c04a6a"), 1.2},
            {"The Amber Compass", "Oren Kelwick",
             {"North of the Amber Compass", "A Crown of Paper Birds", "The Travelling Harbour",
              "The Last Bridge to Velora", "The Compass Comes Home"}, QColor("#2a3a8a"), 0.8},
            {"Postcards from Tessel", "Sela Dornet",
             {"A Small Trouble in Tessel", "The Mayor's Missing Tuesday", "Tea with the Weather Bureau",
              "The Unlicensed Comet", "A Parcel for Yesterday", "The Museum of Almost Everything",
              "Three Unreasonable Umbrellas", "The Midnight Queue", "The Borrowed Lighthouse", "A Very Local Eclipse",
              "The Committee for Lost Socks", "Tessel at Closing Time"}, QColor("#3a6a2a"), 0.6},
            {"The Veylan Routes", "Jorin Astrel",
             {"Departure from Veylan", "The Empty Transit", "Signals through the Salt", "The Long Way to Oravel",
              "All Routes Lead to Morning"}, QColor("#5a3a1a"), 3},
            {"", "Aven Teral", {"The Orchard beyond the Weather", "A House for Unwritten Letters"}, QColor("#6a2a2a"), 2},
        };
        QDir().mkpath(directory.path() + "/library-covers");
        QJsonArray books;
        int number = 0;
        for (const auto &run : runs)
            for (int i = 0; i < run.titles.size(); ++i, ++number) {
                const auto key = QString("/kobalt-fixture-library/%1.epub").arg(number);
                const auto cover = QString("cover-%1.png").arg(number);
                QImage image(240, 360, QImage::Format_RGB32);
                image.fill(run.colour.lighter(100 + 12 * (i % 4)));
                QPainter painter(&image);
                painter.setPen(Qt::white);
                painter.setFont(QFont(ui::interfaceFont().family(), 22));
                painter.drawText(image.rect().adjusted(16, 16, -16, -16), Qt::AlignTop | Qt::TextWordWrap,
                                 run.titles[i]);
                painter.end();
                image.save(directory.path() + "/library-covers/" + cover);
                QJsonObject book{{"key", key},
                                 {"folder", "/kobalt-fixture-library"},
                                 {"name", run.titles[i] + ".epub"},
                                 {"title", run.titles[i]},
                                 {"author", run.author},
                                 {"cover", cover},
                                 {"size", run.megabytes * (1 + i % 3) * 1024 * 1024},
                                 {"added", double(1700000000000LL + number * 1000)},
                                 {"metadata", 1}};
                if (*run.series) {
                    book["series"] = run.series;
                    book["series_index"] = i + 1;
                }
                // Reading the first series: two finished, the third in progress.
                if (reading && number < 3) {
                    book["opened"] = double(1710000000000LL + number * 1000);
                    book["percent"] = number < 2 ? 100 : 38;
                    book["finished"] = number < 2;
                }
                books.append(book);
            }
        storage::writeJson(directory.path() + "/library.json",
                           QJsonObject{{"schema", 1},
                                       {"folders", QJsonArray{QJsonObject{{"key", "/kobalt-fixture-library"},
                                                                          {"label", "Books"}}}},
                                       {"books", books}});
    }
    ~Fixture() {
        window.close();
        if (!QTest::qWaitFor([this] { return window.idle(); }, 30000)) {
            QTest::qFail("Reader did not drain while closing", __FILE__, __LINE__);
        }
    }
    QJsonObject state() {
        if (auto *reader = window.readerWindow()) {
            reader->checkpoint();
        }
        return storage::readJson(paths.stateReport);
    }
    bool turnPage(int direction) {
        auto *reader = window.readerWindow();
        if (!reader) return false;
        const int previous = state().value("page").toInt();
        // Separate deliberate setup turns from duplicate input rejected by the reader.
        QTest::qWait(210);
        reader->pageButton(direction);
        return QTest::qWaitFor([&] {
            const auto current = state();
            return !current.value("busy").toBool() && current.value("page").toInt() != previous;
        }, 5000);
    }
    bool click(QWidget *target, QPoint position = QPoint(-1, -1)) {
        auto *reader = window.readerWindow();
        auto *canvas = window.findChild<QGraphicsView *>();
        if (reader && target && !target->isVisible() && !revealCardRow(target)) return false;
        // A narrow Settings panel lists its sections, and an open section has
        // a back cell in place of the close cell. Return to the list first.
        if (reader && target && !target->isVisible() &&
            (target->objectName() == "rrSettingsClose" ||
             (target->parentWidget() && target->parentWidget()->objectName() == "rrSettingsRail"))) {
            auto *back = reader->findChild<QPushButton *>("rrSettingsBack");
            if (back && back->isVisible() && !click(back)) return false;
        }
        if (!reader || !canvas || !target || !target->isVisible() || !target->isEnabled()) {
            return false;
        }
        if (position.x() < 0) {
            position = target->rect().center();
            // A taller typography panel can cover the page's center. Use
            // exposed paper to dismiss it, as a person would, and never send
            // a page tap to an unrelated control above it.
            if (target->objectName() == "rrPage") {
                for (int y : {position.y(), target->height() / 3, target->height() / 4}) {
                    const QPoint candidate(position.x(), y);
                    if (reader->childAt(target->mapTo(reader, candidate)) == target) {
                        position = candidate;
                        break;
                    }
                }
                if (reader->childAt(target->mapTo(reader, position)) != target)
                    return false;
            }
        }
        if (!reveal(target)) return false;
        const auto point = canvas->mapFromScene(target->mapTo(reader, position));
        if (!canvas->viewport()->rect().contains(point)) {
            return false;
        }
        QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        return true;
    }
    QWidget *widget(const char *name) {
        auto *reader = window.readerWindow();
        auto *widget = reader ? reader->findChild<QWidget *>(name) : nullptr;
        return widget ? widget : window.findChild<QWidget *>(name);
    }
    // A card taller than the screen pages its rows; turn to the target's page.
    bool revealCardRow(QWidget *target) {
        for (auto *overlay = target->parentWidget(); overlay; overlay = overlay->parentWidget()) {
            auto *modal = dynamic_cast<ui::ModalOverlay *>(overlay);
            if (!modal || !modal->isVisible()) continue;
            const auto name = modal->objectName().isEmpty() ? QStringLiteral("rrCard") : modal->objectName();
            auto *previous = modal->findChild<QPushButton *>(name + "Previous");
            auto *next = modal->findChild<QPushButton *>(name + "Next");
            while (!target->isVisible() && previous && previous->isVisible() && previous->isEnabled())
                if (!click(previous)) return false;
            while (!target->isVisible() && next && next->isVisible() && next->isEnabled())
                if (!click(next)) return false;
            QCoreApplication::processEvents();
            return target->isVisible();
        }
        return true;
    }
    // Pages a sheet until its clip shows the whole target, as a person would.
    bool reveal(QWidget *target) {
        auto *reader = window.readerWindow();
        if (!reader || !target) return false;
        if (!target->isVisible() && !revealCardRow(target)) return false;
        const std::pair<const char *, const char *> pagers[] = {
            {"rrHeaderFooterClip", "rrHeaderFooter"}, {"rrAppearanceClip", "rrAppearance"},
            {"rrSettingsPageClip", "rrSettingsOptions"}};
        for (const auto &[clipName, name] : pagers) {
            auto *clip = reader->findChild<QWidget *>(clipName);
            if (!clip || !clip->isVisible() || !clip->isAncestorOf(target)) continue;
            const auto fits = [&] { return clip->rect().contains(QRect(target->mapTo(clip, QPoint()), target->size())); };
            auto *previous = reader->findChild<QPushButton *>(QString(name) + "Previous");
            auto *next = reader->findChild<QPushButton *>(QString(name) + "Next");
            if (!fits()) {
                while (previous && previous->isVisible() && previous->isEnabled())
                    if (!click(previous)) return false;
                while (!fits() && next && next->isVisible() && next->isEnabled())
                    if (!click(next)) return false;
                if (!fits()) return false;
            }
        }
        return true;
    }
    bool press(const char *name) { return click(widget(name)); }
    bool bookmarkAction(const char *action) {
        return press("rrBookmarkMore0") && QTest::qWaitFor([&] { return visible(action); }, 5000) && press(action);
    }
    bool pressAnnotationExport(const char *action = "rrAnnotationExportHtml") {
        if (!press("rrAnnotationMore")) return false;
        if (!QTest::qWaitFor([this, action] { return visible(action); })) return false;
        return press(action);
    }
    bool drag(QWidget *target, QPoint from, QPoint to, bool holdFirst = false) {
        auto *reader = window.readerWindow();
        auto *canvas = window.findChild<QGraphicsView *>();
        if (!reader || !canvas || !target)
            return false;
        const auto start = canvas->mapFromScene(target->mapTo(reader, from));
        const auto end = canvas->mapFromScene(target->mapTo(reader, to));
        if (!canvas->viewport()->rect().contains(start) || !canvas->viewport()->rect().contains(end))
            return false;
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
        bool recognized = true;
        if (holdFirst)
            recognized = QTest::qWaitFor([this] { return state().value("annotation_dragging").toBool(); }, 2000);
        for (int i = 1; i <= 8; ++i) {
            QTest::mouseMove(canvas->viewport(), start + (end - start) * i / 8);
            QTest::qWait(5);
        }
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, end);
        return recognized;
    }
    bool selectWord(QWidget *target, QPoint position) {
        auto *reader = window.readerWindow();
        auto *canvas = window.findChild<QGraphicsView *>();
        auto *page = target && target->objectName() == "rrPage" ? static_cast<ReadingPage *>(target) : nullptr;
        if (!reader || !canvas || !page)
            return false;
        const auto at = canvas->mapFromScene(target->mapTo(reader, position));
        bool recognized = false;
        auto original = page->annotationDragStarted;
        page->annotationDragStarted = [&](QPoint point, int boundary) {
            recognized = true;
            return original(point, boundary);
        };
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        const bool ready = QTest::qWaitFor([&] { return recognized; }, 2000);
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        page->annotationDragStarted = original;
        return ready;
    }
    bool startAnnotation(QWidget *target, QPoint position) {
        if (visible("rrDictionary") && !press("rrDictionaryClose"))
            return false;
        if (visible("rrAnnotationPopover") && !press("rrAnnotationCancel"))
            return false;
        if (!press("rrPage") || !QTest::qWaitFor([this] { return visible("rrAnnotate"); }, 2000) ||
            !press("rrAnnotate") || !selectWord(target, position))
            return false;
        auto *paper = static_cast<ReadingPage *>(target);
        return QTest::qWaitFor([&] { return visible("rrAnnotationEdit") ||
            (!state().value("annotation_dragging").toBool() &&
             paper->annotationContains(position - QPoint(0, paper->topHeight()))); }, 2000) &&
            (visible("rrAnnotationEdit") || click(target, position));
    }
    bool hold(QWidget *target, QPoint position) {
        return selectWord(target, position) && QTest::qWaitFor([this] { return visible("rrSelectionLink"); }, 2000) &&
            press("rrSelectionLink");
    }
    bool visible(const char *name) {
        const auto *item = widget(name);
        return item && item->isVisible();
    }
    QString storedPath(const QString &filename) {
        if (annotationDatabaseEnabled())
            return paths.database();
        QDirIterator files(paths.annotations(), {filename}, QDir::Files, QDirIterator::Subdirectories);
        return files.hasNext() ? files.next() : QString();
    }
    QJsonArray stored(const QString &filename, const QString &key) {
        if (!annotationDatabaseEnabled())
            return storage::readJson(storedPath(filename)).value(key).toArray();
        const auto current = state();
        const auto identity = current.value("book").toString();
        const auto fingerprint = current.value("fingerprint").toString().toLatin1();
        QJsonArray rows;
        if (key == "highlights") {
            HighlightStore store(paths.annotations(), identity, fingerprint, "Fixture", paths.database());
            if (!store.load()) qFatal("Cannot read saved SQLite highlights");
            for (const auto &row : store.highlights()) rows.append(highlightJson(row));
        } else {
            BookmarkStore store(paths.annotations(), identity, fingerprint, "Fixture", paths.database());
            if (!store.load()) qFatal("Cannot read saved SQLite bookmarks");
            for (const auto &row : store.bookmarks()) rows.append(bookmarkJson(row));
        }
        return rows;
    }
    template<class Result, class Query>
    Result coordinateQuery(const QString &path, Result fallback, Query query) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return fallback;
        const auto current = state();
        const auto size = page->image.size();
        const auto fonts = fontInputs;
        struct Pending {
            Result value;
            std::atomic_bool done{false};
        };
        auto pending = std::make_shared<Pending>();
        pending->value = fallback;
        // Even a separate model shares CREngine's global fonts and caches.
        // Keep its construction, queries and destruction on the reader's queue.
        qt::invoke(engineWorker(), [pending, path, current, size, fonts, query]() mutable {
            {
                EngineJob activity;
                QFile input(path);
                ReaderModel coordinates(fonts);
                QString error;
                if (input.open(QIODevice::ReadOnly) &&
                    coordinates.open(input.readAll(), size, current, &error) &&
                    coordinates.goTo(current.value("position").toString()))
                    pending->value = query(coordinates);
            }
            pending->done.store(true);
        }, Qt::QueuedConnection);
        if (!QTest::qWaitFor([pending] { return pending->done.load(); }, 5000)) {
            QTest::qFail("Coordinate probe did not finish on the engine worker", __FILE__, __LINE__);
            return fallback;
        }
        return pending->value;
    }
    QPoint selectionHandleTarget(QPoint point, bool start) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return {-1, -1};
        const int top = page->topHeight();
        return coordinateQuery(book, QPoint(-1, -1), [point, start, top](ReaderModel &coordinates) {
            const auto token = coordinates.annotationTokenAt(point - QPoint(0, top));
            const auto geometry = coordinates.annotationGeometry(token.start, token.end);
            const auto edge = start ? geometry.start : geometry.end;
            return (edge.first + edge.last) / 2 - edge.outward * ui::px(4) + QPoint(0, top);
        });
    }
    AnnotationPageEdges pageEdges() {
        return coordinateQuery(book, AnnotationPageEdges{}, [](ReaderModel &coordinates) {
            return coordinates.annotationPageEdges();
        });
    }
    QPoint wordPoint(int ordinal = 0) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return {-1, -1};
        const int top = page->topHeight();
        const auto size = page->image.size();
        return coordinateQuery(book, QPoint(-1, -1), [ordinal, top, size](ReaderModel &coordinates) mutable {
            QSet<QString> seen;
            for (int y = size.height() / 3; y < size.height() * 2 / 3; y += 12)
                for (int x = size.width() / 3; x < size.width() * 2 / 3; x += 12) {
                    const auto word = coordinates.wordAt({x, y});
                    if (!word.rectangles.isEmpty() && !seen.contains(word.start)) {
                        seen.insert(word.start);
                        if (ordinal-- == 0)
                            return word.rectangles.first().center() + QPoint(0, top);
                    }
                }
            return QPoint(-1, -1);
        });
    }
    QRect wordRect(const QString &text) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return {};
        const int top = page->topHeight();
        const auto size = page->image.size();
        return coordinateQuery(book, QRect{}, [text, top, size](ReaderModel &coordinates) {
            for (int y = 0; y < size.height(); y += 12)
                for (int x = 0; x < size.width(); x += 16) {
                    const auto word = coordinates.wordAt({x, y});
                    if (word.text == text && !word.rectangles.isEmpty())
                        return word.rectangles.first().translated(0, top);
                }
            return QRect{};
        });
    }
    QPoint linkPoint(const QString &path, const QString &label) {
        auto *page = static_cast<ReadingPage *>(widget("rrPage"));
        if (!page)
            return {-1, -1};
        const int top = page->topHeight();
        return coordinateQuery(path, QPoint(-1, -1), [label, top](ReaderModel &coordinates) {
            for (const auto &link : coordinates.pageLinks())
                if (coordinates.linkDetails(link.source).label == label && !link.rectangles.isEmpty())
                    return link.rectangles.first().center() + QPoint(0, top);
            return QPoint(-1, -1);
        });
    }
};

void variants() {
    QTest::addColumn<int>("preset");
    QTest::addColumn<bool>("dark");
    for (int preset : {0, 1, 2}) {
        for (bool dark : {false, true}) {
            QTest::newRow(qPrintable(QString("%1-%2").arg(preset).arg(dark ? "dark" : "light"))) << preset << dark;
        }
    }
}

// The phone in portrait and in landscape.
bool phonePreset(int preset) {
    return preset == 3 || preset == 5;
}

// Each screen has light and dark captures. E-reader profiles also support
// 150% interface text; the phone profiles support normal text only.
void screenshotVariants() {
    QTest::addColumn<int>("preset");
    QTest::addColumn<bool>("dark");
    for (int preset : {1, 2, 3, 4, 5}) {
        for (bool dark : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(preset).arg(dark ? "dark" : "light"))) << preset << dark;
        if (!phonePreset(preset)) QTest::newRow(qPrintable(QString("%1-large").arg(preset))) << preset << false;
    }
}

QString screenshotTheme(bool dark) {
    return largeTextVariant() ? "large" : dark ? "dark" : "light";
}

QSize screenshotSize(int preset) {
    // The phone keeps its camera band outside the reader in landscape.
    return preset == 2 ? QSize(1264, 1680) : preset == 3 ? QSize(720, 1600) : preset == 4 ? QSize(1448, 1072)
         : preset == 5 ? QSize(1480, 720) : QSize(1072, 1448);
}

// The portrait phone keeps its 60-pixel camera band above the Library, as
// Android keeps its status bar there.
QSize librarySize(int preset) {
    return screenshotSize(preset) - QSize(0, preset == 3 ? 60 : 0);
}

QString screenshotDevice(int preset) {
    return QStringList{"fill", "six-inch", "seven-inch", "phone", "six-inch-landscape", "phone-landscape"}.at(preset);
}

QString screenshotVariant(int preset, bool dark) {
    return screenshotDevice(preset) + '-' + screenshotTheme(dark);
}

bool hasText(const QImage &source) {
    const auto image = source.convertToFormat(QImage::Format_ARGB32);
    int paper = 0, ink = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) != 255) {
                continue;
            }
            paper += qGray(row[x]) > 180;
            ink += qGray(row[x]) < 80;
        }
    }
    return paper > 100 && ink > 100;
}

QRect returnInkBounds(const QImage &image, const QRect &area, bool lightInk) {
    QRect bounds;
    const auto clipped = area.intersected(image.rect());
    for (int y = clipped.top(); y <= clipped.bottom(); ++y)
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const auto pixel = image.pixel(x, y);
            if (qAlpha(pixel) > 128 && (lightInk ? qGray(pixel) > 128 : qGray(pixel) < 128))
                bounds |= QRect(x, y, 1, 1);
        }
    return bounds;
}

bool visibleReturnCaption(const QImage &button, const QLabel *title, const QColor &color) {
    const auto area = title->contentsRect().translated(title->pos()).intersected(button.rect());
    int ink = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            ink += button.pixelColor(x, y) == color;
    return ink > 4;
}

bool writeCaptureRecord() {
    QSaveFile record(captureDirectory + "/part.json");
    const auto json = QJsonDocument(captures).toJson();
    return record.open(QIODevice::WriteOnly) && record.write(json) == json.size() && record.commit();
}

bool capture(Fixture &fixture, const QString &name, QWidget *nativeScreen = nullptr) {
    if (captureDirectory.isEmpty()) return true;
    const auto parts = QRegularExpression("^(fill|six-inch|seven-inch|phone|six-inch-landscape|phone-landscape)-(light|dark|large)-(.*)$").match(name);
    if (!parts.hasMatch()) return false;
    if (!captureOnly.isEmpty() && !captureOnly.contains(parts.captured(3)) &&
        !captureOnly.contains("@" + test::screenshotIdentity(parts.captured(3)).first)) return true;
    // Model state and visibility can settle before queued layouts and proxy
    // cache updates. Deliver those updates before rendering the visible frame.
    QCoreApplication::processEvents();
    const auto destination = captureDirectory + '/' + name + ".png";
    QWidget *surface = nativeScreen ? nativeScreen : fixture.window.readerWindow();
    auto *start = fixture.window.findChild<android::StartScreen *>();
    if (!nativeScreen && start && start->libraryShown()) surface = start->library();
    const int preset = fixture.window.findChild<QComboBox *>("desktopScreenPicker")->currentIndex();
    const QSize expected = screenshotSize(preset);
    // A turned phone letterboxes its camera side, as the phone landscape profile does.
    const QSize turned = preset == 3 ? screenshotSize(5) : expected.transposed();
    // The phone's Library starts below the camera band, as it starts below
    // Android's status bar. The capture shows the band in the Library's paper.
    const int band = start && surface == start->library() ? expected.height() - surface->height() : 0;
    if (!surface || !surface->isVisible() || (preset < 1 || preset > 5) || band < 0 || (band && preset != 3) ||
        (surface->size() + QSize(0, band) != expected && surface->size() != turned)) {
        qCritical() << "Screenshot surface does not have the native reader dimensions:" << name;
        return false;
    }
    for (auto *child : surface->findChildren<QWidget *>()) {
        auto *modal = dynamic_cast<ui::ModalOverlay *>(child);
        if (!modal || !modal->isVisible()) continue;
        auto *card = modal->card();
        if (!surface->rect().contains(QRect(card->mapTo(surface, QPoint()), card->size()))) {
            qCritical() << "Modal card extends outside the captured screen:" << name << modal->objectName();
            return false;
        }
    }
    QImage pixels(surface->size() + QSize(0, band), QImage::Format_ARGB32_Premultiplied);
    pixels.fill(band ? surface->palette().color(QPalette::Window) : QColor(Qt::transparent));
    surface->render(&pixels, QPoint(0, band));
    if (auto *button = surface->findChild<QPushButton *>("rrReturn")) {
        if (button->isVisible() && QRegularExpression("^(six-inch|seven-inch|phone|six-inch-landscape|phone-landscape)-(light|dark|large)-menu(-return-pressed)?$").match(name).hasMatch()) {
            auto *title = button->findChild<QLabel *>("rrReturnTitle");
            const auto at = button->mapTo(surface, QPoint());
            const auto rendered = pixels.copy(QRect(at, button->size()));
            const auto &colors = ui::themeColors(button);
            const bool pressed = button->isDown();
            if (rendered.pixelColor(5, 5) != (pressed ? colors.ink : colors.paper) ||
                !title || !visibleReturnCaption(rendered, title, pressed ? colors.paper : colors.ink)) {
                qCritical() << "Return control is not visible in the composed screenshot:" << name;
                return false;
            }
        }
    }
    // Density describes the saved screen. It must not change Qt's paint-device
    // metrics while rendering the already laid-out production widget.
    pixels.setDotsPerMeterX(qRound(300 / 0.0254));
    pixels.setDotsPerMeterY(qRound(300 / 0.0254));
    if (!pixels.save(destination)) return false;
    const QImage image(destination);
    QFile file(destination);
    if (image.isNull() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto identity = test::screenshotIdentity(parts.captured(3));
    if (test::screenshotTitle(identity.first, captureBook).isEmpty()) {
        qCritical().noquote() << "Screenshot has no gallery title:" << identity.first;
        return false;
    }
    for (const auto &value : captures)
        if (value.toObject()["file"].toString() == name + ".png") return false;
    static const auto generalBook = [] {
        QFile file(book);
        if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};
        return QJsonObject{{"title", "Trevelyan's Type Tester v1.2 with chapter-heading CSS fix"},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex())}};
    }();
    captures.append(QJsonObject{{"file", name + ".png"}, {"width", image.width()}, {"height", image.height()},
        {"device", parts.captured(1)}, {"theme", parts.captured(2)}, {"scene", parts.captured(3)},
        {"screen", identity.first}, {"capture_page", identity.second},
        {"group", test::screenshotGroup(parts.captured(3))}, {"book", captureBook.isEmpty() ? generalBook : captureBook},
        {"capture", "screen"}, {"ppi", 300},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex())},
        // The case lets a scene-filtered run find this scene without running the whole tour.
        {"case", QString::fromLatin1(QTest::currentTestFunction()) + ':' + QString::fromLatin1(QTest::currentDataTag())}});
    if (!captureOnly.isEmpty() && !captureOnly.first().startsWith('@') && captures.size() == captureOnly.size()) {
        // Every selected scene of this case is saved. The rest of the case
        // only leads to scenes that were not selected, so the worker ends here.
        std::fflush(nullptr);
        std::_Exit(writeCaptureRecord() ? 0 : 2);
    }
    return true;
}

void capturePages(Fixture &fixture, const QString &name, const char *prefix, int limit = 20) {
    auto *counter = qobject_cast<QLabel *>(fixture.widget(qPrintable(QString(prefix) + "Page")));
    auto *next = fixture.widget(qPrintable(QString(prefix) + "Next"));
    QSet<QString> seen;
    for (int i = 1; i <= limit; ++i) {
        QCoreApplication::processEvents();
        const auto text = counter ? counter->text() : QString();
        if (seen.contains(text)) return;
        seen.insert(text);
        QVERIFY(capture(fixture, name + (i == 1 ? QString() : '-' + QString::number(i))));
        if (!next || !next->isVisible() || !next->isEnabled()) return;
        QVERIFY(fixture.click(next));
        if (i < limit) QTRY_VERIFY(!counter || counter->text() != text);
    }
    // Font choices deliberately sample three pages. Other inventories must
    // complete a cycle within their guard rather than silently truncate.
    QVERIFY(limit == 3 || !counter || seen.contains(counter->text()));
}

void captureReturnPressed(Fixture &fixture, const QString &name) {
    if (!fixture.visible("rrReturn")) QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrReturn"));
    auto *button = qobject_cast<QPushButton *>(fixture.widget("rrReturn"));
    auto *title = qobject_cast<QLabel *>(fixture.widget("rrReturnTitle"));
    auto *canvas = fixture.window.findChild<QGraphicsView *>();
    QVERIFY(button && title && canvas);
    // Panel close briefly guards the same corner against an accidental exit.
    QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 10);
    const auto point = canvas->mapFromScene(title->mapTo(fixture.window.readerWindow(), title->rect().center()));
    QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTRY_VERIFY(button->isDown());
    QVERIFY(visibleReturnCaption(button->grab().toImage(), title, button->palette().color(QPalette::Window)));
    QVERIFY(capture(fixture, name + "-menu-return-pressed"));
    QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTRY_VERIFY(fixture.window.idle());
}

void advanceIntoBody(Fixture &fixture) {
    // Capture narrative text as well as the chapter-opening typography.
    auto *reader = fixture.window.readerWindow();
    QVERIFY(reader);
    const int chapterStart = fixture.state().value("page").toInt();
    QVERIFY(fixture.turnPage(1));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.state().value("page").toInt() > chapterStart, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    QVERIFY(page && hasText(page->image));
}

// Creation dates are visible in the bookmark list. Store a fixed timestamp,
// then let the app load it normally, so captures match between runs.
void fixBookmarkDates(Fixture &fixture) {
    QSignalSpy reopened(&fixture.window, &desktop::Window::bookOpened);
    fixture.window.readerWindow()->close();
    QTRY_VERIFY(fixture.window.idle());
    const auto current = fixture.state();
    BookmarkStore store(fixture.paths.annotations(), current.value("book").toString(),
                        current.value("fingerprint").toString().toLatin1(), "Fixture", fixture.paths.database());
    QVERIFY(store.load());
    auto entries = store.bookmarks();
    for (auto &entry : entries)
        entry.created = QDateTime::fromString("2000-01-01T12:00:00Z", Qt::ISODate).toUTC();
    store.setBookmarks(entries);
    QVERIFY(store.save());
    fixture.window.openBook(book);
    QTRY_COMPARE(reopened.count(), 1);
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_VERIFY(fixture.state().value("bookmarked").toBool());
}

void openPrologueBody(Fixture &fixture) {
    if (!fixture.visible("rrNavChapters"))
        QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrNavChapters"));
    QVERIFY(fixture.press("rrNavChapters"));
    QTRY_VERIFY(fixture.visible("rrChapter1"));
    QVERIFY(fixture.press("rrChapter1"));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    advanceIntoBody(fixture);
}

bool revealSettingsOption(Fixture &fixture, const char *name) {
    auto *reader = fixture.window.readerWindow();
    auto *option = fixture.widget(name);
    if (!reader || !option)
        return false;
    auto *previous = fixture.widget("rrSettingsOptionsPrevious");
    for (int attempt = 0; previous && previous->isVisible() && previous->isEnabled() && attempt < 12; ++attempt) {
        if (!fixture.click(previous)) return false;
        QCoreApplication::processEvents();
    }
    for (int attempt = 0; attempt < 12; ++attempt) {
        auto *hit = reader->childAt(option->mapTo(reader, option->rect().center()));
        if (hit && (hit == option || option->isAncestorOf(hit)))
            return true;
        if (!fixture.press("rrSettingsOptionsNext"))
            return false;
    }
    return false;
}

void verticalJustificationTour(Fixture &fixture, const QString &name, bool screenshots) {
    const auto scene = [&](const QString &suffix) { return !screenshots || capture(fixture, name + "-vertical-" + suffix); };
    QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
    fixture.window.openBook(book);
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    openPrologueBody(fixture);
    const auto fullProsePage = [&] {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const bool fills = fixture.coordinateQuery(book, false, [](ReaderModel &coordinates) {
                const int page = coordinates.page();
                if (!coordinates.applyAppearance({{"vertical_justification", "off"}, {"vertical_spacing_scope", "all"}})) return false;
                const auto before = coordinates.image(page);
                if (!coordinates.applyAppearance({{"vertical_justification", "expand"}}) || coordinates.image(page) == before) return false;
                return coordinates.applyAppearance({{"vertical_justification", "center"}}) && coordinates.image(page) != before;
            });
            if (fills) return;
            const int previous = fixture.state().value("page").toInt();
            QVERIFY(fixture.turnPage(1));
            QTRY_VERIFY(fixture.state().value("page").toInt() > previous);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
        }
        QFAIL("No full prose page found for the vertical spacing sample");
    };
    fullProsePage();
    const auto before = static_cast<ReadingPage *>(fixture.widget("rrPage"))->image;
    const auto pageCount = fixture.state().value("pages");
    QVERIFY(scene("off"));
    const auto settings = [&](const QString &mode, bool columns, bool config, bool protection = false) {
        if (!fixture.visible("rrSettings")) QVERIFY(fixture.press("rrPage"));
        // A resized canvas can report model state before its controls have
        // reached their new visible positions. Wait before sending the click.
        const auto settingsReady = [&] {
            auto *target = fixture.widget("rrSettings");
            auto *reader = fixture.window.readerWindow();
            auto *canvas = fixture.window.findChild<QGraphicsView *>();
            return target && reader && canvas && target->isVisible() && target->isEnabled() &&
                canvas->viewport()->rect().contains(canvas->mapFromScene(target->mapTo(reader, target->rect().center())));
        };
        QTRY_VERIFY(settingsReady());

        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsStyling"));
        auto reveal = [&](const char *key) {
            QCoreApplication::processEvents();
            // Choice captions can change row heights and repaginate Settings.
            // Start from the first page rather than assuming a forward-only tour.
            for (int i = 0; i < 8; ++i) {
                auto *previous = fixture.widget("rrSettingsOptionsPrevious");
                if (!previous || !previous->isEnabled()) break;
                if (!fixture.press("rrSettingsOptionsPrevious")) return false;
                QCoreApplication::processEvents();
            }
            for (int i = 0; i < 8; ++i) {
                auto *row = fixture.widget(key);
                if (row && row->visibleRegion().contains(row->rect().center())) return true;
                if (!fixture.press("rrSettingsOptionsNext")) return false;
                QCoreApplication::processEvents();
            }
            return false;
        };
        QVERIFY(reveal("rrVerticalSpacing"));
        if (config) QVERIFY(scene("settings"));
        QVERIFY(fixture.press("rrVerticalSpacing"));
        QTRY_VERIFY(fixture.visible("rrVerticalSpacing_off"));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.widget("rrStylingPreview")->property("sample_ready").toBool(), 10000);
        if (config) {
            QVERIFY(scene("choices"));
            QVERIFY(fixture.press("rrVerticalSpacing_center"));
            QVERIFY(scene("choices-center"));
            QVERIFY(fixture.press("rrVerticalSpacing_expand"));
            QVERIFY(scene("choices-expand"));
        }
        QVERIFY(fixture.press(qPrintable("rrVerticalSpacing_" + mode)));
        QVERIFY(fixture.visible("rrVerticalSpacingPopup"));
        QVERIFY(fixture.press("rrVerticalSpacingClose"));
        QTRY_VERIFY(!fixture.visible("rrVerticalSpacingPopup"));
        // Which pages to adjust has its own row, available once a mode is chosen.
        QVERIFY(reveal("rrVerticalSpacingScope"));
        QCOMPARE(fixture.widget("rrVerticalSpacingScope")->isEnabled(), mode != "off");
        if (mode != "off") {
            QVERIFY(fixture.press("rrVerticalSpacingScope"));
            QTRY_VERIFY(fixture.visible("rrVerticalSpacingScope_all"));
            QTRY_VERIFY_WITH_TIMEOUT(fixture.widget("rrStylingPreview")->property("sample_ready").toBool(), 10000);
            QVERIFY(fixture.press(protection ? "rrVerticalSpacingScope_protection" : "rrVerticalSpacingScope_all"));
            if (config) QVERIFY(scene("scope"));
            if (config) {
                QVERIFY(fixture.press("rrVerticalSpacingScope_protection"));
                QVERIFY(scene("scope-protection"));
                QVERIFY(fixture.press(protection ? "rrVerticalSpacingScope_protection" : "rrVerticalSpacingScope_all"));
            }
            QVERIFY(fixture.press("rrVerticalSpacingScopeClose"));
            QTRY_VERIFY(!fixture.visible("rrVerticalSpacingScopePopup"));
        }
        if (columns) {
            QVERIFY(fixture.press("rrSettingsDisplay"));
            QVERIFY(reveal("rrLandscapeColumns"));
            auto *choice = qobject_cast<QCheckBox *>(fixture.widget("rrLandscapeColumns"));
            QVERIFY(choice);
            if (!choice->isChecked()) QVERIFY(fixture.press("rrLandscapeColumns"));
        }
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("vertical_justification").toString(), mode);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("vertical_justification").toString(), mode);
        if (mode != "off")
            QCOMPARE(storage::readJson(fixture.paths.preferences()).value("vertical_spacing_scope").toString(),
                     protection ? QString("protection") : QString("all"));
        if (fixture.visible("rrControlsTop")) QVERIFY(fixture.press("rrPage"));
    };
    settings("expand", false, true);
    QCOMPARE(fixture.state().value("pages"), pageCount);
    QTRY_VERIFY(static_cast<ReadingPage *>(fixture.widget("rrPage"))->image != before);
    QVERIFY(scene("expand"));
    settings("center", false, false);
    QTRY_VERIFY(static_cast<ReadingPage *>(fixture.widget("rrPage"))->image != before);
    QVERIFY(scene("center"));
    const auto word = fixture.wordPoint();
    QVERIFY(word.x() >= 0);
    auto *paper = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    QVERIFY(fixture.selectWord(paper, word));
    QTRY_VERIFY(fixture.visible("rrDictionary") || fixture.visible("rrAnnotationPopover"));
    if (fixture.visible("rrDictionary")) QVERIFY(fixture.press("rrDictionaryClose"));
    else QVERIFY(fixture.press("rrAnnotationCancel"));
    settings("off", true, false);
    auto *rotate = fixture.window.findChild<QAction *>("desktopRotateDevice");
    QVERIFY(rotate);
    if (fixture.window.readerWindow()->width() < fixture.window.readerWindow()->height()) {
        if (rotate->isEnabled()) rotate->trigger();
        else fixture.window.resize(1200, 850);
    }
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_COMPARE(fixture.state().value("columns").toInt(), 2);
    fullProsePage();
    const auto columnsBefore = static_cast<ReadingPage *>(fixture.widget("rrPage"))->image;
    QVERIFY(scene("columns-off"));
    settings("expand", false, false);
    QTRY_VERIFY(static_cast<ReadingPage *>(fixture.widget("rrPage"))->image != columnsBefore);
    QVERIFY(scene("columns-expand"));
    settings("center", false, false);
    QTRY_VERIFY(static_cast<ReadingPage *>(fixture.widget("rrPage"))->image != columnsBefore);
    QVERIFY(scene("columns-center"));
    settings("center", false, false, true);
    QCOMPARE(fixture.state().value("vertical_spacing_scope").toString(), QString("protection"));
}

// The same real-widget tour supplies integration checks and screenshot scenes.
void tour(Fixture &fixture, const QString &name, bool screenshots) {
    const auto scene = [&](const QString &suffix) { return !screenshots || capture(fixture, name + '-' + suffix); };
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrSettings"));
    const bool devicePreview = fixture.window.findChild<QComboBox *>("desktopScreenPicker")->currentIndex() != 0;
    QCOMPARE(fixture.visible("rrMenuClock"), devicePreview);
    QCOMPARE(fixture.visible("rrMenuBattery"), devicePreview);
    QVERIFY(scene("menu"));
    QVERIFY(fixture.press("rrNavChapters"));
    QTRY_VERIFY(fixture.visible("rrContents"));
    QVERIFY(fixture.widget("rrChapter2"));
    if (screenshots) capturePages(fixture, name + "-chapters", "rrContents");
    QVERIFY(fixture.press("rrChapter1"));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    QVERIFY(fixture.state().value("page").toInt() > 0);
    advanceIntoBody(fixture);
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrNavText"));
    QVERIFY(fixture.press("rrNavStats"));
    QTRY_VERIFY(fixture.visible("rrChapterGraph"));
    QVERIFY(scene("stats"));
    QVERIFY(fixture.press("rrNavStats"));
    QTRY_VERIFY(!fixture.visible("rrChapterGraph"));
    QVERIFY(fixture.press("rrNavText"));
    QTRY_VERIFY(fixture.visible("rrFont"));
    QVERIFY(scene("text"));
    QVERIFY(fixture.press("rrFont"));
    QTRY_VERIFY(fixture.visible("rrFontChoice0"));
    if (screenshots) capturePages(fixture, name + "-fonts", "rrFontPager", 3);
    const auto originalTypography = fixture.state();
    const int originalSize = originalTypography.value("font_pixels").toInt();
    if (!screenshots)
        QVERIFY(fixture.press("rrFontChoice1"));
    QVERIFY(fixture.press("rrFontClose"));
    QTRY_VERIFY(!fixture.visible("rrChoices"));
    if (!screenshots) {
        QVERIFY(fixture.press("rrLarger"));
        QVERIFY(fixture.press("rrHeavier"));
        QVERIFY(fixture.press("rrLooser"));
        QVERIFY(fixture.press("rrAlignLeft"));
        QVERIFY(fixture.press("rrHyphenation_on"));
        QVERIFY(!fixture.visible("rrOpticalSize")); // The bundled faces are static.
    }
    QVERIFY(fixture.press("rrNavLayout"));
    QTRY_VERIFY(fixture.visible("rrHeaderFooter"));
    if (!screenshots)
        QVERIFY(fixture.press("rrWider"));
    QVERIFY(scene("layout"));
    QVERIFY(fixture.press("rrHeaderFooter"));
    QTRY_VERIFY(fixture.visible("rrHeaderFooterView"));
    if (screenshots) {
        capturePages(fixture, name + "-header-footer", "rrHeaderFooter");
        while (fixture.visible("rrHeaderFooterPrevious") && fixture.widget("rrHeaderFooterPrevious")->isEnabled())
            QVERIFY(fixture.press("rrHeaderFooterPrevious"));
        QVERIFY(fixture.press("rrHeaderFooterSlot0"));
        QTRY_VERIFY(fixture.visible("rrHeaderFooterChooser"));
        while (fixture.visible("rrHeaderFooterFieldsPrevious") && fixture.widget("rrHeaderFooterFieldsPrevious")->isEnabled())
            QVERIFY(fixture.press("rrHeaderFooterFieldsPrevious"));
        capturePages(fixture, name + "-header-footer-fields", "rrHeaderFooterFields");
        QVERIFY(fixture.press("rrHeaderFooterFieldsClose"));
    } else QVERIFY(scene("header-footer"));
    if (!screenshots)
        QVERIFY(fixture.press("rrHeaderFooterPreset_alternative_a"));
    QVERIFY(fixture.press("rrHeaderFooterClose"));
    QTRY_VERIFY(!fixture.visible("rrHeaderFooterView"));
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    QTRY_VERIFY(!fixture.visible("rrMenu"));
    if (!screenshots) {
        QTRY_VERIFY_WITH_TIMEOUT(fixture.state().value("font_pixels").toInt() > originalSize, 30000);
        QVERIFY(!fixture.state().value("font_face").toString().isEmpty());
        QVERIFY(fixture.state().value("weight_steps").toInt() > originalTypography.value("weight_steps").toInt());
        QVERIFY(fixture.state().value("line_spacing").toInt() > originalTypography.value("line_spacing").toInt());
        QVERIFY(fixture.state().value("margin_pixels").toInt() > originalTypography.value("margin_pixels").toInt());
        QCOMPARE(fixture.state().value("text_alignment").toString(), QString("left"));
        QCOMPARE(fixture.state().value("hyphenation").toString(), QString("on"));
        QCOMPARE(fixture.state().value("header_footer_preset").toString(), QString("alternative_a"));
    }
    auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    QVERIFY(page);
    // Model idleness precedes the UI's appearance-apply continuation. The
    // toolbar disappears and Graph is enabled when reading accepts input.
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.visible("rrControlsTop") && fixture.widget("rrGraph")->isEnabled(), 30000);
    QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.state().value("bookmarked").toBool(), 3000);
    QVERIFY(scene("bookmarked"));
    if (screenshots) {
        fixBookmarkDates(fixture);
        if (QTest::currentTestFailed()) return;
        page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
    }
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrBookmarks"));
    QVERIFY(fixture.press("rrBookmarks"));
    QTRY_VERIFY(fixture.visible("rrBookmark0"));
    QVERIFY(scene("bookmarks"));
    if (screenshots) {
        QVERIFY(fixture.bookmarkAction("rrBookmarkDelete"));
        QTRY_VERIFY(fixture.visible("rrRemoveBookmark"));
        QVERIFY(scene("bookmark-remove"));
        QVERIFY(fixture.press("rrKeepBookmark"));
        QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
    }
    QVERIFY(fixture.press("rrAnnotationsTab"));
    QTRY_VERIFY(fixture.visible("rrBookmarkListEmpty"));
    QVERIFY(fixture.widget("rrAnnotationsTab")->x() < fixture.widget("rrBookmarksTab")->x());
    for (const auto *name : {"rrBookmarksTab", "rrAnnotationsTab"}) {
        auto *heading = fixture.widget(name);
        QVERIFY(heading);
        QCOMPARE(heading->font().family(), ui::brandFont(ui::metrics::chrome::titleText).family());
        QCOMPARE(heading->font().pixelSize(), int(ui::metrics::chrome::titleText));
    }
    QVERIFY(scene("annotations"));
    if (!screenshots) {
        QVERIFY(fixture.widget("rrAnnotationMore")->isVisible());
        QVERIFY(fixture.widget("rrAnnotationMore")->isEnabled());
    }
    QVERIFY(fixture.press("rrBookmarkListClose"));
    if (!fixture.visible("rrSearchOpen"))
        QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrSearchOpen"));
    QVERIFY(fixture.press("rrSearchOpen"));
    QTRY_VERIFY(fixture.visible("rrSearchQuery"));
    auto *query = fixture.window.readerWindow()->findChild<QLineEdit *>("rrSearchQuery");
    QVERIFY(query);
    auto *searchDone = fixture.widget("rrSearch")->findChild<QPushButton *>("widgetKeyboardDone");
    QVERIFY(searchDone && searchDone->isVisible());
    QCOMPARE(searchDone->text(), strings::get("search_panel.search"));
    if (screenshots) QVERIFY(scene("search-empty"));
    query->setText("Trevelyan");
    QVERIFY(fixture.press("rrSearchSubmit"));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.visible("rrSearchResult0"), 30000);
    auto *status = fixture.window.readerWindow()->findChild<QLabel *>("rrSearchStatus");
    QVERIFY(status);
    QTRY_VERIFY_WITH_TIMEOUT(!status->text().contains("Searching", Qt::CaseInsensitive), 30000);
    if (screenshots) {
        capturePages(fixture, name + "-search", "rrSearch", 3);
        query->setText("No matching words in this book xyzzy");
        QVERIFY(fixture.press("rrSearchSubmit"));
        QTRY_VERIFY_WITH_TIMEOUT(!status->text().contains("Searching", Qt::CaseInsensitive), 30000);
        QTRY_VERIFY(!fixture.visible("rrSearchResult0"));
        QVERIFY(scene("search-no-results"));
        query->setText("Trevelyan");
        QVERIFY(fixture.press("rrSearchSubmit"));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.visible("rrSearchResult0"), 30000);
    }
    QVERIFY(fixture.press("rrSearchResult0"));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
    QTRY_VERIFY(!fixture.visible("rrSearch"));
    if (!fixture.visible("rrSettings"))
        QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrSettings"));
    QVERIFY(fixture.press("rrSettings"));
    // A narrow screen opens Settings on its section list.
    QTRY_VERIFY(fixture.visible("rrSettingsPages") || fixture.visible("rrSettingsRail"));
    QVERIFY(scene("settings"));
    if (screenshots) {
        for (const auto *tab : {"Display", "Controls", "Defaults", "Styling", "System", "Kosync"}) {
            QVERIFY(fixture.press(qPrintable(QString("rrSettings") + tab)));
            capturePages(fixture, name + "-settings-" + QString(tab).toLower(), "rrSettingsOptions");
        }
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrTimeRemainingFormat"));
        QVERIFY(fixture.press("rrTimeRemainingFormat"));
        QTRY_VERIFY(fixture.visible("rrTimeRemainingFormatPopup"));
        QVERIFY(scene("settings-time-remaining-format"));
        QVERIFY(fixture.press("rrTimeRemainingFormat_detailed"));
        QTRY_VERIFY(!fixture.visible("rrTimeRemainingFormatPopup"));
        QVERIFY(revealSettingsOption(fixture, "rrSessionTimeout"));
        QVERIFY(fixture.press("rrSessionTimeout"));
        QTRY_VERIFY(fixture.visible("rrSessionTimeoutPopup"));
        capturePages(fixture, name + "-session-timeout", "rrSessionTimeout");
        while (fixture.visible("rrSessionTimeoutPrevious") && fixture.widget("rrSessionTimeoutPrevious")->isEnabled())
            QVERIFY(fixture.press("rrSessionTimeoutPrevious"));
        QVERIFY(fixture.press("rrSessionTimeout_15"));
        QTRY_VERIFY(!fixture.visible("rrSessionTimeoutPopup"));
        QVERIFY(fixture.press("rrSettingsStyling"));
        for (auto *previous = fixture.widget("rrSettingsOptionsPrevious");
             previous && previous->isVisible() && previous->isEnabled();)
            QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
        for (const auto &[row, popup, suffix] : {std::tuple{"rrWordSpacing", "rrWordSpacingPopup", "styling-word-spacing"},
                                                std::tuple{"rrWidowsOrphans", "rrWidowsOrphansPopup", "styling-widows"}}) {
            QVERIFY(revealSettingsOption(fixture, row));
            QVERIFY(fixture.press(row));
            QTRY_VERIFY(fixture.visible(popup));
            QTRY_VERIFY_WITH_TIMEOUT(fixture.widget("rrStylingPreview")->property("sample_ready").toBool(), 10000);
            QVERIFY(scene(suffix));
            if (QString(row) == "rrWidowsOrphans") {
                QVERIFY(fixture.press("rrWidowsOrphans_avoid"));
                QVERIFY(scene("styling-widows-avoid"));
                QVERIFY(fixture.press("rrWidowsOrphans_allow"));
                QVERIFY(scene("styling-widows-allow"));
                QVERIFY(fixture.press("rrWidowsOrphans_publisher"));
            }
            QVERIFY(fixture.press(qPrintable(QString(popup).replace("Popup", "Close"))));
            QTRY_VERIFY(!fixture.visible(popup));
        }
    }
    const bool fill = fixture.window.findChild<QComboBox *>("desktopScreenPicker")->currentIndex() == 0;
    QCOMPARE(fixture.visible("rrSettingsDictionaries"), fill);
    QVERIFY(!fixture.widget("rrSettingsUI"));
    QVERIFY(!fixture.widget("rrHideStatusBar"));
    if (fill) {
        QVERIFY(fixture.press("rrSettingsDictionaries"));
        if (screenshots) {
            // Disk space is a host input, like the clock. Keep it fixed in
            // the screenshot fixture while retaining the real catalogue UI.
            auto *dictionaries = dynamic_cast<reader::DictionariesPage *>(fixture.widget("rrDictionaries"));
            QVERIFY(dictionaries);
            dictionaries->setStorage({}, 32000000000LL);
        }
        QTRY_VERIFY(fixture.visible("rrDictionaryRow0"));
        QTRY_VERIFY(fixture.widget("rrDictionaryRow0")->width() == fixture.widget("rrDictionaryRow0")->parentWidget()->width());
        QVERIFY(fixture.widget("rrDictionaryRow0")->width() > 200);
        QVERIFY(scene("dictionaries"));
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFontSize"));
        QVERIFY(scene("dictionary-ui"));
        QVERIFY(fixture.press("rrDictionaryFontSize"));
        QTRY_VERIFY(fixture.visible("rrDictionaryFontSizePopup"));
        QVERIFY(scene("dictionary-font-sizes"));
        QVERIFY(fixture.press("rrDictionaryFontSize100"));
        QTRY_VERIFY(!fixture.visible("rrDictionaryFontSizePopup"));
    }
    QVERIFY(fixture.press("rrSettingsAbout"));
    QTRY_VERIFY(fixture.visible("rrAboutVersion"));
    if (screenshots) capturePages(fixture, name + "-about", "rrSettingsOptions");
    else QVERIFY(scene("about"));
    if (screenshots) {
        QVERIFY(fixture.press("rrOpenAcknowledgments"));
        QTRY_VERIFY(fixture.visible("rrAcknowledgments"));
        QVERIFY(scene("acknowledgments"));
        QVERIFY(fixture.press("rrAcknowledgment0"));
        QTRY_VERIFY(fixture.visible("rrLicenseText"));
        QVERIFY(scene("license"));
        QVERIFY(fixture.press("rrAcknowledgmentsBack"));
        QVERIFY(fixture.press("rrAcknowledgmentsBack"));
    }
    QVERIFY(fixture.press("rrSettingsClose"));
    QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
    if (screenshots) {
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
    }
    QTRY_VERIFY(fixture.visible("rrAnnotate"));
    QVERIFY(fixture.press("rrAnnotate"));
    QVERIFY(!fixture.visible("rrControlsTop"));
    QVERIFY(!fixture.visible("rrAnnotationHeader"));
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
    QVERIFY(scene("annotating"));
    QVERIFY(fixture.press("rrPage"));
    if (screenshots) {
        const auto word = fixture.wordPoint();
        QVERIFY(word.x() >= 0);
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QVERIFY(scene("annotation-start"));
        QVERIFY(fixture.click(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationStyle_shaded"));
        QVERIFY(scene("annotation-actions"));
        QVERIFY(fixture.press("rrAnnotationStyle_shaded"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        // Persistence finishes before the engine delivers saved markup. Wait
        // for the yellow mark at the selected word before capturing the page.
        const auto savedTintPixels = [&] {
            const auto image = page->snapshot();
            const auto area = QRect(word - QPoint(80, 80), QSize(160, 160)).intersected(image.rect());
            int colored = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    colored += pixel.red() > pixel.blue() + 5 && pixel.green() > pixel.blue() + 5;
                }
            return colored;
        };
        QTRY_VERIFY(savedTintPixels() > 0);
        QVERIFY(scene("annotation-saved"));
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationEdit"));
        QVERIFY(scene("annotation-edit"));
        QVERIFY(fixture.click(page, QPoint(10, page->height() / 2)));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
    }
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrAnnotate"));
    QVERIFY(fixture.press("rrAnnotate"));
    if (screenshots) {
        QTRY_VERIFY(!fixture.visible("rrAnnotationModeNotice"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBookmarks"));
        QVERIFY(fixture.press("rrBookmarks"));
        QTRY_VERIFY(fixture.widget("rrAnnotationMore")->isVisible() &&
                    fixture.widget("rrAnnotationMore")->isEnabled());
        QVERIFY(fixture.widget("rrAnnotationMore")->x() > fixture.widget("rrMarksSort")->x());
        QVERIFY(scene("annotation-export-list"));
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportHtml"));
        QVERIFY(scene("annotation-export-menu"));
        auto *exportAction = qobject_cast<QPushButton *>(fixture.widget("rrAnnotationExportHtml"));
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        QVERIFY(exportAction && canvas);
        const auto exportPoint = canvas->mapFromScene(exportAction->mapTo(fixture.window.readerWindow(), exportAction->rect().center()));
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, exportPoint);
        QTRY_VERIFY(exportAction->isDown());
        QVERIFY(scene("annotation-export-pressed"));
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, exportPoint);
        QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
        QVERIFY(scene("annotation-export-confirmation"));
        QVERIFY(fixture.press("rrAnnotationExportConfirm"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportDone"));
        QVERIFY(scene("annotation-export-saved"));
        QVERIFY(fixture.press("rrAnnotationExportDone"));
    }
}
}

namespace {
void checkHeaderFooterSizes(int preset, bool dark) {
    using namespace headerfooter;
    Fixture fixture(dark);
    QVERIFY(fixture.prepared);
    if (phonePreset(preset)) {
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences[headerfooter::presetKey] = "default";
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
    }
    fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
    QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
    fixture.window.openBook(book);
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
    QTRY_VERIFY(!fixture.state().value("busy").toBool() && !fixture.state().value("preview_busy").toBool());
    openPrologueBody(fixture);
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(fixture.press("rrNavLayout"));
    QVERIFY(fixture.press("rrHeaderFooter"));
    QTRY_VERIFY(fixture.visible("rrHeaderFooterView"));
    auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    auto *header = static_cast<ui::SettingSlider *>(fixture.widget("rrHeaderTextSize"));
    auto *footer = static_cast<ui::SettingSlider *>(fixture.widget("rrFooterTextSize"));
    QVERIFY(header && footer && page);
    // Captions stack above the controls on the narrow portrait phone only.
    if (preset == 3) {
        QVERIFY(fixture.reveal(header));
        QVERIFY(header->width() >= ui::px(400));
        QVERIFY(header->parentWidget()->parentWidget()->property("caption_stacked").toBool());
    }

    const auto captureState = [&](const char *suffix) {
        if (captureDirectory.isEmpty()) return true;
        return capture(fixture, screenshotVariant(preset, dark) + '-' + suffix);
    };
    auto *view = fixture.widget("rrHeaderFooterView");
    auto *map = fixture.widget("rrHeaderFooterCustomMap");
    QVERIFY(map);
    QVERIFY(fixture.reveal(map));
    QVERIFY(map->height() >= map->sizeHint().height());
    QVector<QRect> slotBounds;
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        auto *cell = fixture.widget(qPrintable(QString("rrHeaderFooterSlot%1").arg(slot)));
        QVERIFY(cell && cell->width() >= ui::px(60) && cell->height() >= ui::px(60));
        QVERIFY(map->rect().contains(cell->geometry()));
        for (const auto &other : slotBounds) QVERIFY(!other.intersects(cell->geometry()));
        slotBounds.append(cell->geometry());
    }
    auto *progress = fixture.widget("rrHeaderFooterProgressBar_on");
    QVERIFY(fixture.reveal(progress));
    QVERIFY(view->rect().contains(QRect(progress->mapTo(view, QPoint()), progress->size())));
    QCOMPARE(header->mapTo(view, QPoint()).x(), footer->mapTo(view, QPoint()).x());
    QCOMPARE(header->width(), footer->width());
    auto *divider = fixture.widget("rrHeaderFooterSizeDivider");
    QVERIFY(divider);
    QVERIFY(progress->mapTo(view, progress->rect().bottomLeft()).y() < divider->mapTo(view, QPoint()).y());
    QVERIFY(divider->mapTo(view, QPoint()).y() < header->mapTo(view, QPoint()).y());
    const int initialHeight = view->height();
    for (const auto name : {QString("rrHeaderTextSize"), QString("rrFooterTextSize")}) {
        auto *slider = static_cast<ui::SettingSlider *>(fixture.widget(qPrintable(name)));
        QCOMPARE(slider->value(), smallSize);
        auto *value = qobject_cast<QLabel *>(fixture.widget(qPrintable(name + "Value")));
        QVERIFY(value);
        QCOMPARE(value->text(), strings::get("reader.text_size_value").arg("5.8"));
        QVERIFY(fixture.press(qPrintable(name + "More")));
        QTRY_COMPARE(slider->value(), smallSize + 1);
        QVERIFY(fixture.press(qPrintable(name + "Less")));
        QTRY_COMPARE(slider->value(), smallSize);
        QVERIFY(fixture.press(qPrintable(name + "Less")));
        QTRY_COMPARE(slider->value(), offSize);
        QVERIFY(fixture.press(qPrintable(name + "More")));
        QTRY_COMPARE(slider->value(), smallSize);
        // Tap the actual canvas slider at every named checkpoint, then beyond L.
        const int inset = ui::metrics::control::sliderInset;
        const int span = slider->width() - 2 * inset;
        for (const auto point : {QPoint(1, smallSize), QPoint(2, mediumSize), QPoint(3, largeSize),
                                 QPoint(4, maximumSize)}) {
            QVERIFY(fixture.click(slider, QPoint(inset + qRound(span * point.x() / 4.0), slider->height() / 2)));
            QTRY_COMPARE(slider->value(), point.y());
        }
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && !fixture.state().value("preview_busy").toBool());
    }
    QCOMPARE(page->chrome.headerSize, maximumSize);
    QCOMPARE(page->chrome.footerSize, maximumSize);
    QCOMPARE(page->headerTextPixels(), page->footerTextPixels());
    QCOMPARE(view->height(), initialHeight);
    QVERIFY(page->headerTextRect().height() >= page->headerTextPixels());
    QVERIFY(page->footerTextRect().height() >= page->footerTextPixels());
    QCOMPARE(fixture.state().value("header_footer_preset").toString(), QString("default"));
    auto *customCard = fixture.widget("rrHeaderFooterPreset_default");
    QVERIFY(fixture.reveal(customCard));
    const auto cardImage = customCard->grab().toImage();
    QVERIFY(qobject_cast<QAbstractButton *>(customCard)->isChecked());
    QVERIFY(hasText(cardImage));
    QVERIFY(captureState("header-footer-large"));
    const auto selectSize = [&](ui::SettingSlider *slider, bool maximum) {
        const int inset = ui::metrics::control::sliderInset;
        return fixture.click(slider, QPoint(maximum ? slider->width() - inset : inset, slider->height() / 2));
    };
    QVERIFY(selectSize(header, false));
    QTRY_VERIFY(!page->chrome.headerEnabled);
    QVERIFY(selectSize(footer, false));
    QTRY_VERIFY(!page->chrome.footerEnabled);
    QCOMPARE(page->topHeight(), 0);
    QCOMPARE(page->bottomHeight(), 0);
    QVERIFY(captureState("header-footer-off"));
    const auto savedPreferences = storage::readJson(fixture.paths.preferences());
    const auto saved = savedPreferences.value(textKey).toObject();
    QCOMPARE(saved.value("header_font_size").toInt(), maximumSize);
    QCOMPARE(saved.value("footer_font_size").toInt(), maximumSize);
    QVERIFY(!savedPreferences.contains(customKey));
    // Preset changes preserve independent text sizes and Off states.
    QVERIFY(fixture.press("rrHeaderFooterPreset_alternative_a"));
    QTRY_COMPARE(header->value(), offSize);
    QTRY_COMPARE(footer->value(), offSize);
    QVERIFY(fixture.press("rrHeaderFooterProgressBar_off"));
    QTRY_COMPARE(fixture.state().value("header_footer_preset").toString(), QString("custom"));
    const auto savedCustom = storage::readJson(fixture.paths.preferences()).value(customKey);
    QCoreApplication::processEvents();
    QVERIFY(fixture.press("rrHeaderFooterPreset_default"));
    QTRY_COMPARE(fixture.state().value("header_footer_preset").toString(), QString("default"));
    QTRY_COMPARE(header->value(), offSize);
    QVERIFY(fixture.press("rrHeaderTextSizeMore"));
    QTRY_COMPARE(header->value(), smallSize);
    QVERIFY(!fixture.visible("rrReplaceCustom"));
    QCOMPARE(fixture.state().value("header_footer_preset").toString(), QString("default"));
    QCOMPARE(storage::readJson(fixture.paths.preferences()).value(customKey), savedCustom);
    QVERIFY(selectSize(header, true));
    QTRY_COMPARE(header->value(), maximumSize);
    QVERIFY(selectSize(footer, true));
    QTRY_COMPARE(footer->value(), maximumSize);
    QVERIFY(selectSize(header, false));
    QVERIFY(fixture.press("rrHeaderFooterClose"));
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool() && !fixture.state().value("preview_busy").toBool());
    fixture.window.readerWindow()->close();
    QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
    fixture.window.openBook(book);
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 10000);
    QTRY_VERIFY(!fixture.state().value("busy").toBool() && !fixture.state().value("preview_busy").toBool());
    page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    QVERIFY(!page->chrome.headerEnabled && page->chrome.footerEnabled);
    QCOMPARE(page->chrome.headerSize, maximumSize);
    QCOMPARE(page->chrome.footerSize, maximumSize);
}
}

void checkPinnedBookmarks(int preset, bool dark) {
    Fixture fixture(dark);
    QVERIFY(fixture.prepared);
    fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
    const auto name = screenshotVariant(preset, dark);
    QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
    fixture.window.openBook(book);
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    openPrologueBody(fixture);
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(fixture.press("rrBookmarks"));
    QVERIFY(fixture.press("rrAnnotationMore"));
    QTRY_VERIFY(fixture.visible("rrBookmarkAddCurrent"));
    QVERIFY(capture(fixture, name + "-bookmark-page-add-menu"));
    QVERIFY(fixture.click(fixture.widget("rrBookmarkActions"), QPoint(5, 5)));
    QTRY_VERIFY(!fixture.widget("rrBookmarkActions"));
    QVERIFY(fixture.press("rrBookmarkListClose"));
    QVERIFY(fixture.press("rrPage"));

    QString firstPosition;
    for (int i = 0; i < 9; ++i) {
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), i + 1);
        QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").size(), i + 1);
        if (i == 0) {
            QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
            QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
            QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").size(), 0);
            QVERIFY(!fixture.visible("rrRemoveBookmark"));
            QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
            QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").size(), 1);
            firstPosition = fixture.stored("bookmarks.json", "bookmarks").first().toObject()["position"].toString();
            fixBookmarkDates(fixture);
        }
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrBookmarks"));
        QTRY_VERIFY(fixture.visible("rrBookmarkList"));
        auto *list = static_cast<BookmarkList *>(fixture.widget("rrBookmarkList"));
        list->setSortOrder("newest");
        QTRY_VERIFY(fixture.visible("rrBookmarkMore0"));
        if (i == 1) QVERIFY(capture(fixture, name + "-pinned-bookmark-list"));
        if (i == 0) {
            QVERIFY(fixture.press("rrAnnotationMore"));
            QTRY_VERIFY(fixture.visible("rrBookmarkRemoveCurrent"));
            QVERIFY(capture(fixture, name + "-bookmark-page-remove-menu"));
            QVERIFY(fixture.click(fixture.widget("rrBookmarkActions"), QPoint(5, 5)));
            QTRY_VERIFY(!fixture.widget("rrBookmarkActions"));
        }
        QVERIFY(fixture.press("rrBookmarkMore0"));
        QTRY_VERIFY(fixture.visible("rrBookmarkRowActions"));
        QVERIFY(fixture.widget("rrBookmarkDelete")->height() >= int(ui::metrics::chrome::menuActionHeight));
        QVERIFY(fixture.press("rrBookmarkPin"));
        if (i == 8) {
            QTRY_VERIFY(fixture.visible("rrPinLimit"));
            QVERIFY(capture(fixture, name + "-pinned-bookmark-limit"));
            QVERIFY(fixture.press("rrPinLimitClose"));
            QVERIFY(!fixture.visible("rrBookmarkRowActions"));
            QVERIFY(fixture.press("rrBookmarkListClose"));
            break;
        }
        QTRY_VERIFY(fixture.visible("rrPinEditor"));
        auto *input = qobject_cast<QLineEdit *>(fixture.widget("rrPinName"));
        QVERIFY(input);
        if (i == 0) {
            input->setText("Cancelled name");
            QVERIFY(fixture.press("rrPinCancel"));
            QTRY_VERIFY(!fixture.visible("rrPinEditor"));
            QVERIFY(!fixture.visible("rrBookmarkRowActions"));
            QVERIFY(fixture.stored("bookmarks.json", "bookmarks").first().toObject()["icon"].toString().isEmpty());
            QVERIFY(fixture.bookmarkAction("rrBookmarkPin"));
            QTRY_VERIFY(fixture.visible("rrPinName"));
            input = qobject_cast<QLineEdit *>(fixture.widget("rrPinName"));
        }
        input->setText(i == 0 ? "Map" : QString("Reference %1").arg(i + 1));
        const auto token = ui::bookmarkIconNames()[i];
        QVERIFY(!ui::controlIcon(ui::bookmarkIcon(token)).isNull());
        // A short screen shows fewer icons per page.
        const auto iconName = ("rrPinIcon_" + token).toLatin1();
        for (int turn = 0; turn < 4 && !fixture.visible(iconName.constData()); ++turn)
            QVERIFY(fixture.press("rrPinIconsNext"));
        QVERIFY(fixture.press(iconName.constData()));
        if (i == 0) QVERIFY(capture(fixture, name + "-pinned-bookmark-editor"));
        QVERIFY(fixture.press("rrPinSave"));
        QTRY_VERIFY(!fixture.visible("rrPinEditor"));
        QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").last().toObject()["icon"].toString(), token);
        if (i == 0) {
            QVERIFY(fixture.bookmarkAction("rrBookmarkDelete"));
            QTRY_VERIFY(fixture.visible("rrRemoveBookmark"));
            QVERIFY(qobject_cast<QLabel *>(fixture.widget("rrRemoveBookmarkDetail"))->text().contains(strings::get("bookmarks.remove_pinned_description")));
            QVERIFY(fixture.press("rrKeepBookmark"));
            QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
        }
        QVERIFY(fixture.press("rrBookmarkListClose"));
        QTRY_VERIFY(fixture.visible("rrPinnedBookmarks"));
        auto *bar = fixture.widget("rrPinnedBookmarks");
        auto *first = fixture.widget("rrPinnedBookmark0");
        auto *last = fixture.widget(QString("rrPinnedBookmark%1").arg(i).toLatin1().constData());
        // A short screen fills a second column; the first column stays the tallest.
        int lowest = 0;
        for (int pin = 0; pin <= i; ++pin) {
            const auto *button = fixture.widget(QString("rrPinnedBookmark%1").arg(pin).toLatin1().constData());
            if (button->x() == first->x()) lowest = qMax(lowest, button->y() + button->height());
        }
        QCOMPARE(first->y(), bar->height() - lowest);
        const int cell = first->width() + 2 * ui::px(12);
        const int columnStep = cell + ui::px(16);
        QVERIFY(last->x() >= first->x() && (last->x() - first->x()) % columnStep == 0 && last->x() + cell <= bar->width() + first->x());
        if (last->x() > first->x()) {
            int firstRows = 0, lastRows = 0;
            for (int pin = 0; pin <= i; ++pin) {
                const auto *button = fixture.widget(QString("rrPinnedBookmark%1").arg(pin).toLatin1().constData());
                firstRows += button->x() == first->x();
                lastRows += button->x() == last->x();
                QVERIFY(bar->rect().contains(button->geometry()));
            }
            QVERIFY(qAbs(firstRows - lastRows) <= 1);
            QWidget backdrop(fixture.window.readerWindow());
            backdrop.setGeometry(bar->geometry());
            backdrop.setStyleSheet("background:#515151;");
            backdrop.show();
            bar->raise();
            QCoreApplication::processEvents();
            const auto frame = fixture.window.readerWindow()->grab().toImage();
            const QPoint between(first->x() - ui::px(12) + cell + ui::px(8), bar->height() / 2);
            QCOMPARE(frame.pixelColor(bar->pos() + between), QColor("#515151"));
            backdrop.hide();
        }
        QCOMPARE(bar->geometry().right(), fixture.window.readerWindow()->width() - 1 - ui::px(20));
        if (i == 0) {
            QCOMPARE(first->accessibleName(), QString("Map"));
            // Rounded corners must reveal the page, including over images.
            QWidget backdrop(fixture.window.readerWindow());
            backdrop.setGeometry(bar->geometry());
            backdrop.setStyleSheet("background:#515151;");
            backdrop.show();
            bar->raise();
            QCoreApplication::processEvents();
            const auto frame = fixture.window.readerWindow()->grab().toImage();
            QCOMPARE(frame.pixelColor(bar->pos() + QPoint(2, 2)), QColor("#515151"));
            backdrop.hide();

            QVERIFY(capture(fixture, name + "-pinned-bookmark-single"));
            QVERIFY(fixture.press("rrNavStats"));
            QTRY_VERIFY(fixture.visible("rrChapterGraph"));
            QVERIFY(!fixture.visible("rrPinnedBookmarks"));
            QVERIFY(fixture.press("rrNavStats"));
            QTRY_VERIFY(fixture.visible("rrPinnedBookmarks"));
        }
        const int navigationTop = fixture.widget("rrNavText")->mapTo(fixture.window.readerWindow(), QPoint()).y();
        QVERIFY2(bar->geometry().bottom() < navigationTop, "Bookmark shortcuts overlap the navigation tabs");
        if (i == 3) QVERIFY(capture(fixture, name + "-pinned-bookmark-four"));
        if (i == 7) QVERIFY(capture(fixture, name + "-pinned-bookmark-eight"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrPinnedBookmarks"));
        advanceIntoBody(fixture);
    }
    const auto readingOrigin = fixture.state().value("position");
    QVERIFY(fixture.press("rrPinnedBookmark0"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_COMPARE(fixture.state().value("position").toString(), firstPosition);
    for (int shortcut : {2, 4}) {
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrPinnedBookmarks"));
        const auto destination = fixture.widget(QString("rrPinnedBookmark%1").arg(shortcut).toLatin1().constData())
                                     ->property("bookmark_position").toString();
        QVERIFY(fixture.press(QString("rrPinnedBookmark%1").arg(shortcut).toLatin1().constData()));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("position").toString(), destination);
    }
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(fixture.press("rrNavSeek"));
    QTRY_VERIFY(fixture.visible("rrScrubberReturn"));
    QVERIFY(fixture.press("rrScrubberReturn"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_COMPARE(fixture.state().value("position"), readingOrigin);
    advanceIntoBody(fixture);
    const auto nextOrigin = fixture.state().value("position");
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrPinnedBookmarks"));
    QVERIFY(fixture.press("rrPinnedBookmark0"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_COMPARE(fixture.state().value("position").toString(), firstPosition);
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(fixture.press("rrNavSeek"));
    QTRY_VERIFY(fixture.visible("rrScrubberReturn"));
    QVERIFY(fixture.press("rrScrubberReturn"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_COMPARE(fixture.state().value("position"), nextOrigin);

    QVERIFY(!fixture.visible("rrPinnedBookmarks"));
    fixture.window.readerWindow()->close();
    QTRY_VERIFY(fixture.window.idle());
    // Persist a creation order opposite to book order before reopening.
    const auto current = fixture.state();
    BookmarkStore reordered(fixture.paths.annotations(), current.value("book").toString(),
                            current.value("fingerprint").toString().toLatin1(), "Fixture", fixture.paths.database());
    QVERIFY(reordered.load());
    auto entries = reordered.bookmarks();
    QStringList orderedPins;
    for (const auto &entry : entries)
        if (entry.pinned()) orderedPins.append(entry.position);
    std::reverse(entries.begin(), entries.end());
    reordered.setBookmarks(entries);
    QVERIFY(reordered.save());
    fixture.window.openBook(book);
    QTRY_COMPARE(opened.count(), 3); // Includes the fixed-date reopening.
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrPinnedBookmark7"));
    for (int i = 0; i < orderedPins.size(); ++i)
        QTRY_COMPARE(fixture.widget(QString("rrPinnedBookmark%1").arg(i).toLatin1().constData())
                     ->property("bookmark_position").toString(), orderedPins[i]);

    QVERIFY(fixture.press("rrBookmarks"));
    auto *list = static_cast<BookmarkList *>(fixture.widget("rrBookmarkList"));
    list->setSortOrder("position");
    QVERIFY(fixture.press("rrBookmarkMore0"));
    QTRY_VERIFY(fixture.visible("rrBookmarkPin"));
    QVERIFY(capture(fixture, name + "-pinned-bookmark-actions"));
    QVERIFY(fixture.press("rrBookmarkPin"));
    QTRY_VERIFY(fixture.visible("rrPinName"));
    QVERIFY(!fixture.widget("rrPinRemove"));
    QVERIFY(capture(fixture, name + "-pinned-bookmark-edit"));
    qobject_cast<QLineEdit *>(fixture.widget("rrPinName"))->setText(QString::fromUtf8("Carte 日本語"));
    // A pending bookmark refresh may raise the list behind an open editor.
    list->raise();
    QCoreApplication::processEvents();
    auto *icon = fixture.widget("rrPinIcon_compass");
    auto *reader = fixture.window.readerWindow();
    QCOMPARE(reader->childAt(icon->mapTo(reader, icon->rect().center())), icon);
    QVERIFY(fixture.press("rrPinIcon_compass"));
    QVERIFY(fixture.press("rrPinSave"));
    QTRY_VERIFY(!fixture.visible("rrPinEditor"));
    QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").last().toObject()["name"].toString(), QString::fromUtf8("Carte 日本語"));
    QVERIFY(fixture.bookmarkAction("rrBookmarkPin"));
    QTRY_VERIFY(fixture.visible("rrPinIconsNext"));
    auto *pinKeyboard = fixture.widget("rrPinEditor")->findChild<QWidget *>("widgetKeyboard");
    QVERIFY(pinKeyboard && !pinKeyboard->isVisible());
    QVERIFY(!fixture.widget("rrPinIconsPrevious")->isEnabled());
    // Four pages of twelve icons; a short screen shows six per page.
    const int iconPages = preset == 5 ? 8 : 4;
    for (int iconPage = 2; iconPage <= iconPages; ++iconPage) {
        QVERIFY(fixture.press("rrPinIconsNext"));
        QTRY_COMPARE(qobject_cast<QLabel *>(fixture.widget("rrPinIconsPage"))->text(), QString("%1 of %2").arg(iconPage).arg(iconPages));
        // Every profile has cards for pages 2 to 4; the short screen's later pages are only checked.
        if (iconPage <= 4) QVERIFY(capture(fixture, name + QString("-pinned-bookmark-icons-page%1").arg(iconPage)));
    }
    QVERIFY(fixture.visible("rrPinIcon_diamond"));
    QVERIFY(!fixture.widget("rrPinIconsNext")->isEnabled());
    QVERIFY(fixture.press("rrPinIcon_diamond"));
    QVERIFY(fixture.press("rrPinIconsPrevious"));
    QVERIFY(fixture.press("rrPinIconsNext"));
    QVERIFY(qobject_cast<QPushButton *>(fixture.widget("rrPinIcon_diamond"))->isChecked());
    auto *nameInput = fixture.widget("rrPinName");
    QCOMPARE(reader->childAt(nameInput->mapTo(reader, nameInput->rect().center())), nameInput);
    QVERIFY(fixture.press("rrPinName"));
    QTRY_VERIFY(pinKeyboard->isVisible());
    QTRY_VERIFY(!fixture.visible("rrPinIconSection"));
    QVERIFY(capture(fixture, name + "-pinned-bookmark-name"));
    QVERIFY(fixture.click(pinKeyboard->findChild<QPushButton *>("widgetKeyboardDone")));
    QTRY_VERIFY(!pinKeyboard->isVisible());
    QTRY_VERIFY(fixture.visible("rrPinIcon_diamond"));
    QVERIFY(qobject_cast<QPushButton *>(fixture.widget("rrPinIcon_diamond"))->isChecked());
    QVERIFY(fixture.press("rrPinSave"));
    QTRY_VERIFY(!fixture.visible("rrPinEditor"));
    QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").last().toObject()["icon"].toString(), QString("diamond"));
    QVERIFY(fixture.bookmarkAction("rrBookmarkPin"));
    QTRY_VERIFY(fixture.visible("rrPinIcon_diamond"));
    QVERIFY(qobject_cast<QPushButton *>(fixture.widget("rrPinIcon_diamond"))->isChecked());
    QVERIFY(fixture.press("rrPinIconsPrevious"));
    QVERIFY(fixture.press("rrPinCancel"));
    QTRY_VERIFY(!fixture.visible("rrPinEditor"));
    QCOMPARE(fixture.stored("bookmarks.json", "bookmarks").last().toObject()["icon"].toString(), QString("diamond"));

    QVERIFY(fixture.press("rrBookmarkListClose"));
    QTRY_VERIFY(fixture.visible("rrPinnedBookmark0"));
    QVERIFY(fixture.press("rrPinnedBookmark0"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_COMPARE(fixture.state().value("position").toString(), firstPosition);
    auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    QTRY_VERIFY(page->bookmarked);
    const auto beforeRemoval = fixture.stored("bookmarks.json", "bookmarks");
    QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
    QTRY_VERIFY(fixture.visible("rrRemoveBookmark"));
    QCOMPARE(qobject_cast<QLabel *>(fixture.widget("rrRemoveBookmarkPrompt"))->text(),
             strings::get("bookmarks.remove_pinned_prompt"));
    QVERIFY(qobject_cast<QLabel *>(fixture.widget("rrRemoveBookmarkDetail"))->text()
                .contains(strings::get("bookmarks.remove_pinned_description")));
    QVERIFY(capture(fixture, name + "-pinned-bookmark-remove"));
    QCOMPARE(fixture.stored("bookmarks.json", "bookmarks"), beforeRemoval);
    QVERIFY(fixture.press("rrKeepBookmark"));
    QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
    QVERIFY(page->bookmarked);
    QVERIFY(!fixture.visible("rrBookmarkList"));
    QCOMPARE(fixture.stored("bookmarks.json", "bookmarks"), beforeRemoval);
    QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
    QTRY_VERIFY(fixture.visible("rrRemoveBookmark"));
    QVERIFY(fixture.press("rrConfirmRemoveBookmark"));
    QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
    QTRY_VERIFY(!page->bookmarked);
    QVERIFY(!fixture.visible("rrBookmarkList"));
    auto remaining = beforeRemoval;
    for (int i = 0; i < remaining.size(); ++i)
        if (remaining[i].toObject()["position"].toString() == firstPosition) {
            remaining.removeAt(i);
            break;
        }
    QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks"), remaining);
}

void checkPageAnimationCompatibility(int preset, bool dark);

void checkImageViewer(int preset, bool dark, bool screenshots) {
    Fixture fixture(dark);
    QVERIFY(fixture.prepared);
    fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
    QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
    fixture.window.openBook(book);
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
    auto *view = fixture.window.findChild<QGraphicsView *>();
    QVERIFY(page && view);
    const auto name = screenshotVariant(preset, dark);
    const auto position = fixture.state().value("page");
    const auto nativePosition = fixture.state().value("position");
    const auto imagePoint = page->image.rect().center();
    QVERIFY(pageImageAtPoint(page->image, imagePoint));
    const auto at = view->mapFromScene(page->mapTo(fixture.window.readerWindow(), imagePoint + QPoint(0, page->topHeight())));
    const auto openImage = [&] {
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        const bool visible = QTest::qWaitFor([&] { return fixture.visible("rrImageViewer"); }, 2000);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        return visible;
    };
    QVERIFY(!pageSingleImage(page->image).isEmpty());
    QVERIFY(!fixture.visible("rrScrubberImage"));
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrScrubberImage"));
    QCOMPARE(qobject_cast<QPushButton *>(fixture.widget("rrScrubberImage"))->text(), QString("Zoom image"));
    QVERIFY(fixture.press("rrScrubberImage"));
    QTRY_VERIFY(fixture.visible("rrImageViewer"));
    auto *image = fixture.widget("rrImageCanvas");
    QVERIFY(image);
    QTRY_VERIFY(image->property("imageReady").toBool());
    QCOMPARE(image->property("imageSize").toSize(), QSize(600, 851));
    QCOMPARE(image->property("imageZoom").toDouble(), 1.0);
    // Check the printed cover, not only its dimensions or its paper colour.
    const auto pixels = image->grab().toImage();
    int darkPixels = 0;
    for (int y = pixels.height() / 4; y < pixels.height() / 2; ++y)
        for (int x = pixels.width() / 4; x < pixels.width() * 3 / 4; ++x) {
            const auto color = pixels.pixel(x, y);
            darkPixels += qRed(color) < 80 && qGreen(color) < 80 && qBlue(color) < 80;
        }
    QVERIFY2(darkPixels > 1000, "The image viewer lost the cover's printed content");
    auto *viewer = fixture.widget("rrImageViewer");
    auto *header = fixture.widget("rrImageHeader");
    auto *controls = fixture.widget("rrImageControls");
    auto *fit = qobject_cast<QPushButton *>(fixture.widget("rrImageFit"));
    auto *title = qobject_cast<QLabel *>(fixture.widget("rrImageTitle"));
    QVERIFY(viewer && header && controls && fit && title);
    QCOMPARE(title->text(), QString("Image Viewer"));
    QCOMPARE(title->font().family(), ui::brandFont(ui::metrics::chrome::titleText).family());
    QCOMPARE(title->font().pixelSize(), int(ui::metrics::chrome::titleText));
    QVERIFY(!title->font().bold());
    QCOMPARE(title->alignment(), Qt::Alignment(Qt::AlignCenter));
    QVERIFY(qAbs(title->geometry().center().x() - header->rect().center().x()) <= 1);
    auto *zoomOut = fixture.widget("rrImageZoomOut");
    auto *zoomIn = fixture.widget("rrImageZoomIn");
    QVERIFY(!zoomOut->isEnabled());
    QVERIFY(zoomIn->isEnabled());
    QVERIFY(fit->text().isEmpty());
    QCOMPARE(fit->accessibleName(), strings::get("image.fit"));
    QCOMPARE(fit->x(), 0);
    QCOMPARE(fit->width(), zoomIn->width());
    QCOMPARE(zoomOut->geometry().right() + 1, zoomIn->x());
    QCOMPARE(zoomIn->geometry().right(), controls->width() - 1);
    const int rule = ui::metrics::control::border;
    for (const auto *name : {"rrImageZoomOut", "rrImageFit", "rrImageZoomIn"}) {
        const auto *button = fixture.widget(name);
        QCOMPARE(button->y(), rule);
        QCOMPARE(button->height(), controls->height() - rule);
    }
    const auto checkRules = [&] {
        QImage composed(viewer->size(), QImage::Format_RGB32);
        composed.fill(Qt::magenta);
        viewer->render(&composed);
        const auto ink = ui::themeColors(viewer).ink;
        for (int x = 0; x < composed.width(); ++x) {
            QCOMPARE(composed.pixelColor(x, header->geometry().bottom()), ink);
            QCOMPARE(composed.pixelColor(x, controls->y()), ink);
        }
    };
    checkRules();
    const auto fitPoint = view->mapFromScene(fit->mapTo(fixture.window.readerWindow(), fit->rect().center()));
    QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, fitPoint);
    QVERIFY(fit->isDown());
    const auto pressed = fit->grab().toImage();
    const auto ink = ui::themeColors(fit).ink;
    for (const auto &corner : {QPoint(2, 2), QPoint(pressed.width() - 3, 2),
                               QPoint(2, pressed.height() - 3), QPoint(pressed.width() - 3, pressed.height() - 3)})
        QCOMPARE(pressed.pixelColor(corner), ink);
    checkRules();
    QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, fitPoint);
    QVERIFY(!fixture.visible("rrAnnotationPopover"));
    if (screenshots) QVERIFY(capture(fixture, name + "-image-viewer-fit"));
    QVERIFY(fixture.press("rrImageZoomIn"));
    QVERIFY(fixture.press("rrImageZoomIn"));
    QVERIFY(fixture.press("rrImageZoomIn"));
    QVERIFY(image->property("imageZoom").toDouble() > 1.9);
    const auto center = image->rect().center();
    QVERIFY(fixture.drag(image, center, center + QPoint(100, 100)));
    QVERIFY(image->property("imageOffset").toPointF().manhattanLength() > 50);
    QCOMPARE(image->property("imagePreviewOffset"), image->property("imageOffset"));
    if (screenshots) QVERIFY(capture(fixture, name + "-image-viewer-zoomed"));
    const auto enabledPlus = zoomIn->grab().toImage();
    for (int i = 0; i < 20 && zoomIn->isEnabled(); ++i)
        QVERIFY(fixture.press("rrImageZoomIn"));
    QCOMPARE(image->property("imageZoom").toDouble(), 8.0);
    QVERIFY(!zoomIn->isEnabled());
    QVERIFY(zoomOut->isEnabled());
    QVERIFY(zoomIn->grab().toImage() != enabledPlus);
    QVERIFY(fixture.press("rrImageZoomOut"));
    QVERIFY(zoomIn->isEnabled());
    fixture.window.readerWindow()->pageButton(1);
    QCOMPARE(fixture.state().value("page"), position);
    const auto rememberedZoom = image->property("imageZoom").toDouble();
    const auto rememberedOffset = image->property("imageOffset").toPointF();
    QVERIFY(fixture.press("rrImageClose"));
    QTRY_VERIFY(!fixture.visible("rrImageViewer"));
    QVERIFY(openImage());
    QTRY_VERIFY(image->property("imageReady").toBool());
    QCOMPARE(image->property("imageZoom").toDouble(), rememberedZoom);
    QVERIFY((image->property("imageOffset").toPointF() - rememberedOffset).manhattanLength() < 1);
    if (!screenshots) {
        fixture.window.readerWindow()->close();
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        image = fixture.widget("rrImageCanvas");
        QVERIFY(page && image);
        QVERIFY(openImage());
        QTRY_VERIFY(image->property("imageReady").toBool());
        QCOMPARE(image->property("imageZoom").toDouble(), rememberedZoom);
        QVERIFY((image->property("imageOffset").toPointF() - rememberedOffset).manhattanLength() < 1);
    }
    QVERIFY(fixture.press("rrImageZoomOut"));
    QVERIFY(fixture.press("rrImageFit"));
    QCOMPARE(image->property("imageZoom").toDouble(), 1.0);
    QCOMPARE(image->property("imageOffset").toPointF(), QPointF());
    QVERIFY(!fixture.widget("rrImageZoomOut")->isEnabled());
    QVERIFY(fixture.widget("rrImageZoomIn")->isEnabled());
    QVERIFY(fixture.press("rrImageClose"));
    QTRY_VERIFY(!fixture.visible("rrImageViewer"));
    QCOMPARE(fixture.state().value("page"), position);
    QCOMPARE(fixture.state().value("position"), nativePosition);
    QVERIFY(!image->property("imageReady").toBool());
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrScrubberImage"));
    QVERIFY(fixture.press("rrScrubberImageDismiss"));
    QVERIFY(!fixture.visible("rrScrubberImage"));
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(!fixture.visible("rrScrubberImage"));
    QVERIFY(fixture.press("rrPage"));
    // A bookmark jump to the cover offers both actions and preserves the return trip through the viewer.
    QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
    QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
    QVERIFY(fixture.turnPage(1));
    const auto returnPosition = fixture.state().value("position");
    QVERIFY(fixture.press("rrPage"));
    QVERIFY(fixture.press("rrBookmarks"));
    QTRY_VERIFY(fixture.visible("rrBookmark0"));
    QVERIFY(fixture.press("rrBookmark0"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_COMPARE(fixture.state().value("page"), position);
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrScrubberReturn"));
    auto *suggestion = fixture.widget("rrScrubberImage");
    auto *returnButton = fixture.widget("rrScrubberReturn");
    const bool bothFit = suggestion->sizeHint().width() + returnButton->sizeHint().width() +
                         2 * int(ui::metrics::scrubber::closeWidth) + 2 * int(ui::metrics::control::border) +
                         3 * int(ui::metrics::scrubber::gap) <= fixture.widget("rrPageScrubber")->width();
    QCOMPARE(suggestion->isVisible(), bothFit);
    if (bothFit) {
        QVERIFY(suggestion->geometry().right() < returnButton->x());
        QVERIFY(suggestion->width() >= suggestion->sizeHint().width());
        QVERIFY(returnButton->width() >= returnButton->sizeHint().width());
    }
    if (screenshots) QVERIFY(capture(fixture, name + "-image-viewer-entry"));
    if (bothFit) QVERIFY(fixture.press("rrScrubberImage"));
    else QVERIFY(openImage());
    QTRY_VERIFY(image->property("imageReady").toBool());
    QVERIFY(fixture.press("rrImageClose"));
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrScrubberReturn"));
    QVERIFY(fixture.press("rrScrubberReturn"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QTRY_COMPARE(fixture.state().value("position"), returnPosition);
    QVERIFY(fixture.turnPage(-1));
    QTRY_COMPARE(fixture.state().value("page"), position);
    if (screenshots) return;
    auto *device = QTest::createTouchDevice();
    QVERIFY(openImage());
    QTRY_VERIFY(image->property("imageReady").toBool());
    QCOMPARE(image->property("imageZoom").toDouble(), 1.0);
    const auto mapTouch = [&](QPoint point) {
        return view->mapFromScene(image->mapTo(fixture.window.readerWindow(), point));
    };
    const auto touch = [&](QEvent::Type type, QEventPoint::State state, const QList<QPoint> &positions) {
        QList<QEventPoint> points;
        for (int i = 0; i < positions.size(); ++i) {
            const auto local = mapTouch(positions[i]);
            QEventPoint point(i, state, local, view->viewport()->mapToGlobal(local));
            QMutableEventPoint::setPosition(point, local);
            points.append(point);
        }
        QTouchEvent event(type, device, Qt::NoModifier, points);
        QApplication::sendEvent(view->viewport(), &event);
    };
    touch(QEvent::TouchBegin, QEventPoint::Pressed, {center - QPoint(60, 0), center + QPoint(60, 0)});
    touch(QEvent::TouchUpdate, QEventPoint::Updated, {center - QPoint(120, 0), center + QPoint(120, 0)});
    QVERIFY(image->property("imageZoom").toDouble() > 1.9);
    touch(QEvent::TouchEnd, QEventPoint::Released, {center - QPoint(120, 0), center + QPoint(120, 0)});
    QCOMPARE(image->property("imagePreviewZoom"), image->property("imageZoom"));
    touch(QEvent::TouchBegin, QEventPoint::Pressed, {center});
    QElapsedTimer burst;
    burst.start();
    int previews = 0;
    auto shown = image->property("imagePreviewOffset");
    for (int i = 1; i <= 30; ++i) {
        touch(QEvent::TouchUpdate, QEventPoint::Updated, {center + QPoint(i * 2, i)});
        QTest::qWait(5);
        const auto next = image->property("imagePreviewOffset");
        previews += next != shown;
        shown = next;
    }
    QVERIFY(previews > 0);
    QVERIFY2(previews <= burst.elapsed() / 100 + 2, "Drag previews followed input faster than the throttle allows");
    const auto beforeRelease = image->property("imageOffset").toPointF();
    touch(QEvent::TouchEnd, QEventPoint::Released, {center + QPoint(75, 40)});
    QVERIFY(image->property("imageOffset").toPointF() != beforeRelease);
    QCOMPARE(image->property("imagePreviewOffset"), image->property("imageOffset"));
    const auto finished = image->property("imagePreviewOffset");
    QTest::qWait(120);
    QCOMPARE(image->property("imagePreviewOffset"), finished);
    QVERIFY(fixture.press("rrImageFit"));
    touch(QEvent::TouchBegin, QEventPoint::Pressed, {center});
    touch(QEvent::TouchEnd, QEventPoint::Released, {center});
    touch(QEvent::TouchBegin, QEventPoint::Pressed, {center});
    touch(QEvent::TouchEnd, QEventPoint::Released, {center});
    QCOMPARE(image->property("imageZoom").toDouble(), 2.0);
    QVERIFY(fixture.press("rrImageClose"));
    // A cover hold must also work while annotation mode is enabled.
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrAnnotate"));
    QVERIFY(fixture.press("rrAnnotate"));
    QVERIFY(openImage());
    QTRY_VERIFY(image->property("imageReady").toBool());
    QTest::keyClick(fixture.widget("rrImageViewer"), Qt::Key_Escape);
    QTRY_VERIFY(!fixture.visible("rrImageViewer"));
    QVERIFY(openImage());
    fixture.window.readerWindow()->sleepReader();
    QVERIFY(!fixture.visible("rrImageViewer"));
    QVERIFY(!image->property("imageReady").toBool());
    fixture.window.readerWindow()->prepareWake();
    fixture.window.readerWindow()->wakeReader();
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrNavChapters"));
    QVERIFY(fixture.press("rrNavChapters"));
    QTRY_VERIFY(fixture.visible("rrContents"));
    QVERIFY(fixture.press("rrChapter1"));
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    const auto ornaments = QJsonDocument::fromJson(page->image.text("kobalt.image_regions").toUtf8()).array();
    QVERIFY(!ornaments.isEmpty());
    for (const auto &value : ornaments) {
        const auto r = value.toArray();
        const QRect bounds(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        QVERIFY(!pageImageAtPoint(page->image, bounds.center()));
        const auto point = view->mapFromScene(page->mapTo(fixture.window.readerWindow(), bounds.center() + QPoint(0, page->topHeight())));
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTest::qWait(550);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(!fixture.visible("rrImageViewer"));
    }
    // A single-page image book still needs its floating action, even without page navigation.
    fixture.window.readerWindow()->close();
    QTRY_VERIFY(fixture.window.idle());
    const auto files = test::publisherPackage("en", "Single image", {
        {"text.xhtml", test::publisherHtml("<p style='margin:0'><img src='art.svg' style='width:70%;height:auto'/></p>")},
        {"art.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='600' height='800'><rect width='600' height='800' fill='#d32f2f'/></svg>"}},
        "<itemref idref=\"text.xhtml\"/>");
    QVERIFY(writeBook(fixture.directory.path(), "single-image.epub", files));
    const int previousOpens = opened.count();
    fixture.window.openBook(fixture.directory.path() + "/single-image.epub");
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), previousOpens + 1, 30000);
    QTRY_VERIFY(!fixture.state().value("busy").toBool());
    QCOMPARE(fixture.state().value("pages").toInt(), 1);
    QVERIFY(fixture.press("rrPage"));
    QTRY_VERIFY(fixture.visible("rrScrubberImage"));
    QVERIFY(fixture.widget("rrScrubberImage")->isEnabled());
    QVERIFY(fixture.press("rrScrubberImage"));
    image = fixture.widget("rrImageCanvas");
    QTRY_VERIFY(image->property("imageReady").toBool());
    const auto colourImage = image->grab().toImage();
    QCOMPARE(colourImage.pixelColor(colourImage.rect().center()), QColor("#d32f2f"));
}

class DesktopTests final : public QObject {
    Q_OBJECT
private slots:
    void imageViewer_data() { screenshotVariants(); }
    void imageViewer() { QFETCH(int, preset); QFETCH(bool, dark); checkImageViewer(preset, dark, false); }
    void pageAnimationCompatibility_data() { screenshotVariants(); }
    void pageAnimationCompatibility() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        checkPageAnimationCompatibility(preset, dark);
    }

    void galleryWorkerFailure() {
        QTemporaryDir directory(QDir::tempPath() + "/gallery workers-XXXXXX");
        QVERIFY(directory.isValid());
        const auto script = directory.path() + "/worker.sh";
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray source = R"SH(#!/bin/sh
if [ "$3" = "-datatags" ]; then
    printf 'ScreenshotTests fixture 1-light\nScreenshotTests fixture 1-dark\n'
    exit 0
fi
if [ "$3" = "fixture:1-light" ]; then
    echo $$ > "$2/pid"
    exec sleep 30
fi
# Fail only after proving the other case is running concurrently.
root=${2%/*}
i=0
while [ ! -f "$root/worker-0/pid" ] && [ "$i" -lt 100 ]; do
    sleep 0.01
    i=$((i + 1))
done
[ -f "$root/worker-0/pid" ] || exit 9
exit 7
)SH";
        QCOMPARE(file.write(source), source.size());
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto previous = qgetenv("KOBALT_GALLERY_JOBS");
        const bool wasSet = qEnvironmentVariableIsSet("KOBALT_GALLERY_JOBS");
        const auto restore = qScopeGuard([&] {
            if (wasSet) qputenv("KOBALT_GALLERY_JOBS", previous);
            else qunsetenv("KOBALT_GALLERY_JOBS");
        });
        qputenv("KOBALT_GALLERY_JOBS", "2");
        QJsonArray images;
        QElapsedTimer timer;
        timer.start();
        QTest::ignoreMessage(QtCriticalMsg, QRegularExpression("fixture:1-dark.*"));
        QVERIFY(!test::screenshotJobs(script, directory.path(), directory.path(), false, images));
        QVERIFY(timer.elapsed() < 5000);
        QFile pidFile(directory.path() + "/worker-0/pid");
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        bool ok = false;
        const auto pid = pidFile.readAll().trimmed().toLongLong(&ok);
        QVERIFY(ok && pid > 0);
        errno = 0;
        QCOMPARE(::kill(pid_t(pid), 0), -1);
        QCOMPARE(errno, ESRCH);
        QVERIFY(images.isEmpty());
        qputenv("KOBALT_GALLERY_JOBS", "0");
        QTest::ignoreMessage(QtCriticalMsg, "KOBALT_GALLERY_JOBS must be between 1 and 32");
        QVERIFY(!test::screenshotJobs(script, directory.path(), directory.path(), false, images));
    }
    void gallerySceneSelection() {
        QTemporaryDir directory(QDir::tempPath() + "/gallery scenes-XXXXXX");
        QVERIFY(directory.isValid());
        const auto script = directory.path() + "/worker.sh";
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray source = R"SH(#!/bin/sh
if [ "$3" = "-datatags" ]; then
    printf 'ScreenshotTests fixture 1-light\nScreenshotTests fixture 1-dark\n'
    exit 0
fi
: > "$2/$3.png"
printf '[{"file":"%s.png","scene":"%s"}]' "$3" "$KOBALT_GALLERY_CAPTURE" > "$2/part.json"
)SH";
        QCOMPARE(file.write(source), source.size());
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto destination = directory.path() + "/images";
        QVERIFY(QDir().mkpath(destination));
        QJsonArray images;
        QVERIFY(test::screenshotJobs(script, directory.path(), destination, false, images,
                                     {{"fixture:1-dark", {"about", "license"}}}));
        // Only the selected case ran, in its own worker, with its scenes.
        QCOMPARE(images.size(), 1);
        QCOMPARE(images.first().toObject()["file"].toString(), QString("fixture:1-dark.png"));
        QCOMPARE(images.first().toObject()["scene"].toString(), QString("about,license"));
        QVERIFY(!QFileInfo::exists(directory.path() + "/worker-1"));
        images = {};
        QTest::ignoreMessage(QtCriticalMsg, QRegularExpression("Screenshot case no longer exists: fixture:2-light.*"));
        QVERIFY(!test::screenshotJobs(script, directory.path(), destination, false, images, {{"fixture:2-light", {"about"}}}));
        QVERIFY(images.isEmpty());
    }
    void galleryIntegrity() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QJsonArray images;
        QMap<QString, QByteArray> originals;
        for (const auto &device : {QString("six-inch"), QString("seven-inch"), QString("phone"), QString("six-inch-landscape"), QString("phone-landscape")}) {
            for (const auto &theme : {QString("light"), QString("dark"), QString("large")}) {
                if (device.startsWith("phone") && theme == "large") continue;
                QImage image(screenshotSize(QStringList{"fill", "six-inch", "seven-inch", "phone", "six-inch-landscape", "phone-landscape"}.indexOf(device)), QImage::Format_RGB32);
                image.fill(theme == "dark" ? Qt::black : Qt::white);
                image.setDotsPerMeterX(11811);
                image.setDotsPerMeterY(11811);
                const auto name = device + '-' + theme + "-reading.png";
                QVERIFY(image.save(directory.path() + '/' + name));
                QFile file(directory.path() + '/' + name);
                QVERIFY(file.open(QIODevice::ReadOnly));
                const auto bytes = file.readAll();
                originals[name] = bytes;
                images.append(QJsonObject{{"file", name}, {"device", device}, {"theme", theme},
                    {"scene", "reading"}, {"screen", "reading"}, {"capture_page", 1}, {"group", "Reading"},
                    {"capture", "screen"}, {"ppi", 300}, {"width", image.width()}, {"height", image.height()},
                    {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
            }
        }
        const auto write = [&](const QString &name, const QByteArray &bytes) {
            QFile file(directory.path() + '/' + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), bytes.size());
        };
        const auto manifest = [&](const QJsonArray &shots) {
            write("manifest.json", QJsonDocument(QJsonObject{{"generator", "kobalt-desktop"}, {"images", shots}}).toJson());
        };
        const auto run = [&](int expected) {
            QProcess child;
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.setWorkingDirectory(directory.path());
            child.start(QCoreApplication::applicationFilePath(), {"--gallery", directory.path()});
            const bool finished = child.waitForFinished(10000);
            if (!finished) {
                child.kill();
                child.waitForFinished();
            }
            QVERIFY(finished);
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), expected);
            if (expected != 0)
                QVERIFY(child.readAll().contains("Screenshot bytes, dimensions or density differ"));
        };
        manifest(images);
        run(0);
        QFile html(directory.path() + "/index.html");
        QVERIFY(html.open(QIODevice::ReadOnly));
        const auto previous = html.readAll();
        html.close();
        QVERIFY(!previous.isEmpty());
        const auto retained = [&] {
            QFile file(directory.path() + "/index.html");
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), previous);
        };
        const auto name = images[0].toObject()["file"].toString();
        // A changed file must not be embedded under the old checksum.
        const QByteArray broken = "broken PNG";
        write(name, broken);
        run(2);
        retained();
        // Matching a corrupt file's hash still does not make it a valid PNG.
        auto changed = images;
        auto shot = changed[0].toObject();
        shot["sha256"] = QString::fromLatin1(QCryptographicHash::hash(broken, QCryptographicHash::Sha256).toHex());
        changed[0] = shot;
        manifest(changed);
        run(2);
        retained();
        // Valid bytes with stale dimensions must also preserve the old page.
        write(name, originals[name]);
        changed = images;
        shot = changed[0].toObject();
        shot["width"] = shot["width"].toInt() + 1;
        changed[0] = shot;
        manifest(changed);
        run(2);
        retained();
    }
    void diagnostics() {
        // Capture native stdout/stderr too: Qt's message handler does not see
        // libpng errors or CREngine's direct printf diagnostics.
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {"startup", "libraryOnboarding", "reading:0-light", "errors", "features"});
        // Leave time to kill and reap the child before the CMake parent's limit.
        const bool finished = child.waitForFinished(90000);
        if (!finished) {
            child.kill();
            child.waitForFinished();
        }
        const auto output = child.readAll();
        QVERIFY2(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData());
        QVERIFY2(!output.contains("libpng error:"), output.constData());
        QVERIFY2(!output.contains("CRE WARNING:"), output.constData());
        QVERIFY2(!output.contains("styles re-init needed"), output.constData());
    }
    void storageDiagnostics() {
        // ldomDocument unregisters its fonts on destruction, even when empty.
        struct Fonts {
            bool owned = !fontMan;
            ~Fonts() { if (owned) ShutdownFontManager(); }
        } fonts;
        if (fonts.owned)
            QVERIFY(InitFontManager(lString8()));
        class Storage final : public ldomDataStorageManager {
        public:
            Storage(ldomDocument &document, lUInt32 limit)
                : ldomDataStorageManager(&document, 't', limit, 65536) {}
            bool warned() const { return _maxSizeReachedWarned; }
        };
        ldomDocument document;
        Storage normal(document, 1024 * 1024);
        const auto address = normal.allocText(16, 0, lString8("A stored paragraph"));
        normal.compact(0xFFFFFF);
        QVERIFY(!normal.warned());
        QCOMPARE(QByteArray(normal.getText(address).c_str()), QByteArray("A stored paragraph"));
        // Actual pressure still warns, even after a deliberate compaction.
        Storage limited(document, 1);
        limited.compact(0xFFFFFF);
        QVERIFY(!limited.warned());
        limited.allocText(16, 0, lString8("A stored paragraph"));
        QVERIFY(limited.getUncompressedSize() > 1);
        limited.compact(0);
        QVERIFY(limited.warned());
    }
    // Opening and closing a book write "reader-shown" and "book-close" with
    // stages that fit inside their totals.
    void timingEvents() {
        QTemporaryDir logs;
        QVERIFY(logs.isValid());
        kobalt::initializeDiagnostics(logs.path());
        QMutex mutex;
        QVector<QJsonObject> events;
        kobalt::setDiagnosticObserver([&](const QJsonObject &event) {
            QMutexLocker lock(&mutex);
            events.append(event);
        });
        const auto find = [&](const char *name) {
            QMutexLocker lock(&mutex);
            for (const auto &event : events)
                if (event.value("event").toString() == QLatin1String(name))
                    return event;
            return QJsonObject();
        };
        const auto sum = [](const QJsonObject &event, std::initializer_list<const char *> stages) {
            double total = 0;
            for (const auto *stage : stages)
                total += event.value(QLatin1String(stage)).toDouble();
            return total;
        };
        {
            Fixture fixture;
            fixture.window.openBook(book);
            QTRY_VERIFY_WITH_TIMEOUT(!find("reader-shown").isEmpty(), 30000);
            const auto shown = find("reader-shown");
            for (const auto *field : {"engine_ms", "window_ms", "wait_ms", "build_ms", "build_reading_page_ms", "build_settings_ms",
                                      "theme_ms", "show_ms", "total_ms"})
                QVERIFY2(shown.value(QLatin1String(field)).isDouble(), field);
            const double parts = sum(shown, {"engine_ms", "window_ms", "wait_ms", "show_ms"});
            const double total = shown.value("total_ms").toDouble();
            qInfo() << "reader-shown" << shown;
            QVERIFY2(parts <= total + 1 && parts >= total * 0.9, "The opening stages do not fit its total");
            QVERIFY(sum(shown, {"build_reading_page_ms", "build_toolbar_ms", "build_bottom_controls_ms", "build_menu_ms",
                                "build_overlays_ms", "build_appearance_ms", "build_settings_ms", "build_notices_ms",
                                "build_annotations_ms"}) <= shown.value("build_ms").toDouble() &&
                    shown.value("build_ms").toDouble() <= shown.value("window_ms").toDouble());
            QTest::keyClick(&fixture.window, Qt::Key_PageDown);
            QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
            fixture.window.readerWindow()->close();
            QTRY_VERIFY_WITH_TIMEOUT(!find("book-close").isEmpty(), 30000);
            const auto closed = find("book-close");
            qInfo() << "book-close" << closed;
            for (const auto *field : {"checkpoint_ms", "native_ms", "session_ms", "sync_ms", "total_ms"})
                QVERIFY2(closed.value(QLatin1String(field)).isDouble(), field);
            QVERIFY(closed.value("sync_ms").toDouble() <= closed.value("total_ms").toDouble());
        }
        kobalt::setDiagnosticObserver({});
        kobalt::pauseDiagnostics(true);
    }
    void startup() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QCOMPARE(fixture.window.findChild<QComboBox *>("desktopScreenPicker")->currentIndex(), 1);
        QCOMPARE(fixture.window.findChild<QComboBox *>("desktopResolutionPicker")->currentData().toInt(), 300);
        QCOMPARE(fixture.window.findChild<QComboBox *>("desktopTextSizePicker")->currentData().toInt(), 100);
        QVERIFY(!fixture.window.readerWindow());
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->libraryShown());
        QVERIFY(start->library()->findChild<QPushButton *>("libraryOpenBook")->isVisible());
        QVERIFY(!start->findChild<QWidget *>("androidIntro"));
    }
    void libraryOnboarding() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        library::Store store(directory.path(), std::make_shared<library::LocalSource>());
        android::StartScreen start(desktop::readerServices().presentation, store);
        start.resize(640, 800);
        start.show();
        QVERIFY(start.libraryShown());
        QVERIFY(!start.dismissModal());
        auto *view = start.library();
        QVERIFY(!view->findChild<QPushButton *>("libraryBack"));
        QVERIFY(!view->findChild<QWidget *>("librarySort")->isVisible());
        QVERIFY(!view->findChild<QPushButton *>("libraryViewGrid")->isVisible());
        QVERIFY(!view->findChild<QPushButton *>("libraryLeave"));
        QSignalSpy open(&start, &android::StartScreen::libraryOpenRequested);
        view->findChild<QPushButton *>("libraryOpenBook")->click();
        QTRY_COMPARE(open.count(), 1);
        QVERIFY(start.libraryShown());
        QVERIFY(QDir().mkpath(directory.path() + "/books"));
        QVERIFY(QFile::copy(book, directory.path() + "/books/first.epub"));
        connect(&start, &android::StartScreen::addFolderRequested, &start, [&] {
            store.addFolder({directory.path() + "/books", "Books"});
        });
        view->findChild<QPushButton *>("libraryChooseFolder")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!store.index().folders.isEmpty() && !store.scanning(), 10000);
        QCOMPARE(store.index().visibleCount(), 1);
        QVERIFY(!view->findChild<QWidget *>("libraryEmpty")->isVisible());
        QVERIFY(view->findChild<QPushButton *>("librarySort")->isVisible());
        QVERIFY(start.libraryShown());
    }
    void library() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start);
        const auto libraryBooks = [&fixture] {
            library::Index index;
            QJsonArray books;
            if (index.loadDatabase(fixture.directory.path()))
                for (const auto &book : index.books) books.append(book.json());
            return books;
        };
        QVERIFY(start->libraryShown());
        QTRY_COMPARE(start->library()->size(), QSize(1072, 1448));
        QVERIFY(start->library()->findChild<QWidget *>("libraryEmpty")->isVisible());
        // Menu, Finder and dropped paths use this same entry point.
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        fixture.window.readerWindow()->close();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        QVERIFY(start->libraryShown());
        QTRY_VERIFY_WITH_TIMEOUT(libraryBooks().size() == 1, 5000);
        QVERIFY(libraryBooks().first().toObject().value("opened").toDouble() > 0);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        fixture.window.readerWindow()->close();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        QCOMPARE(libraryBooks().size(), 1);
        // Its cover is saved and the cover grid draws it. The library itself and
        // the drawing for a missing cover are gray; the fixture's cover is tan.
        const auto cover = libraryBooks().first().toObject().value("cover").toString();
        QVERIFY(!cover.isEmpty());
        QVERIFY(!QImage(fixture.directory.path() + "/library-covers/" + cover).isNull());
        const auto pages = start->library()->findChild<QWidget *>("libraryPages")->grab().toImage().convertToFormat(
            QImage::Format_RGB32);
        int coloured = 0;
        for (int y = 0; y < pages.height(); y += 4)
            for (int x = 0; x < pages.width(); x += 4) {
                const QRgb pixel = pages.pixel(x, y);
                coloured += qAbs(qRed(pixel) - qBlue(pixel)) > 12;
            }
        QVERIFY2(coloured > 200, "The cover grid did not draw the book's cover");

        // Library is the root, not a page above an intro screen.
        QVERIFY(!start->dismissModal());
        QVERIFY(start->libraryShown());
    }
    void libraryTextInheritance() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto original = fixture.state();
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.press("rrFont"));
        QTRY_VERIFY(fixture.visible("rrFontChoice0"));
        QVERIFY(fixture.press("rrFontChoice0"));
        QVERIFY(fixture.press("rrFontClose"));
        QTRY_VERIFY(fixture.state().value("preview_ready").toBool() && !fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrLarger"));
        QTRY_VERIFY(fixture.state().value("preview_ready").toBool() && !fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrLooser"));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrAlignJustify"));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrMenu") && !fixture.state().value("busy").toBool());
        const auto first = fixture.state();
        QVERIFY(first.value("font_pixels").toInt() > original.value("font_pixels").toInt());
        const auto other = fixture.directory.path() + "/unopened-book.epub";
        QVERIFY(QFile::copy(book, other));
        fixture.window.openBook(other);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto inherited = fixture.state();
        for (const auto *key : {"font_face", "font_pixels", "line_spacing", "text_alignment"})
            QCOMPARE(inherited.value(key), first.value(key));
        fixture.window.readerWindow()->close();
        QTRY_VERIFY(fixture.window.idle());

        // A previously read book has both a position and a distinct override.
        const auto identity = first.value("book").toString();
        auto saved = storage::readBookState(fixture.paths, identity).object;
        QVERIFY(!saved.value("position").toString().isEmpty());
        saved["font_pixels"] = first.value("font_pixels").toInt() + 12;
        QVERIFY(storage::writeBookState(fixture.paths, identity, saved));
        const auto fingerprint = saved.value("fingerprint").toString().toLatin1();
        BookmarkStore bookmarks(fixture.paths.annotations(), identity, fingerprint, "Reader fixture", fixture.paths.database());
        bookmarks.setBookmarks({{saved.value("position").toString(), "Prologue", "Saved bookmark",
                                 QDateTime::currentDateTimeUtc()}});
        QVERIFY(bookmarks.save());
        HighlightStore highlights(fixture.paths.annotations(), identity, fingerprint, "Reader fixture", fixture.paths.database());
        highlights.setHighlights({{"kept-highlight", original.value("position").toString(),
                                  saved.value("position").toString(), "Saved highlight", "Prologue",
                                  QDateTime::currentDateTimeUtc()}});
        QVERIFY(highlights.save());
        const auto preferences = storage::readJson(fixture.paths.preferences());
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->libraryShown());
        start->library()->markBook(book, library::Book::Mark::Unread);
        QCOMPARE(storage::readBookState(fixture.paths, identity).object, QJsonObject({{"book", identity}}));
        QCOMPARE(storage::readJson(fixture.paths.preferences()), preferences);
        QVERIFY(bookmarks.load() && bookmarks.bookmarks().size() == 1);
        QVERIFY(highlights.load() && highlights.highlights().size() == 1);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 3);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto restarted = fixture.state();
        QCOMPARE(restarted.value("page").toInt(), 0);
        for (const auto *key : {"font_face", "font_pixels", "line_spacing", "text_alignment"})
            QCOMPARE(restarted.value(key), first.value(key));
    }
    void libraryGridRows() {
        // 37 books. Medium covers make 4 by 3 on both previews, four pages.
        // On the 7-inch, Small makes 5 by 4 (two pages) and Large 3 by 2 (seven).
        Fixture fixture(false, false, true, true);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        auto *picker = fixture.window.findChild<QComboBox *>("desktopScreenPicker");
        QVERIFY(start && picker);
        start->showLibrary();
        auto *view = start->library();
        auto *canvas = start->libraryCanvas();
        auto *counter = view->findChild<QLabel *>("libraryPage");
        QVERIFY(counter);
        const auto pages = [](int count) {
            return strings::get("navigation.page_count").arg(1).arg(count);
        };
        for (const int preset : {1, 2}) {
            picker->setCurrentIndex(preset);
            QTRY_VERIFY(view->isVisible() && view->size() == canvas->sceneRect().size().toSize() &&
                        view->width() == (preset == 1 ? 1072 : 1264));
            QTRY_COMPARE(counter->text(), pages(4));
        }
        const auto chooseCover = [view](const char *name) {
            view->findChild<QPushButton *>("librarySort")->click();
            auto *choice = view->findChild<QAbstractButton *>(name);
            QVERIFY(choice && choice->isVisible());
            choice->click();
        };
        chooseCover("libraryCoverSmall");
        QTRY_COMPARE(counter->text(), pages(2));
        chooseCover("libraryCoverLarge");
        QTRY_COMPARE(counter->text(), pages(7));
    }
    // At 150% text, a scan caption too wide to sit beside the progress
    // squares hides them and shows in full. Standard text keeps both.
    void libraryScanningCaption() {
        Fixture fixture(false, false, true, true);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        auto *textSize = fixture.window.findChild<QComboBox *>("desktopTextSizePicker");
        QVERIFY(start && textSize);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(1);
        for (const int percent : {150, 100}) {
            textSize->setCurrentIndex(textSize->findData(percent));
            start->showLibrary();
            auto *view = start->library();
            auto *canvas = start->libraryCanvas();
            QTRY_VERIFY(view->isVisible() && view->size() == canvas->sceneRect().size().toSize() &&
                        view->width() == 1072);
            view->setScanning(true, 17);
            auto *progress = dynamic_cast<ui::LoadingIndicator *>(view->findChild<QWidget *>("libraryScanProgress"));
            auto *sort = view->findChild<QPushButton *>("librarySort");
            QVERIFY(progress && sort);
            QTRY_VERIFY(progress->isRunning());
            QCOMPARE(progress->isVisible(), percent == 100);
            if (percent == 100) {
                view->setScanning(false, 17);
                continue;
            }
            // The caption's ink starts at the inset and spans its full width.
            const auto caption = strings::get("library.scanning").arg(17).toUpper();
            const int full = ui::interfaceTextWidth(caption, ui::captionFont());
            const auto image = sort->grab().toImage().convertToFormat(QImage::Format_RGB32);
            int first = -1, last = -1;
            for (int x = 0; x < image.width(); ++x)
                for (int y = 0; y < image.height(); ++y)
                    if (qGray(image.pixel(x, y)) < 128) {
                        if (first < 0)
                            first = x;
                        last = x;
                        break;
                    }
            QVERIFY2(first >= int(ui::metrics::chrome::inset) - ui::px(4) &&
                         first <= int(ui::metrics::chrome::inset) + ui::px(8),
                     "the caption must start at the inset, without progress squares");
            QVERIFY2(last - first + 1 >= full - ui::px(8), "the scan caption was elided");
            view->setScanning(false, 17);
        }
    }
    void libraryHold() {
        // Holding a book opens its menu in every view, through the canvas as
        // the Mac app delivers the press.
        Fixture fixture(false, false, true, true);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start);
        start->showLibrary();
        auto *view = start->library();
        auto *canvas = start->libraryCanvas();
        QTRY_VERIFY(view->isVisible() && view->geometry().bottomRight() + QPoint(1, 1) == canvas->sceneRect().bottomRight().toPoint() && view->width() > 600);
        auto *pages = view->findChild<QWidget *>("libraryPages");
        QVERIFY(pages);
        for (const char *mode : {"libraryViewGrid", "libraryViewList", "libraryViewHome"}) {
            view->findChild<QPushButton *>(mode)->click();
            // The first book: the top left of the pages, inside its cover or row.
            const QPoint inPages(pages->width() / 8, pages->height() / 8);
            const QPoint point = canvas->mapFromScene(QPointF(pages->mapTo(view, inPages)));
            QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            // A resting finger drifts a few points; the hold must survive that.
            const QPoint drift = point + QPoint(4, 3);
            QMouseEvent move(QEvent::MouseMove, drift, canvas->viewport()->mapToGlobal(drift), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QTRY_VERIFY_WITH_TIMEOUT(view->findChild<QWidget *>("libraryBookMenu"), 2000);
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, drift);
            QVERIFY(view->dismiss());
            QTRY_VERIFY(!view->findChild<QWidget *>("libraryBookMenu"));
        }
        // Switching views closes the sort choices.
        view->findChild<QPushButton *>("libraryViewGrid")->click();
        view->findChild<QPushButton *>("librarySort")->click();
        auto *sheet = view->findChild<QWidget *>("librarySortSheet");
        QVERIFY(sheet && sheet->isVisible());
        view->findChild<QPushButton *>("libraryViewList")->click();
        QVERIFY(!sheet->isVisible());
    }
    void libraryAtStart() {
        // Once there is a library, a fresh start opens on it.
        Fixture fixture(false, false, true, false);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->libraryShown());
        QVERIFY(!start->dismissModal());
        QVERIFY(start->libraryShown());
    }
    void reading_data() { variants(); }
    void reading() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        auto *reader = fixture.window.readerWindow();
        QVERIFY(reader);
        QVERIFY(fixture.clockReads.load() > 0);
        QCOMPARE(reader->property("reader_dark").toBool(), dark);
        QVERIFY(fixture.state().value("pages").toInt() > 2);
        openPrologueBody(fixture);
        QVERIFY(hasText(reader->findChild<QWidget *>("rrPage")->grab().toImage()));
        const int page = fixture.state().value("page").toInt();
        // The preceding navigation used the same 200 ms input guard.
        QTest::qWait(210);
        reader->pageButton(1);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.state().value("page").toInt() > page, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        const auto position = fixture.state().value("position").toString();
        QVERIFY(!position.isEmpty());
        QVERIFY(fixture.click(reader->findChild<QWidget *>("rrPage")));
        auto *exit = reader->findChild<QPushButton *>("rrReturn");
        QTRY_VERIFY(exit->isVisible());
        QVERIFY(dynamic_cast<ui::Button *>(exit));
        QCOMPARE(exit->accessibleName(), strings::get("library.exit_to_library"));
        QVERIFY(exit->width() > int(ui::metrics::chrome::cellWidth));
        QVERIFY(exit->findChildren<QPushButton *>().isEmpty());
        auto *returnTitle = reader->findChild<QLabel *>("rrReturnTitle");
        QVERIFY(returnTitle && returnTitle->isVisible());
        QCOMPARE(returnTitle->text(), strings::get("library.exit_to_library"));
        QCOMPARE(returnTitle->parentWidget(), exit);
        QVERIFY(returnTitle->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(returnTitle->height(), exit->height());
        const auto originalCaption = returnTitle->text();
        for (const auto &caption : {strings::get("reader.exit"), strings::get("library.exit_to_library")}) {
            returnTitle->setText(caption);
            const auto image = returnTitle->grab().toImage();
            int first = image.height(), last = -1;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixel(x, y);
                    if (qAlpha(pixel) > 128 && (dark ? qGray(pixel) > 128 : qGray(pixel) < 128)) {
                        first = qMin(first, y);
                        last = qMax(last, y);
                    }
                }
            const auto buttonImage = exit->grab().toImage();
            const auto arrow = returnInkBounds(buttonImage,
                QRect(0, 0, returnTitle->x(), exit->height()), dark);
            const auto textArea = returnTitle->contentsRect().translated(returnTitle->pos());
            const auto word = returnInkBounds(buttonImage, textArea, dark);
            const auto firstLetter = returnInkBounds(buttonImage,
                QRect(textArea.left(), 0, ui::interfaceTextWidth(caption.left(1), returnTitle->font()), exit->height()), dark);
            QVERIFY(!arrow.isEmpty() && !word.isEmpty() && !firstLetter.isEmpty());
            const int expectedGap = qRound(6 * ui::density() / 72.0);
            QVERIFY(qAbs(word.left() - arrow.right() - 1 - expectedGap) <= 2);
            QVERIFY(qAbs(firstLetter.top() + firstLetter.bottom() + 2 * (int(ui::metrics::chrome::returnArrowOffset) - int(ui::metrics::chrome::returnTitleOffset)) - arrow.top() - arrow.bottom()) <= 2);
            QVERIFY(last >= first);
            QVERIFY2(qAbs(first + last - (image.height() - 1) - 2 * int(ui::metrics::chrome::returnTitleOffset)) <= 2,
                     qPrintable(QString("%1 ink center is %2 px from the toolbar center")
                                    .arg(caption).arg((first + last - image.height() + 1) / 2.0)));
        }
        returnTitle->setText(originalCaption);
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        QVERIFY(canvas);
        const auto point = preset == 2 ? QPoint(int(ui::metrics::chrome::cellWidth) / 2, exit->height() / 2)
                                      : returnTitle->mapTo(exit, returnTitle->rect().center());
        const auto target = canvas->mapFromScene(exit->mapTo(reader, point));
        const auto normal = exit->grab().toImage();
        QCOMPARE(normal.pixelColor(5, 5), ui::themeColors(dark).paper);
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        QTRY_VERIFY(exit->isDown());
        const auto pressed = exit->grab().toImage();
        QCOMPARE(pressed.pixelColor(5, 5), ui::themeColors(dark).ink);
        QCOMPARE(pressed.pixelColor(int(ui::metrics::chrome::cellWidth) + 5, 5), ui::themeColors(dark).ink);
        QVERIFY(visibleReturnCaption(pressed, returnTitle, ui::themeColors(dark).paper));
        QCOMPARE(pressed.pixelColor(int(ui::metrics::chrome::cellWidth) - 1, 5), ui::themeColors(dark).ink);
        QCOMPARE(pressed.pixelColor(pressed.width() - 1, 5), exit->palette().color(QPalette::Mid));
        QVERIFY(pressed != normal);
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, target);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        QCOMPARE(fixture.state().value("position").toString(), position);
    }
    void restartFinishedBooks() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.state().value("page").toInt() > 0);
        fixture.window.readerWindow()->close();
        QTRY_VERIFY(fixture.window.idle());
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->library());
        start->library()->markBook(QFileInfo(book).absoluteFilePath(), library::Book::Mark::Finished);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("page").toInt(), 0);
        fixture.window.readerWindow()->pageButton(1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("page").toInt() > 0);
        const auto position = fixture.state().value("position").toString();
        fixture.window.readerWindow()->close();
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 3);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("position").toString(), position);
    }
    void errors() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy failed(&fixture.window, &desktop::Window::bookFailed);
        fixture.window.openBook(fixture.directory.path() + "/missing.epub");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        fixture.window.openBook(book);
        fixture.window.close();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        QVERIFY(!fixture.window.isVisible());
    }
    void features() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        tour(fixture, {}, false);
    }
    void bookmarks_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void pinnedBookmarks_data() { screenshotVariants(); }
    void pinnedBookmarks() { QFETCH(int, preset); QFETCH(bool, dark); checkPinnedBookmarks(preset, dark); }
    void bookmarks() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        // The list shows the bookmark at once; its save follows on a writer thread.
        QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").size(), 1);
        auto original = fixture.stored("bookmarks.json", "bookmarks");
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBookmarks"));
        QVERIFY(fixture.press("rrBookmarks"));
        QTRY_VERIFY(fixture.visible("rrBookmarkMore0"));
        QVERIFY(fixture.widget("rrAnnotationMore")->isVisible());
        QVERIFY(fixture.widget("rrAnnotationMore")->isEnabled());
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrBookmarkRemoveCurrent"));
        QCOMPARE(qobject_cast<QPushButton *>(fixture.widget("rrBookmarkRemoveCurrent"))->text(), strings::get("bookmarks.remove_current"));
        QCOMPARE(fixture.widget("rrBookmarkActions")->findChildren<QPushButton *>().size(), 1);
        QVERIFY(!fixture.visible("rrAnnotationExportHtml"));
        const auto removePageIcon = fixture.widget("rrAnnotationMore")->grab().toImage();
        QVERIFY(fixture.press("rrBookmarkRemoveCurrent"));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QTRY_VERIFY(!page->bookmarked);
        QTRY_VERIFY(fixture.widget("rrAnnotationMore")->grab().toImage() != removePageIcon);
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrBookmarkAddCurrent"));
        QCOMPARE(qobject_cast<QPushButton *>(fixture.widget("rrBookmarkAddCurrent"))->text(), strings::get("bookmarks.add_current"));
        QCOMPARE(fixture.widget("rrBookmarkActions")->findChildren<QPushButton *>().size(), 1);
        QVERIFY(fixture.press("rrBookmarkAddCurrent"));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        QTRY_VERIFY(page->bookmarked);
        QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").size(), 1);
        original = fixture.stored("bookmarks.json", "bookmarks");
        QVERIFY(fixture.press("rrAnnotationsTab"));
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportHtml"));
        QVERIFY(!fixture.widget("rrAnnotationExportHtml")->isEnabled());
        QVERIFY(!fixture.widget("rrAnnotationExportMarkdown")->isEnabled());
        QVERIFY(fixture.click(fixture.widget("rrAnnotationActions"), QPoint(5, 5)));
        QTRY_VERIFY(!fixture.widget("rrAnnotationActions"));
        QVERIFY(fixture.press("rrBookmarksTab"));
        QVERIFY(fixture.bookmarkAction("rrBookmarkDelete"));
        QTRY_VERIFY(fixture.visible("rrRemoveBookmark"));
        QCOMPARE(fixture.stored("bookmarks.json", "bookmarks"), original);
        QVERIFY(fixture.press("rrKeepBookmark"));
        QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
        QCOMPARE(fixture.stored("bookmarks.json", "bookmarks"), original);
        QVERIFY(fixture.bookmarkAction("rrBookmarkDelete"));
        QTRY_VERIFY(fixture.visible("rrConfirmRemoveBookmark"));
        QVERIFY(fixture.press("rrConfirmRemoveBookmark"));
        QTRY_VERIFY(!fixture.visible("rrRemoveBookmark"));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QTRY_VERIFY(fixture.stored("bookmarks.json", "bookmarks").isEmpty());
        QVERIFY(!page->bookmarked);
        QVERIFY(fixture.visible("rrBookmarkListEmpty"));
        QVERIFY(fixture.press("rrBookmarkListClose"));
        if (!fixture.visible("rrReturn"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrReturn"));
        // Closing the list intentionally guards Exit against the release
        // of the tap that dismissed it. Wait for that defined input interval.
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 20);
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("bookmark_count").toInt(), 0);
    }
    void annotations_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("pickerExport");
        QTest::addColumn<bool>("markdownExport");
        QTest::newRow("light-default-html") << false << false << false;
        QTest::newRow("dark-default-markdown") << true << false << true;
        QTest::newRow("light-picker-markdown") << false << true << true;
        QTest::newRow("dark-picker-html") << true << true << false;
    }
    void bookmarkMerge() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
        for (int index = 0; index < 2; ++index) {
            QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
            QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), index + 1);
            if (!index) {
                const int before = fixture.state().value("page").toInt();
                QVERIFY(fixture.turnPage(1));
                QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("page").toInt() > before);
            }
        }
        const int originalSize = fixture.state().value("font_pixels").toInt();
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavText"));
        QVERIFY(fixture.press("rrNavText"));
        QTRY_VERIFY(fixture.visible("rrTextSize"));
        auto *size = fixture.widget("rrTextSize");
        QVERIFY(fixture.click(size, {0, size->height() / 2}));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_VERIFY(fixture.state().value("font_pixels").toInt() < originalSize);
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        QTRY_COMPARE(fixture.stored("bookmarks.json", "bookmarks").size(), 1);
        QTRY_VERIFY(fixture.state().value("bookmarked").toBool());
        QTRY_VERIFY(!fixture.visible("rrControlsTop") && fixture.widget("rrGraph")->isEnabled());
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QTRY_VERIFY(fixture.stored("bookmarks.json", "bookmarks").isEmpty());
    }
    void touchCursor() {
        Fixture fixture(false);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        QVERIFY(canvas);
        const QPoint point = canvas->viewport()->rect().center();
        QMouseEvent hoverEvent(QEvent::MouseMove, point, canvas->viewport()->mapToGlobal(point), Qt::NoButton,
                               Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &hoverEvent);
        QVERIFY(QApplication::overrideCursor());
        const auto hover = QApplication::overrideCursor()->pixmap().toImage();
        QVERIFY(!hover.isNull());
        QCOMPARE(QApplication::overrideCursor()->hotSpot(), QPoint(14, 14));
        QCOMPARE(hover.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(hover.pixelColor(hover.width() / 2, hover.height() / 2).alpha(), 128);
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        const auto pressed = QApplication::overrideCursor()->pixmap().toImage();
        QVERIFY(pressed.pixelColor(pressed.width() / 2, pressed.height() / 2).alpha() >
                hover.pixelColor(hover.width() / 2, hover.height() / 2).alpha());
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(QApplication::overrideCursor()->pixmap().toImage(), hover);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&fixture.window, &leave);
        QVERIFY(!QApplication::overrideCursor());
        auto *screens = fixture.window.findChild<QComboBox *>("desktopScreenPicker");
        screens->setCurrentIndex(0);
        QTest::mouseMove(canvas->viewport(), point + QPoint(1, 0));
        QVERIFY(!QApplication::overrideCursor());
    }
    void touchCursorOutsideReader() {
        // E-reader presets use the touch pointer across the whole window,
        // including the preview toolbar and the space around the device.
        Fixture fixture(false);
        QVERIFY(fixture.prepared);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        auto *picker = fixture.window.findChild<QComboBox *>("desktopScreenPicker");
        QVERIFY(start && picker);
        const auto hover = [](QWidget *target) {
            const QPoint point = target->rect().center();
            QMouseEvent move(QEvent::MouseMove, point, target->mapToGlobal(point), Qt::NoButton, Qt::NoButton,
                             Qt::NoModifier);
            QApplication::sendEvent(target, &move);
        };
        const auto touchPointer = [] {
            const auto *cursor = QApplication::overrideCursor();
            return cursor && !cursor->pixmap().isNull() && cursor->hotSpot() == QPoint(14, 14);
        };
        hover(start);
        QVERIFY(touchPointer());
        hover(picker);
        QVERIFY(touchPointer());
        start->showLibrary();
        auto *library = start->libraryCanvas();
        QTRY_VERIFY(library->isVisible());
        hover(library->viewport());
        QVERIFY(touchPointer());
        hover(picker);
        QVERIFY(touchPointer());
        hover(&fixture.window);
        QVERIFY(touchPointer());
        // The seven-inch preset follows the same rule.
        picker->setCurrentIndex(2);
        hover(picker);
        QVERIFY(touchPointer());
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&fixture.window, &leave);
        QVERIFY(!touchPointer());
        hover(picker);
        QVERIFY(touchPointer());
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(&fixture.window, &deactivate);
        QVERIFY(!touchPointer());
        // Fill window keeps the normal pointer.
        picker->setCurrentIndex(0);
        hover(library->viewport());
        QVERIFY(!touchPointer());
    }
    void libraryPageButtons() {
        Fixture fixture(false, false, true);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start && start->libraryShown());
        auto *library = start->library();
        QTRY_VERIFY(library->isVisible() && library->size() == QSize(1264, 1680));
        QApplication::setActiveWindow(&fixture.window);
        for (const auto *mode : {"libraryViewGrid", "libraryViewList"}) {
            library->findChild<QPushButton *>(mode)->click();
            auto *counter = library->findChild<QLabel *>("libraryPage");
            QVERIFY(counter && counter->isVisible());
            const auto initial = counter->text();
            fixture.window.setFocus();
            QTest::keyClick(&fixture.window, Qt::Key_PageDown);
            QTRY_VERIFY(counter->text() != initial);
            QTest::keyClick(&fixture.window, Qt::Key_PageUp);
            QTRY_COMPARE(counter->text(), initial);
            auto *next = fixture.window.findChild<QPushButton *>("desktopDeviceNext");
            auto *previous = fixture.window.findChild<QPushButton *>("desktopDevicePrevious");
            QVERIFY(next && previous);
            QTRY_VERIFY(next->isVisible() && previous->isVisible() && next->isEnabled() && previous->isEnabled());
            QTest::mouseClick(next, Qt::LeftButton);
            QTRY_VERIFY(counter->text() != initial);
            QTest::mouseClick(previous, Qt::LeftButton);
            QTRY_COMPARE(counter->text(), initial);
            library->findChild<QPushButton *>("libraryMore")->click();
            library->pageButton(1);
            QCOMPARE(counter->text(), initial);
            QVERIFY(library->dismiss());
            library->showNotice("Notice", "Hardware buttons must not page behind this notice.");
            library->pageButton(1);
            QCOMPARE(counter->text(), initial);
            QVERIFY(library->dismiss());
        }
    }
    void hardwarePageButtons_data() {
        QTest::addColumn<QString>("view");
        for (const auto *view : {"dictionary", "dictionary-choices", "footnote", "settings", "settings-fonts",
                                "reading-fonts", "stats", "fields", "chapters", "bookmarks", "search",
                                "acknowledgments", "license", "downloaded-dictionaries", "blocked-modal"})
            QTest::newRow(view) << QString::fromLatin1(view);
    }
    void hardwarePageButtons() {
        QFETCH(QString, view);
        Fixture fixture;
        QVERIFY(fixture.prepared);
        if (view == "downloaded-dictionaries")
            fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCoreApplication::processEvents();
        auto *reader = fixture.window.readerWindow();
        const auto position = fixture.state().value("position");
        const QString definition = QString("<p>A long definition with enough text to wrap and fill several pages.</p>").repeated(100);
        const char *counterName = nullptr;
        if (view.startsWith("dictionary")) {
            auto *panel = dynamic_cast<DictionaryPanel *>(fixture.widget("rrDictionary"));
            QVERIFY(panel);
            QVector<DictionaryChoice> choices;
            for (int i = 0; i < 35; ++i)
                choices.append({QString::number(i), QString("Dictionary %1").arg(i)});
            panel->setChoices(choices);
            DictionaryResult result;
            result.received = result.restored = true;
            result.dictionary = "Dictionary 0";
            result.dictionaryId = "0";
            result.html = definition;
            panel->open("word", result, {100, 100, 50, 40});
            counterName = "rrDictionaryPages";
            if (view == "dictionary-choices") {
                QVERIFY(fixture.press("rrDictionarySelect"));
                counterName = "rrDictionaryChoicesPages";
            }
        } else if (view == "footnote") {
            auto *panel = dynamic_cast<LinkPanel *>(fixture.widget("rrLinkPanel"));
            QVERIFY(panel);
            LinkDetails details;
            details.kind = LinkKind::Footnote;
            details.html = definition;
            details.target = "passage";
            panel->open(details, {100, 100, 50, 40});
            counterName = "rrLinkPages";
        } else if (view == "settings" || view == "settings-fonts" ||
                   view == "downloaded-dictionaries" || view == "blocked-modal") {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrSettings"));
            QVERIFY(fixture.press(view == "downloaded-dictionaries" ? "rrSettingsDictionaries" : "rrSettingsSystem"));
            counterName = view == "downloaded-dictionaries" ? "rrDictionariesPage" : "rrSettingsOptionsPage";
            if (view == "settings-fonts") {
                QVERIFY(revealSettingsOption(fixture, "rrUiFont"));
                QVERIFY(fixture.press("rrUiFont"));
                counterName = "rrSettingsFontPage";
            }
        } else if (view == "reading-fonts") {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrNavText"));
            QVERIFY(fixture.press("rrFont"));
            auto *choices = dynamic_cast<ChoiceScreen *>(fixture.widget("rrChoices"));
            QVERIFY(choices);
            QVector<Choice> fonts;
            for (int i = 0; i < 40; ++i)
                fonts.append({QString("Font %1").arg(i), "Libron", QString("testFont%1").arg(i)});
            choices->showChoices("Font", fonts, {}, ChoiceScreen::Mode::Fonts);
            counterName = "rrFontPagerPage";
        } else if (view == "stats") {
            auto *graph = dynamic_cast<ChapterGraph *>(fixture.widget("rrChapterGraph"));
            QVERIFY(graph);
            QVector<ChapterLink> chapters;
            for (int i = 0; i < 50; ++i)
                chapters.append({QString("Chapter %1").arg(i), QString::number(i), i, 0, 100});
            graph->changed = {};
            graph->setDense(false);
            graph->setData(chapters, {}, true, {});
            graph->open();
            counterName = "rrGraphPage";
        } else if (view == "fields") {
            auto *fields = dynamic_cast<kobalt::reader::FieldChooser *>(fixture.widget("rrHeaderFooterChooser"));
            QVERIFY(fields);
            fields->setGeometry(reader->rect());
            fields->open(0, headerfooter::choosableFields[0], {}, false);
            fields->show();
            fields->raise();
            counterName = "rrHeaderFooterFieldsPage";
        } else if (view == "chapters") {
            auto *choices = dynamic_cast<ChoiceScreen *>(fixture.widget("rrChoices"));
            QVERIFY(choices);
            QVector<Choice> chapters;
            for (int i = 0; i < 50; ++i)
                chapters.append({QString("Chapter %1").arg(i), QString::number(i), QString("testChapter%1").arg(i)});
            choices->changed = {};
            choices->showChoices("Chapters", chapters);
            counterName = "rrContentsPage";
        } else if (view == "bookmarks") {
            auto *list = dynamic_cast<BookmarkList *>(fixture.widget("rrBookmarkList"));
            QVERIFY(list);
            QVector<BookmarkRow> rows;
            for (int i = 0; i < 40; ++i)
                rows.append({QString::number(i), "Chapter", "Excerpt", {}, i});
            list->showBookmarks(rows);
            counterName = "rrBookmarkListPage";
        } else if (view == "search") {
            auto *search = dynamic_cast<SearchPanel *>(fixture.widget("rrSearch"));
            QVERIFY(search);
            search->open(false, 0);
            SearchBatch batch;
            batch.done = true;
            for (int i = 0; i < 40; ++i)
                batch.hits.append({QString::number(i), QString::number(i + 1), "Before", "match", "after", i});
            search->append(batch, 100);
            counterName = "rrSearchPage";
        } else {
            auto *acknowledgments = dynamic_cast<Acknowledgments *>(fixture.widget("rrAcknowledgments"));
            QVERIFY(acknowledgments);
            acknowledgments->setGeometry(reader->rect());
            acknowledgments->open();
            if (view == "license") {
                QVERIFY(fixture.press("rrAcknowledgment0"));
                for (auto *child : acknowledgments->findChildren<QWidget *>())
                    if (auto *license = dynamic_cast<LicenseText *>(child))
                        license->setText(QString("A long license paragraph.\n\n").repeated(300));
                acknowledgments->resize(acknowledgments->width(), acknowledgments->height() - 1);
            }
            counterName = "rrAcknowledgmentsPage";
        }
        // Dictionary rows redraw after their nested layout has resized.
        QCoreApplication::processEvents();
        auto *counter = reader->findChild<QLabel *>(counterName);
        QVERIFY(counter && counter->isVisible());
        auto *pager = dynamic_cast<ui::PagerBar *>(counter->parentWidget());
        QVERIFY(pager && pager->next->isEnabled());
        const auto initial = counter->text();
        if (view == "blocked-modal") {
            ui::NoticeModal notice(reader, {"", "Notice", {}}, {"OK", "testNoticeClose"});
            notice.show();
            notice.raise();
            reader->pageButton(1);
            QCOMPARE(counter->text(), initial);
            QCOMPARE(fixture.state().value("position"), position);
            notice.hide();
        }
        reader->pageButton(1);
        QTRY_VERIFY(counter->text() != initial);
        QCOMPARE(fixture.state().value("position"), position);
        reader->pageButton(-1);
        QTRY_COMPARE(counter->text(), initial);
        QApplication::setActiveWindow(&fixture.window);
        fixture.window.setFocus();
        QTest::keyClick(&fixture.window, Qt::Key_PageDown);
        QTRY_VERIFY(counter->text() != initial);
        QTest::keyClick(&fixture.window, Qt::Key_PageUp);
        QTRY_COMPARE(counter->text(), initial);
        QCOMPARE(fixture.state().value("position"), position);
    }
    void dictionaryBookSearch_data() { screenshotVariants(); }
    void dictionaryBookSearch() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto point = fixture.wordPoint();
        const auto original = fixture.state().value("position");
        QVERIFY(fixture.selectWord(page, point));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        auto *headword = qobject_cast<QLabel *>(fixture.widget("rrDictionaryWord"));
        auto *query = qobject_cast<QLineEdit *>(fixture.widget("rrSearchQuery"));
        auto *lookup = qobject_cast<QLineEdit *>(fixture.widget("rrDictionaryQuery"));
        auto *action = qobject_cast<QPushButton *>(fixture.widget("rrDictionarySearch"));
        auto *status = qobject_cast<QLabel *>(fixture.widget("rrSearchStatus"));
        QVERIFY(headword && query && lookup && action && status);
        const auto selected = headword->text();
        QCOMPARE(action->accessibleName(), strings::get("dictionary_panel.search_in_book"));
        QVERIFY(action->text().isEmpty());
        QVERIFY(action->isVisible());
        QVERIFY(fixture.widget("rrDictionary")->rect().contains(action->geometry()));
        QVERIFY(!action->geometry().intersects(fixture.widget("rrDictionaryEdit")->geometry()));
        QCOMPARE(action->geometry().top(), fixture.widget("rrDictionaryEdit")->geometry().top());
        QCOMPARE(action->size(), fixture.widget("rrDictionaryEdit")->size());
        auto *selector = qobject_cast<QPushButton *>(fixture.widget("rrDictionarySelect"));
        QVERIFY(selector);
        QVERIFY(selector->width() > action->width());
        QCOMPARE(selector->height(), action->height());
        QVERIFY(selector->text().isEmpty());
        QVERIFY(!action->geometry().intersects(selector->geometry()));
        QVERIFY(fixture.widget("rrDefinition")->y() <= action->geometry().bottom() + 1 + int(ui::metrics::control::border));
        QVERIFY(fixture.press("rrDictionarySearch"));
        QTRY_VERIFY(fixture.visible("rrSearchResult0"));
        QCOMPARE(query->text(), selected);
        QVERIFY(!fixture.visible("rrDictionary"));
        QVERIFY(!query->hasFocus());
        for (auto *key : reader->findChildren<QPushButton *>("widgetKeyboardDone"))
            QVERIFY(!key->isVisible());
        QVERIFY(!page->annotationContains(point - QPoint(0, page->topHeight())));
        QCOMPARE(fixture.state().value("position"), original);
        QVERIFY(fixture.press("rrSearchBack"));
        QVERIFY(fixture.selectWord(page, point));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(fixture.press("rrDictionaryEdit"));
        QVERIFY(!action->isVisible());
        lookup->setText("  Trevelyan  ");
        QTest::keyClick(lookup, Qt::Key_Return);
        QTRY_VERIFY(action->isVisible());
        QCOMPARE(headword->text(), QString("Trevelyan"));
        // Cancelling an unfinished replacement keeps the current lookup term.
        QVERIFY(fixture.press("rrDictionaryEdit"));
        lookup->setText("discard this edit");
        QTest::keyClick(lookup, Qt::Key_Escape);
        QTRY_VERIFY(action->isVisible());
        QVERIFY(fixture.press("rrDictionarySearch"));
        QCOMPARE(query->text(), QString("Trevelyan"));
        QTRY_VERIFY(fixture.visible("rrSearchResult0"));
        QTRY_VERIFY(!status->text().contains("Searching", Qt::CaseInsensitive));
        QVERIFY(fixture.press("rrSearchResult0"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_VERIFY(!fixture.visible("rrSearch"));
        QVERIFY(fixture.state().value("position") != original);
        if (!fixture.visible("rrSearchOpen")) QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSearchOpen"));
        QVERIFY(fixture.press("rrSearchOpen"));
        QTRY_VERIFY(fixture.visible("rrSearchOriginal"));
        QCOMPARE(query->text(), QString("Trevelyan"));
        QVERIFY(fixture.press("rrSearchOriginal"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("position"), original);
        QVERIFY(!fixture.visible("rrSearch"));
        auto *panel = dynamic_cast<DictionaryPanel *>(fixture.widget("rrDictionary"));
        QVERIFY(panel);
        DictionaryResult result;
        result.received = result.restored = true;
        result.html = "<p>A definition.</p>";
        QRect wordBounds;
        for (const auto &name : {QString("English"), QString::fromUtf8("English to 日本語"), QString(100, 'W')}) {
            result.dictionary = name;
            panel->open("Trevelyan", result, {100, 100, 40, 40});
            QVERIFY(selector->width() > action->width());
            QCOMPARE(selector->height(), action->height());
            QVERIFY(selector->text().isEmpty());
            QCOMPARE(selector->accessibleDescription(), name);
            QCOMPARE(selector->toolTip(), name);
            const auto bounds = headword->geometry();
            if (!wordBounds.isNull()) QCOMPARE(bounds, wordBounds);
            wordBounds = bounds;
        }
    }
    void dictionaryBookSearchUnicode_data() { bookmarks_data(); }
    void dictionaryBookSearchUnicode() {
        QFETCH(bool, dark);
        bool suspended = false;
        Fixture fixture(dark, false, false, true, false, false, [&](ReaderHostServices &services) {
            services.suspended = [&] { return suspended; };
        });
        QVERIFY(fixture.prepared);
        const auto longTerm = QString::fromUtf8("é").repeated(140);
        auto contents = test::lookupBook();
        contents["one.xhtml"].replace("</body>", ("<p>" + longTerm + "</p></body>").toUtf8());
        QVERIFY(writeBook(fixture.directory.path(), "lookup.epub", contents));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(fixture.directory.path() + "/lookup.epub");
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *panel = dynamic_cast<DictionaryPanel *>(fixture.widget("rrDictionary"));
        auto *lookup = qobject_cast<QLineEdit *>(fixture.widget("rrDictionaryQuery"));
        auto *query = qobject_cast<QLineEdit *>(fixture.widget("rrSearchQuery"));
        auto *status = qobject_cast<QLabel *>(fixture.widget("rrSearchStatus"));
        QVERIFY(panel && lookup && query && status);
        const auto original = fixture.state().value("position");
        // Use real dictionary editing, reader dispatch, worker search and result rows.
        for (const auto &term : {QString::fromUtf8("café"), QString("Hello, world"), QString::fromUtf8("𐐀token")}) {
            panel->open("old lookup", {}, {100, 100, 40, 40});
            QVERIFY(fixture.press("rrDictionaryEdit"));
            lookup->setText(term);
            QVERIFY(fixture.press("rrDictionarySubmit"));
            QVERIFY(fixture.press("rrDictionarySearch"));
            QCOMPARE(query->text(), term);
            QTRY_VERIFY(fixture.visible("rrSearchResult0"));
            QVERIFY(fixture.widget("rrSearchResult0")->accessibleName().contains(term));
            QCOMPARE(fixture.state().value("position"), original);
            QVERIFY(fixture.press("rrSearchBack"));
        }
        panel->open(longTerm, {}, {100, 100, 40, 40});
        QVERIFY(fixture.press("rrDictionarySearch"));
        QCOMPARE(query->text(), longTerm);
        QTRY_VERIFY(fixture.visible("rrSearchResult0"));
        QVERIFY(fixture.widget("rrSearchResult0")->accessibleName().contains(longTerm));
        QVERIFY(fixture.press("rrSearchBack"));
        panel->open("Hello", {}, {100, 100, 40, 40});
        QVERIFY(fixture.press("rrDictionarySearch"));
        // Closing immediately must reject late batches and keep the keyboard hidden.
        QVERIFY(fixture.press("rrSearchBack"));
        panel->open("Hello", {}, {100, 100, 40, 40});
        auto *action = qobject_cast<QPushButton *>(fixture.widget("rrDictionarySearch"));
        QVERIFY(action);
        suspended = true;
        action->click();
        QVERIFY(!fixture.visible("rrSearch"));
        QVERIFY(panel->isVisible());
        suspended = false;
        fixture.window.readerWindow()->hide();
        action->click();
        QVERIFY(!fixture.visible("rrSearch"));
        fixture.window.readerWindow()->show();
        QCoreApplication::processEvents();
        QVERIFY(!fixture.visible("rrSearch"));
        QVERIFY(!fixture.visible("rrDictionary"));
        QCOMPARE(fixture.state().value("position"), original);
    }
    void dictionaryEditing_data() { bookmarks_data(); }
    void dictionaryEditing() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto word = fixture.wordPoint();
        const auto position = fixture.state().value("position");
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        auto *headword = qobject_cast<QLabel *>(fixture.widget("rrDictionaryWord"));
        auto *query = qobject_cast<QLineEdit *>(fixture.widget("rrDictionaryQuery"));
        QVERIFY(headword && query);
        const auto capture = qEnvironmentVariable("KOBALT_DICTIONARY_CAPTURE");
        const auto saveCapture = [&](const QString &state) {
            if (capture.isEmpty())
                return true;
            if (!QDir().mkpath(capture))
                return false;
            QCoreApplication::processEvents();
            const auto image = fixture.window.saveScreenshot(capture);
            return !image.isEmpty() && QFile::rename(image, QDir(capture).filePath(
                QString("%1-%2.png").arg(state, dark ? "dark" : "light")));
        };
        const auto selected = headword->text();
        const auto bounds = fixture.widget("rrDictionary")->geometry();
        QVERIFY(saveCapture("definition"));
        QVERIFY(fixture.press("rrDictionaryEdit"));
        QTRY_VERIFY(query->isVisible());
        QPushButton *dictionaryDone = nullptr;
        for (auto *button : reader->findChildren<QPushButton *>("widgetKeyboardDone")) {
            if (!button->isVisible()) continue;
            QVERIFY(!dictionaryDone);
            dictionaryDone = button;
        }
        QVERIFY(dictionaryDone);
        QCOMPARE(dictionaryDone->text(), QString::fromUtf8("✓"));
        QCOMPARE(query->text(), selected);
        QCOMPARE(fixture.widget("rrDictionary")->y(), 0);
        QCOMPARE(fixture.widget("rrDictionary")->height(), bounds.height());
        QVERIFY(fixture.visible("rrDefinition"));
        QVERIFY(fixture.visible("rrDictionaryEditHint"));
        QVERIFY(saveCapture("initial-editor"));
        query->setText(QString::fromUtf8("  oír  "));
        QVERIFY(saveCapture("editor"));
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_VERIFY(!query->isVisible());
        QCOMPARE(headword->text(), QString::fromUtf8("oír"));
        QVERIFY(saveCapture("submitted"));
        QCOMPARE(fixture.widget("rrDictionary")->geometry(), bounds);
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(page->annotationContains(word - QPoint(0, page->topHeight())));
        QVERIFY(fixture.press("rrDictionaryEdit"));
        QCOMPARE(query->text(), QString::fromUtf8("oír"));
        query->setText("discard this edit");
        QVERIFY(fixture.press("rrDictionaryClose"));
        QCOMPARE(headword->text(), QString::fromUtf8("oír"));
        QVERIFY(fixture.press("rrDictionaryEdit"));
        reader->hide();
        reader->show();
        QTRY_VERIFY(!fixture.visible("rrDictionary"));
        QVERIFY(!query->hasFocus());
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QCOMPARE(headword->text(), selected);
        QVERIFY(fixture.press("rrDictionaryEdit"));
        QCOMPARE(query->text(), selected);
        QVERIFY(fixture.press("rrDictionaryClose"));
        QVERIFY(fixture.press("rrDictionaryClose"));
        QTRY_VERIFY(!fixture.visible("rrDictionary"));
        QTRY_VERIFY(!page->annotationContains(word - QPoint(0, page->topHeight())));
    }
    void dictionaryDismissal_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("action");
        const QStringList actions{"left", "centre", "right", "close", "hide-reader"};
        for (bool dark : {false, true})
            for (int action = 0; action < actions.size(); ++action)
                QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light", actions[action]))) << dark << action;
    }
    void dictionaryDismissal() {
        QFETCH(bool, dark);
        QFETCH(int, action);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        auto *reader = fixture.window.readerWindow();
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        const auto word = fixture.wordPoint();
        QVERIFY(word.x() >= 0);
        const auto position = fixture.state().value("position");
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(page->annotationContains(word - QPoint(0, page->topHeight())));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        android::takeRefreshFrame(reader, canvas->viewport(), QRegion(page->rect()));
        if (action == 4) {
            auto *dictionary = fixture.widget("rrDictionary");
            reader->hide();
            const bool dismissedWhileHidden = dictionary->isHidden();
            reader->show();
            QVERIFY(dismissedWhileHidden);
            QTRY_VERIFY(!fixture.visible("rrDictionary"));
            QCOMPARE(fixture.state().value("position"), position);
            return;
        }
        if (action == 3) {
            QVERIFY(fixture.press("rrDictionaryClose"));
        } else {
            auto *dictionary = fixture.widget("rrDictionary");
            const int y = dictionary->y() > 0 ? dictionary->y() - ui::px(24)
                                             : dictionary->geometry().bottom() + ui::px(24);
            const auto point = page->mapFrom(reader, QPoint((1 + 4 * action) * reader->width() / 10, y));
            QVERIFY(!page->annotationContains(point - QPoint(0, page->topHeight())));
            QCOMPARE(reader->childAt(page->mapTo(reader, point)), page);
            QVERIFY(fixture.click(page, point));
        }
        QTRY_VERIFY(!fixture.visible("rrDictionary"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(!fixture.visible("rrControlsTop"));
        QTRY_VERIFY(!page->annotationContains(word - QPoint(0, page->topHeight())));
        QVERIFY(page->annotationHandleHitRegion().isEmpty());
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        const QRegion damage(page->rect());
        QVERIFY(android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
        QVERIFY(!android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
    }
    void wordSelection_data() { annotationSelection_data(); }
    void wordSelection() {
        QFETCH(bool, dark);
        QFETCH(int, resolution);
        Fixture fixture(dark);
        auto *picker = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        picker->setCurrentIndex(picker->findData(resolution));
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        auto *toggle = qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"));
        const auto first = fixture.wordPoint(0), later = fixture.wordPoint(3);
        QVERIFY(first.x() >= 0 && later.x() >= 0);
        const auto position = fixture.state().value("position");
        QVERIFY(fixture.selectWord(page, first));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        auto *headword = qobject_cast<QLabel *>(fixture.widget("rrDictionaryWord"));
        QVERIFY(headword);
        const auto word = headword->text();
        QCOMPARE(word.split(' ', Qt::SkipEmptyParts).size(), 1);
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
        QVERIFY(!page->annotationHandleHitRegion().isEmpty());
        QVERIFY(!toggle->isChecked());
        // Moving a held lookup across other words or past a page edge must
        // retain its original word and consume the whole gesture.
        for (const auto destination : {later, QPoint(10, page->height() / 2),
                                       QPoint(page->width() / 2, page->height() - 10)}) {
            QVERIFY(fixture.click(page, first));
            QTRY_VERIFY(fixture.visible("rrAnnotationCancel"));
            QVERIFY(fixture.press("rrAnnotationCancel"));
            QVERIFY(fixture.drag(page, first, destination, true));
            QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
            QTRY_VERIFY(fixture.visible("rrDictionary"));
            QCOMPARE(headword->text(), word);
            QVERIFY(page->annotationContains(first - QPoint(0, page->topHeight())));
            QVERIFY(!page->annotationContains(later - QPoint(0, page->topHeight())));
            QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
            QVERIFY(!page->annotationHandleHitRegion().isEmpty());
            QCOMPARE(fixture.state().value("position"), position);
            QVERIFY(!toggle->isChecked());
        }
        // Lookup handles adjust a phrase without enabling annotations or turning pages.
        const auto endGrip = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, endGrip, endGrip + later - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QTRY_VERIFY(headword->text().split(' ', Qt::SkipEmptyParts).size() >= 3);
        const auto phrase = headword->text();
        const int phraseWords = phrase.split(' ', Qt::SkipEmptyParts).size();
        const auto second = fixture.wordPoint(1);
        const auto startGrip = page->annotationHandleRect(true).center();
        QVERIFY(fixture.drag(page, startGrip, startGrip + second - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(fixture.visible("rrDictionary") && headword->text() != phrase);
        QVERIFY(headword->text().split(' ', Qt::SkipEmptyParts).size() < phraseWords);
        QVERIFY(!toggle->isChecked());
        QCOMPARE(fixture.stored("highlights.json", "highlights").size(), 0);
        QCOMPARE(fixture.state().value("position"), position);
        // A lookup grip stays on this page even when dragged past its edge.
        const auto edgeGrip = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, edgeGrip, QPoint(page->width() / 2, page->height() - 2)));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.press("rrDictionaryClose"));
        QTRY_VERIFY(page->annotationHandleRect(true).isEmpty() && page->annotationHandleRect(false).isEmpty());
        QVERIFY(!page->annotationContains(first - QPoint(0, page->topHeight())));
        QVERIFY(!toggle->isChecked());
        QCOMPARE(fixture.stored("highlights.json", "highlights").size(), 0);
        QVERIFY(fixture.selectWord(page, first));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        // Cancelling a grip must not schedule a lookup that reopens a dismissed sheet.
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        auto *reader = fixture.window.readerWindow();
        const auto grip = page->annotationHandleRect(false).center();
        const auto at = canvas->mapFromScene(page->mapTo(reader, grip));
        const auto moved = at + QPoint(30, 0);
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseMove(canvas->viewport(), moved);
        QTRY_VERIFY(fixture.state().value("annotation_dragging").toBool());
        page->cancelHold();
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, moved);
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        // The geometry query waits behind any earlier lookup work on the engine worker.
        QVERIFY(fixture.wordPoint(1).x() >= 0);
        QVERIFY(!fixture.visible("rrDictionary"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(!toggle->isChecked());
        QVERIFY(fixture.click(page, first));
        QTRY_VERIFY(fixture.visible("rrSelectionLookup"));
        QVERIFY(fixture.press("rrSelectionLookup"));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(fixture.click(page, first));
        QVERIFY(!fixture.visible("rrSelectionAnnotate"));
        QVERIFY(!fixture.visible("rrAnnotationStyles"));
        for (const auto *name : {"rrSelectionLookup", "rrAnnotationCancel"}) {
            auto *button = qobject_cast<QAbstractButton *>(fixture.widget(name));
            QVERIFY(button);
            QCOMPARE(ui::elideInterfaceText(button->text(), button->font(), button->width() - 2 * ui::px(16)),
                     button->text());
        }
        QVERIFY(fixture.startAnnotation(page, first));
        QTRY_VERIFY(fixture.visible("rrAnnotationStyle_shaded"));
        QVERIFY(toggle->isChecked());
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const auto end = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, end, end + later - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(page->annotationHandleRect(false).center() != end);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.click(page, first));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QVERIFY(fixture.stored("highlights.json", "highlights").first().toObject().value("text").toString().split(' ').size() >= 3);
        QVERIFY(page->annotationHandleRect(true).isEmpty());
        QVERIFY(toggle->isChecked());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(!toggle->isChecked());
        QVERIFY(!fixture.visible("rrControlsTop"));
        const auto next = fixture.wordPoint(12);
        QVERIFY(next.x() >= 0);
        QVERIFY(fixture.selectWord(page, next));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QCOMPARE(headword->text().split(' ', Qt::SkipEmptyParts).size(), 1);
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
        QVERIFY(!toggle->isChecked());
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.press("rrDictionaryClose"));
        QTRY_VERIFY(!page->annotationContains(next - QPoint(0, page->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(!toggle->isChecked());
    }
    void annotationSelection_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("resolution");
        for (int resolution : {300, 0})
            for (bool dark : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(resolution).arg(dark ? "dark" : "light"))) << dark << resolution;
    }
    void annotationSelection() {
        QFETCH(bool, dark);
        QFETCH(int, resolution);
        Fixture fixture(dark);
        auto *resolutionPicker = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        QVERIFY(resolutionPicker);
        resolutionPicker->setCurrentIndex(resolutionPicker->findData(resolution));
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        const QPoint first = fixture.wordPoint(0), second = fixture.wordPoint(1), fourth = fixture.wordPoint(3),
                     later = fixture.wordPoint(7);
        QVERIFY(first.x() >= 0 && second.x() >= 0 && fourth.x() >= 0 && later.x() >= 0);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto position = fixture.state().value("position");
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(!fixture.visible("rrAnnotationHeader"));
        const int bodyPage = fixture.state().value("page").toInt();
        QVERIFY(fixture.turnPage(-1));
        QTRY_VERIFY(fixture.state().value("page").toInt() < bodyPage && !fixture.state().value("busy").toBool());
        const auto division = fixture.wordRect("division"), published = fixture.wordRect("published"),
                   through = fixture.wordRect("through");
        QVERIFY(!division.isEmpty() && !published.isEmpty() && !through.isEmpty());
        QVERIFY(fixture.selectWord(page, division.center()));
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const QPoint divisionHandle = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, divisionHandle,
                             divisionHandle + QPoint(published.right() - division.right() + 20,
                                                     published.center().y() - division.center().y())));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QVERIFY2(page->annotationContains(published.center() - QPoint(0, page->topHeight())),
                 "Dragging past the line's final word must include published");
        const QPoint divisionStart = page->annotationHandleRect(true).center();
        QVERIFY(fixture.drag(page, divisionStart,
                             divisionStart + QPoint(through.left() - division.left() - 20,
                                                    through.center().y() - division.center().y())));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QVERIFY2(page->annotationContains(through.center() - QPoint(0, page->topHeight())),
                 "Dragging before the line's first word must include through");
        QVERIFY(fixture.click(page, division.center()));
        QTRY_VERIFY(fixture.visible("rrAnnotationCancel"));
        QVERIFY(fixture.press("rrAnnotationCancel"));
        QVERIFY(fixture.turnPage(1));
        QTRY_VERIFY(fixture.state().value("page").toInt() == bodyPage && !fixture.state().value("busy").toBool());
        QVERIFY(fixture.selectWord(page, first));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
        const auto originalStart = page->annotationHandleRect(true);
        const auto end = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, end, end + fourth - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(page->annotationHandleRect(false).center() != end);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        QCOMPARE(fixture.state().value("position"), position);
        QCOMPARE(page->annotationHandleRect(true), originalStart);
        QVERIFY(page->annotationHandleRect(false).center() != end);

        const auto fixedEnd = page->annotationHandleRect(false);
        const auto start = page->annotationHandleRect(true).center();
        QVERIFY(fixture.drag(page, start, start + second - first));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(page->annotationHandleRect(true) != originalStart);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(page->annotationHandleRect(false), fixedEnd);
        QVERIFY(page->annotationHandleRect(true) != originalStart);
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());

        const auto extend = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, extend, extend + later - fourth));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(page->annotationHandleRect(false) != fixedEnd);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.click(page, fourth));
        QTRY_VERIFY(fixture.visible("rrAnnotationStyle_underline"));
        auto *shaded = fixture.widget("rrAnnotationStyle_shaded");
        auto *underline = fixture.widget("rrAnnotationStyle_underline");
        auto *dotted = fixture.widget("rrAnnotationStyle_dotted");
        auto *cancel = fixture.widget("rrAnnotationCancel");
        QVERIFY(!fixture.widget("rrAnnotationDismiss"));
        auto *save = fixture.widget("rrAnnotationSave");
        const auto frame = [page](QWidget *widget) {
            auto *reader = page->window();
            return QRect(widget->mapTo(reader, QPoint()) - page->mapTo(reader, QPoint()), widget->size());
        };
        QCOMPARE(frame(shaded).right() + 1, frame(underline).left());
        QCOMPARE(frame(underline).right() + 1, frame(dotted).left());
        QCOMPARE(frame(save).right() + 1, frame(cancel).left());
        auto *styleSection = fixture.widget("rrAnnotationStyleSection");
        auto *actionsSection = fixture.widget("rrAnnotationActionsSection");
        QVERIFY(styleSection && actionsSection);
        QVERIFY(styleSection->isVisible() && actionsSection->isVisible());
        QVERIFY(frame(styleSection).bottom() < frame(shaded).top());
        if (auto *palette = fixture.widget("rrAnnotationPalette")) {
            auto *colorSection = fixture.widget("rrAnnotationColorSection");
            QVERIFY(colorSection && colorSection->isVisible());
            QCOMPARE(frame(colorSection).bottom() + 1, frame(palette).top());
            QCOMPARE(frame(palette).bottom() + 1, frame(styleSection).top());
            const auto swatches = palette->findChildren<QAbstractButton *>();
            QCOMPARE(swatches.size(), 5);
            int circleTop = -1;
            for (auto *swatch : swatches) {
                const auto image = swatch->grab().toImage();
                const auto paper = swatch->palette().color(QPalette::Window);
                QRect content, color;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const auto pixel = image.pixelColor(x, y);
                        if (pixel != paper)
                            content |= QRect(x, y, 1, 1);
                        if (qMax(pixel.red(), qMax(pixel.green(), pixel.blue())) -
                            qMin(pixel.red(), qMin(pixel.green(), pixel.blue())) > 30)
                            color |= QRect(x, y, 1, 1);
                    }
                }
                QVERIFY(!content.isEmpty());
                QVERIFY(qAbs(content.top() - (image.height() - 1 - content.bottom())) <= ui::px(10));
                if (!color.isEmpty()) {
                    if (circleTop >= 0)
                        QCOMPARE(color.top(), circleTop);
                    circleTop = color.top();
                }
            }
        }
        QCOMPARE(frame(shaded).bottom() + 1, frame(actionsSection).top());
        QCOMPARE(frame(actionsSection).bottom() + 1, frame(cancel).top());
        QCOMPARE(frame(shaded).left(), frame(fixture.widget("rrAnnotationSave")).left());
        QCOMPARE(frame(dotted).right(), frame(cancel).right());
        for (const auto *name : {"rrAnnotationSave", "rrAnnotationCancel"}) {
            auto *button = dynamic_cast<ui::CaptionButton *>(fixture.widget(name));
            QVERIFY(button && button->isIconAboveLabel());
            QCOMPARE(ui::elideInterfaceText(button->text(), button->font(), button->width() - 2 * ui::px(16)),
                     button->text());
            QVERIFY(ui::metrics::chrome::cellIcon + ui::px(8) + QFontMetrics(button->font()).height() +
                    2 * ui::px(8) <= button->height());
        }
        for (auto *header : {styleSection, actionsSection}) {
            const auto image = header->grab().toImage();
            QCOMPARE(image.pixelColor(image.width() - 10, image.height() / 2), header->palette().color(QPalette::Window));
            QCOMPARE(image.pixelColor(image.width() - 10, image.height() - 1), ui::dividerColor(header));
        }
        const auto paper = shaded->palette().color(QPalette::Window);
        const auto selectedSample = shaded->grab().toImage();
        QCOMPARE(selectedSample.pixelColor(selectedSample.width() / 2, selectedSample.height() - ui::px(6)), paper);

        QVERIFY2(cancel->height() >= QFontMetrics(cancel->font()).height() + 2 * ui::px(20),
                 qPrintable(QString("Action height %1, font height %2, font pixels %3, padding %4")
                     .arg(cancel->height()).arg(QFontMetrics(cancel->font()).height()).arg(cancel->font().pixelSize()).arg(ui::px(20))));

        const QRect selection = page->annotationHandleRect(true).united(page->annotationHandleRect(false));
        auto *card = fixture.widget("rrAnnotationStyles")->parentWidget();
        const auto cardBounds = QRect(page->mapFromGlobal(card->mapToGlobal(QPoint())), card->size());
        QVERIFY(cardBounds.bottom() <= selection.top() || cardBounds.top() >= selection.bottom());
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        QVERIFY(fixture.press("rrAnnotationStyle_underline"));
        const QRect check(shaded->width() - ui::px(40), ui::px(10), ui::px(24), ui::px(24));
        QVERIFY(selectedSample.copy(check) != shaded->grab().toImage().copy(check));
        const auto selectedUnderline = underline->grab().toImage();
        QCOMPARE(selectedUnderline.pixelColor(selectedUnderline.width() / 2, selectedUnderline.height() - ui::px(6)), paper);
        QVERIFY(fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        // A tap on the selection dismisses the open card instead of rebuilding it.
        QVERIFY(fixture.click(page, fourth));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.click(page, fourth));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotationStyle_underline"))->isChecked());
        for (const auto *name : {"rrAnnotationPalette", "rrAnnotationStyles", "rrAnnotationSave", "rrAnnotationCancel"}) {
            auto *item = fixture.widget(name);
            if (!item)
                continue; // Grayscale omits the color palette.
            QVERIFY(item->isVisible());
            QVERIFY(item->visibleRegion().contains(item->rect()));
            QVERIFY(card->rect().contains(QRect(item->mapTo(card, QPoint()), item->size())));
        }
        QVERIFY(fixture.click(page, QPoint(10, page->height() / 2)));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.click(page, fourth));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        const auto saved = fixture.stored("highlights.json", "highlights").first().toObject();
        QCOMPARE(saved.value("style").toString(), QString("underline"));
        QVERIFY(saved.value("text").toString().split(' ').size() >= 4);
        QVERIFY(page->annotationHandleRect(true).isEmpty() && page->annotationHandleRect(false).isEmpty());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        // Outside annotation mode, saved passages use ordinary word lookup.
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.selectWord(page, second));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(!fixture.visible("rrAnnotationEdit"));
        QVERIFY(!fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrDictionaryClose"));
        QVERIFY(page->annotationHandleRect(true).isEmpty() && page->annotationHandleRect(false).isEmpty());
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));

        const auto previousPage = fixture.state().value("page").toInt();
        QVERIFY(fixture.drag(page, first, first - QPoint(qMax(120, page->width() / 4), 0)));
        QTRY_VERIFY(fixture.state().value("page").toInt() != previousPage);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject(), saved);
        QVERIFY(fixture.turnPage(-1));
        QTRY_VERIFY(fixture.state().value("page").toInt() == previousPage && !fixture.state().value("busy").toBool());
        QTest::qWait(210);
        QVERIFY(fixture.drag(page, QPoint(page->width() - 10, page->height() / 2),
                             QPoint(page->width() / 2, page->height() / 2)));
        QTRY_VERIFY(fixture.state().value("page").toInt() != previousPage);
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
    }
    void coordinateProbeSerialization() {
        Fixture fixture;
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        struct Gate {
            QSemaphore release;
            std::atomic_bool entered{false}, finished{false};
        };
        auto gate = std::make_shared<Gate>();
        auto unblock = qScopeGuard([gate] { gate->release.release(); });
        qt::invoke(engineWorker(), [gate] {
            EngineJob activity;
            gate->entered.store(true);
            gate->release.acquire();
            gate->finished.store(true);
        }, Qt::QueuedConnection);
        QTRY_VERIFY(gate->entered.load());
        qt::later(20, qApp, [gate] { gate->release.release(); });
        const auto result = fixture.coordinateQuery(book, QPair<bool, bool>{false, false}, [gate](ReaderModel &model) {
            return qMakePair(QThread::currentThread() == engineWorker()->thread(),
                             gate->finished.load() && model.pages() > 0);
        });
        QVERIFY2(result.first, "Coordinate probes must use the reader's engine worker");
        QVERIFY2(result.second, "A probe must wait behind existing engine work without blocking GUI callbacks");
    }
    void annotationGripMenus_data() {
        QTest::addColumn<bool>("toolbarEntry");
        QTest::newRow("lookup-then-toolbar-native") << false;
        QTest::newRow("annotation-300") << true;
    }
    void annotationGripMenus() {
        QFETCH(bool, toolbarEntry);
        Fixture fixture(toolbarEntry);
        auto *picker = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        picker->setCurrentIndex(picker->findData(toolbarEntry ? 300 : 0));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto first = fixture.wordPoint(), second = fixture.wordPoint(1), fourth = fixture.wordPoint(3);
        QVERIFY(first.x() >= 0 && second.x() >= 0 && fourth.x() >= 0);
        const auto position = fixture.state().value("position");
        if (toolbarEntry) {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrAnnotate"));
        }
        QVERIFY(fixture.selectWord(page, first));
        if (!toolbarEntry) {
            QTRY_VERIFY(fixture.visible("rrDictionary"));
            QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
            QVERIFY(!fixture.visible("rrSelectionAnnotate"));
            QVERIFY(fixture.startAnnotation(page, first));
            QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        }
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        for (const bool start : {false, true}) {
            // Grips stay usable through the marking palette for either entry route.
            if (!fixture.visible("rrAnnotationPopover"))
                QVERIFY(fixture.click(page, first));
            QTRY_VERIFY(fixture.visible("rrAnnotationPopover"));
            const auto moving = page->annotationHandleRect(start);
            const auto fixed = page->annotationHandleRect(!start);
            QVERIFY(!moving.isEmpty() && !fixed.isEmpty());
            bool recognized = false;
            auto original = page->annotationDragStarted;
            page->annotationDragStarted = [&](QPoint point, int boundary) {
                recognized = boundary == (start ? 0 : 1);
                return original(point, boundary);
            };
            const bool dragged = fixture.drag(page, moving.center(),
                moving.center() + (start ? second : fourth) - first);
            page->annotationDragStarted = original;
            QVERIFY(dragged);
            QVERIFY2(recognized, "An open selection menu must pass handle drags to the page");
            QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
            QTRY_VERIFY(page->annotationHandleRect(start) != moving);
            QCOMPARE(page->annotationHandleRect(!start), fixed);
            QCOMPARE(fixture.state().value("position"), position);
            QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
            QVERIFY(!fixture.visible("rrAnnotationPopover"));
        }
        if (!fixture.visible("rrAnnotationPopover"))
            QVERIFY(fixture.click(page, second));
        QTRY_VERIFY(fixture.visible("rrAnnotationPopover"));
        const auto kept = page->annotationHandleRect(true);
        QVERIFY(fixture.click(page, QPoint(10, page->height() / 2)));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(page->annotationHandleRect(true), kept);
        QCOMPARE(fixture.state().value("position"), position);
        QCOMPARE(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked(), true);
    }
    void annotationCrossPageDrag_data() {
        QTest::addColumn<bool>("startHandle");
        QTest::addColumn<int>("direction");
        QTest::newRow("start-backward") << true << -1;
        QTest::newRow("start-forward") << true << 1;
        QTest::newRow("end-backward") << false << -1;
        QTest::newRow("end-forward") << false << 1;
    }
    void annotationCrossPageDrag() { checkCrossPageDrag(true); }
    void selectionCrossPageDrag_data() { annotationCrossPageDrag_data(); }
    void selectionCrossPageDrag() { checkCrossPageDrag(false); }
    void annotationSectionBoundary_data() { annotationCrossPageDrag_data(); }
    void annotationSectionBoundary() { checkCrossPageDrag(true, true); }
    void checkCrossPageDrag(bool toolbarEntry, bool sectionBoundary = false) {
        QFETCH(bool, startHandle);
        QFETCH(int, direction);
        Fixture fixture(startHandle);
        auto *picker = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        picker->setCurrentIndex(picker->findData(startHandle ? 0 : 300));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        // Use chapter-interior pages for continuation, and the two sides of
        // the Prologue/Chapter One boundary for the section clamp.
        const int advance = sectionBoundary ? (direction > 0 ? 1 : 2) : 3;
        for (int i = 0; i < advance; ++i) {
            QVERIFY(fixture.turnPage(1));
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
        }
        const int origin = fixture.state().value("page").toInt();
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        if (toolbarEntry) {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrAnnotate"));
        }
        const auto first = fixture.wordPoint();
        QVERIFY(first.x() >= 0);
        QVERIFY(fixture.selectWord(page, first));
        if (!toolbarEntry) {
            QTRY_VERIFY(fixture.visible("rrDictionary"));
            QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
            QVERIFY(!fixture.visible("rrSelectionAnnotate"));
            QVERIFY(fixture.startAnnotation(page, first));
            QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        }
        QTRY_VERIFY(!page->annotationHandleRect(startHandle).isEmpty());
        const auto edges = fixture.pageEdges();
        QCOMPARE(direction > 0 ? edges.continuesAfter : edges.continuesBefore, !sectionBoundary);
        QVERIFY(!edges.first.text.isEmpty() && !edges.last.text.isEmpty());
        const auto edge = direction > 0 ? edges.end : edges.start;
        QVERIFY(edge.visible);
        const QPoint target = (edge.first + edge.last) / 2 + edge.outward * ui::px(20);
        const auto handle = page->annotationHandleRect(startHandle).center();
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        const auto map = [&](QPoint point) { return canvas->mapFromScene(page->mapTo(reader, point)); };
        // Preserve the handle-to-word offset used by the production pointer path.
        const QPoint offset = handle - fixture.selectionHandleTarget(first, startHandle);
        const QPoint exit = target + QPoint(0, page->topHeight()) + offset;
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, map(handle));
        QTest::mouseMove(canvas->viewport(), map(exit));
        if (sectionBoundary) {
            QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
            QTest::qWait(800);
            QCOMPARE(fixture.state().value("page").toInt(), origin);
            const auto boundary = direction > 0 ? edges.last : edges.first;
            QVERIFY(page->annotationContains(boundary.rectangles.first().center()));
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, map(exit));
            QCOMPARE(fixture.state().value("page").toInt(), origin);
            return;
        }
        const bool turned = QTest::qWaitFor([&] { return fixture.state().value("page").toInt() == origin + direction &&
                                                         !fixture.state().value("busy").toBool(); }, 5000);
        if (!turned)
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, map(exit));
        QVERIFY(turned);
        QTRY_VERIFY(!(direction > 0 ? page->annotationHandleRect(false) : page->annotationHandleRect(true)).isEmpty());
        auto destination = fixture.pageEdges();
        auto selected = direction > 0 ? destination.first : destination.last;
        if (direction < 0)
            QVERIFY2(!selected.punctuation, "Backward extension must stop on the last word, not punctuation");
        QVERIFY(page->annotationContains(selected.rectangles.first().center()));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        const auto stoppedStart = page->annotationHandleRect(true), stoppedEnd = page->annotationHandleRect(false);
        // Keep the same press held past the long-press delay, then move back
        // into the text and out again. None of this may restart the stopped drag.
        QTest::qWait(800);
        const auto inside = fixture.wordPoint(2);
        QTest::mouseMove(canvas->viewport(), map(inside + offset));
        const auto destinationEdge = direction > 0 ? destination.end : destination.start;
        const auto beyond = (destinationEdge.first + destinationEdge.last) / 2 +
                            destinationEdge.outward * ui::px(20) + QPoint(0, page->topHeight()) + offset;
        QTest::mouseMove(canvas->viewport(), map(beyond));
        QTest::qWait(200);
        QCOMPARE(fixture.state().value("page").toInt(), origin + direction);
        QVERIFY(!fixture.state().value("annotation_dragging").toBool());
        QCOMPARE(page->annotationHandleRect(true), stoppedStart);
        QCOMPARE(page->annotationHandleRect(false), stoppedEnd);
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, map(beyond));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        int expectedPage = origin + direction;
        QCOMPARE(fixture.state().value("page").toInt(), expectedPage);
        if (!startHandle && direction > 0) {
            // A fresh grip drag can extend the same selection to another page.
            const auto grip = page->annotationHandleRect(false).center();
            const auto selectedPoint = selected.rectangles.first().center() + QPoint(0, page->topHeight());
            const auto freshOffset = grip - fixture.selectionHandleTarget(selectedPoint, false);
            const auto nextExit = (destination.end.first + destination.end.last) / 2 +
                                  destination.end.outward * ui::px(20) + QPoint(0, page->topHeight()) + freshOffset;
            QVERIFY(fixture.drag(page, grip, nextExit));
            ++expectedPage;
            QTRY_VERIFY(fixture.state().value("page").toInt() == expectedPage && !fixture.state().value("busy").toBool());
            QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
            destination = fixture.pageEdges();
            selected = destination.first;
            QTRY_VERIFY(page->annotationContains(selected.rectangles.first().center()));
        }
        // Both explicit entry routes keep annotation mode active after saving.
        if (!fixture.visible("rrAnnotationPopover"))
            QVERIFY(fixture.click(page, selected.rectangles.first().center() + QPoint(0, page->topHeight())));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QVERIFY(fixture.stored("highlights.json", "highlights").first().toObject().value("text").toString().size() > 100);
        QVERIFY(fixture.press("rrPage"));
        auto *toggle = qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"));
        QVERIFY(toggle);
        QCOMPARE(toggle->isChecked(), true);
    }
    void annotationCrossPageOverlap_data() {
        QTest::addColumn<int>("direction");
        QTest::newRow("backward") << -1;
        QTest::newRow("forward") << 1;
    }
    void annotationCrossPageOverlap() {
        QFETCH(int, direction);
        Fixture fixture;
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        for (int i = 0; i < 3; ++i) {
            QVERIFY(fixture.turnPage(1));
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
        }
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto edges = fixture.pageEdges();
        const auto word = direction > 0 ? edges.first : edges.last;
        QVERIFY(!word.rectangles.isEmpty());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        const auto marked = word.rectangles.first().center() + QPoint(0, page->topHeight());
        QVERIFY(fixture.selectWord(page, marked));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QVERIFY(fixture.click(page, marked));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        const auto saved = fixture.stored("highlights.json", "highlights");
        QVERIFY(fixture.turnPage(-direction));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const int origin = fixture.state().value("page").toInt();
        const auto first = fixture.wordPoint();
        QVERIFY(fixture.selectWord(page, first));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        const bool start = direction < 0;
        const auto handle = page->annotationHandleRect(start).center();
        const auto bounds = fixture.pageEdges();
        const auto edge = direction > 0 ? bounds.end : bounds.start;
        const auto exit = (edge.first + edge.last) / 2 + edge.outward * ui::px(20) +
                          QPoint(0, page->topHeight()) + handle - fixture.selectionHandleTarget(first, start);
        QVERIFY(fixture.drag(page, handle, exit));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool() && !fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("page").toInt(), origin);
        QTRY_VERIFY(!page->annotationHandleRect(start).isEmpty());
        QVERIFY(page->annotationContains(first - QPoint(0, page->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationOverlap"));
        QCOMPARE(fixture.stored("highlights.json", "highlights"), saved);
        // Moves before the boundary remain valid. The blocked turn must not
        // include the saved passage, and the remaining draft can still be saved.
        QVERIFY(fixture.click(page, first));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 2);
        QVERIFY(fixture.stored("highlights.json", "highlights").contains(saved.first()));
    }
    void annotationOverlap_data() { bookmarks_data(); }
    void annotationOverlap() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const QPoint first = fixture.wordPoint(0), second = fixture.wordPoint(1), fourth = fixture.wordPoint(3),
                     beyond = fixture.wordPoint(12);
        QVERIFY(first.x() >= 0 && second.x() >= 0 && fourth.x() >= 0 && beyond.x() >= 0);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.selectWord(page, second));
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const QPoint handle = page->annotationHandleRect(false).center();
        QVERIFY(fixture.drag(page, handle, handle + fourth - second));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QVERIFY(fixture.click(page, second));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        const auto saved = fixture.stored("highlights.json", "highlights").first().toObject();
        // Both annotation entry routes reject crossing another saved passage.
        // Check both handles, including moves beyond the whole mark.
        for (bool marking : {true, false}) {
            for (bool fromLeft : {true, false}) {
                const QPoint word = fromLeft ? first : beyond;
                QVERIFY(fixture.selectWord(page, word));
                if (!marking) {
                    QTRY_VERIFY(fixture.visible("rrDictionary"));
                    QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
                    QVERIFY(!fixture.visible("rrSelectionAnnotate"));
                    QVERIFY(fixture.startAnnotation(page, word));
                }
                QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
                const auto start = page->annotationHandleRect(true), end = page->annotationHandleRect(false);
                const QPoint grip = (fromLeft ? end : start).center();
                QVERIFY(fixture.drag(page, grip, grip + (fromLeft ? beyond : first) - word));
                QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
                QCOMPARE(fromLeft ? page->annotationHandleRect(true) : page->annotationHandleRect(false),
                         fromLeft ? start : end);
                QVERIFY(!page->annotationContains(second - QPoint(0, page->topHeight())));
                QVERIFY(!page->annotationContains(fourth - QPoint(0, page->topHeight())));
                QVERIFY(!fixture.visible("rrAnnotationOverlap"));
                QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject(), saved);
                if (!fixture.visible("rrAnnotationCancel"))
                    QVERIFY(fixture.click(page, word));
                QTRY_VERIFY(fixture.visible("rrAnnotationCancel"));
                QVERIFY(fixture.press("rrAnnotationCancel"));
                if (!marking) {
                    QVERIFY(fixture.press("rrPage"));
                    QVERIFY(fixture.press("rrAnnotate"));
                    QVERIFY(!fixture.visible("rrControlsTop"));
                }
            }
            if (marking) {
                QVERIFY(fixture.press("rrPage"));
                QVERIFY(fixture.press("rrAnnotate"));
                QVERIFY(!fixture.visible("rrControlsTop"));
            }
        }
        // Adjust may change its own saved range, but cannot cross another mark.
        QVERIFY(fixture.selectWord(page, beyond));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QVERIFY(!fixture.visible("rrSelectionAnnotate"));
        QVERIFY(fixture.startAnnotation(page, beyond));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 2);
        const auto highlights = fixture.stored("highlights.json", "highlights");
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QVERIFY(fixture.selectWord(page, beyond));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(!fixture.visible("rrAnnotationEdit"));
        QVERIFY(fixture.startAnnotation(page, beyond));
        QTRY_VERIFY(fixture.visible("rrAnnotationEdit"));
        QVERIFY(!fixture.visible("rrAnnotationSave"));
        QVERIFY(page->annotationHandleRect(true).isEmpty() && page->annotationHandleRect(false).isEmpty());
        QVERIFY(fixture.press("rrAnnotationEdit"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty() && !page->annotationHandleRect(false).isEmpty());
        const QPoint grip = page->annotationHandleRect(true).center();
        QVERIFY(fixture.drag(page, grip, grip + first - beyond));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QVERIFY(!page->annotationContains(second - QPoint(0, page->topHeight())));
        QVERIFY(!page->annotationContains(fourth - QPoint(0, page->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationOverlap"));
        QCOMPARE(fixture.stored("highlights.json", "highlights"), highlights);
    }
    void headerFooterSizes_data() { screenshotVariants(); }
    void headerFooterSizes() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        checkHeaderFooterSizes(preset, dark);
    }
    void batteryPercentageOnly_data() { bookmarks_data(); }
    void batteryPercentageOnly() {
        using namespace headerfooter;
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavLayout"));
        QVERIFY(fixture.press("rrHeaderFooter"));
        QTRY_VERIFY(fixture.visible("rrHeaderFooterView"));
        QVERIFY(fixture.press("rrHeaderFooterSlot2"));
        QTRY_VERIFY(fixture.visible("rrHeaderFooterChooser"));
        for (int turns = 0; !fixture.visible("rrHeaderFooterField_battery_percent_only") && turns < 10; ++turns) {
            QVERIFY(fixture.press("rrHeaderFooterFieldsNext"));
        }
        QVERIFY(fixture.visible("rrHeaderFooterField_battery_percent_only"));
        auto *choice = qobject_cast<QCheckBox *>(fixture.widget("rrHeaderFooterField_battery_percent_only"));
        QVERIFY(choice);
        QCOMPARE(choice->text(), strings::get("header_footer.field.battery_percent_only"));
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto saveCapture = [&](const QString &name) {
            const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
            if (output.isEmpty()) return true;
            QCoreApplication::processEvents();
            auto *surface = fixture.window.readerWindow();
            for (auto *child : surface->findChildren<QWidget *>()) {
        auto *modal = dynamic_cast<ui::ModalOverlay *>(child);
        if (!modal || !modal->isVisible()) continue;
        auto *card = modal->card();
        if (!surface->rect().contains(QRect(card->mapTo(surface, QPoint()), card->size()))) {
            qCritical() << "Modal card extends outside the captured screen:" << name << modal->objectName();
            return false;
        }
    }
    QImage pixels(surface->size(), QImage::Format_ARGB32_Premultiplied);
            pixels.fill(Qt::transparent);
            surface->render(&pixels);
            return QDir().mkpath(output) && pixels.save(output + '/' + name + (dark ? "-dark.png" : "-light.png"));
        };
        QVERIFY(saveCapture("battery-percentage-chooser"));
        QVERIFY(fixture.press("rrHeaderFooterField_battery_percent_only"));
        QTRY_VERIFY(!fixture.visible("rrHeaderFooterChooser"));
        QTRY_COMPARE(page->chrome.slotFields[2], Field::BatteryPercentOnly);
        QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value(customKey).toObject()
                         .value("top_right").toString(), QString("battery_percent_only"));
        QVERIFY(saveCapture("battery-percentage-layout"));
        QVERIFY(fixture.press("rrHeaderFooterClose"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(page->slotContents[2].text, strings::get("common.percent").arg(100));
        QVERIFY(!page->slotContents[2].batteryIcon);
        QVERIFY(saveCapture("battery-percentage-reading"));
        fixture.window.readerWindow()->close();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QCOMPARE(page->chrome.slotFields[2], Field::BatteryPercentOnly);
    }
    void headerFooterDismissal_data() { bookmarks_data(); }
    void headerFooterDismissal() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        fixture.window.resize(1000, 1600);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavLayout"));
        QVERIFY(fixture.press("rrHeaderFooter"));
        QTRY_VERIFY(fixture.visible("rrHeaderFooterView"));
        const auto position = fixture.state().value("position");

        auto *screenshot = fixture.window.findChild<QPushButton *>("desktopScreenshot");
        QVERIFY(screenshot && screenshot->isVisible());
        {
            // Exercise the real toolbar click without writing to the user's
            // Desktop. Use the same capture method with a temporary directory.
            const QSignalBlocker blocked(screenshot);
            QTest::mouseClick(screenshot, Qt::LeftButton);
        }
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QTemporaryDir captures;
        QVERIFY(captures.isValid());
        QString error;
        const auto captured = fixture.window.saveScreenshot(captures.path(), &error);
        QVERIFY2(!captured.isEmpty(), qPrintable(error));
        QVERIFY(!QImage(captured).isNull());
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        auto *frame = fixture.window.findChild<QWidget *>("desktopPreviewToolbar");
        QTest::mouseClick(frame, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QTest::mouseClick(&fixture.window, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
        QVERIFY(fixture.visible("rrHeaderFooterView"));

        auto *page = fixture.widget("rrPage");
        auto *reader = fixture.window.readerWindow();
        QPoint exposed(-1, -1);
        for (int y = 0; y < page->height(); y += 10) {
            const QPoint point(page->width() / 2, y);
            if (reader->childAt(page->mapTo(reader, point)) == page) {
                exposed = point;
                break;
            }
        }
        QVERIFY(exposed.x() >= 0);
        QVERIFY(fixture.click(page, exposed));
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        auto *device = QTest::createTouchDevice();
        auto touch = QTest::touchEvent(page, device, false);
        touch.press(0, exposed).commit();
        touch.release(0, exposed).commit();
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.press("rrHeaderFooterPreset_alternative_a"));
        QTRY_COMPARE(fixture.state().value("header_footer_preset").toString(), QString("alternative_a"));
        QVERIFY(fixture.visible("rrHeaderFooterView"));
        QVERIFY(fixture.press("rrHeaderFooterClose"));
        QTRY_VERIFY(!fixture.visible("rrHeaderFooterView"));
        QVERIFY(fixture.visible("rrHeaderFooter"));
    }
    void annotationModeFeedback_data() { bookmarks_data(); }
    void annotationModeFeedback() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        for (const bool enabled : {true, false, true, false}) {
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrAnnotate"));
            QVERIFY(fixture.press("rrAnnotate"));
            QTRY_VERIFY(!fixture.visible("rrControlsTop"));
            QCOMPARE(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked(), enabled);
            QTRY_VERIFY(fixture.visible("rrAnnotationModeNotice"));
            auto *notice = fixture.widget("rrAnnotationModeNotice");
            QVERIFY(notice->testAttribute(Qt::WA_TransparentForMouseEvents));
            QCOMPARE(notice->findChild<QLabel *>("rrAnnotationModeState")->text(),
                     strings::get(enabled ? "common.on" : "common.off"));
            QTRY_VERIFY(!fixture.visible("rrAnnotationModeNotice"));
        }
        auto *page = fixture.widget("rrPage");
        QVERIFY(fixture.selectWord(page, fixture.wordPoint()));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QVERIFY(!fixture.visible("rrAnnotationModeNotice"));
    }
    void annotations() {
        QFETCH(bool, dark);
        QFETCH(bool, pickerExport);
        QFETCH(bool, markdownExport);
        const char *exportAction = markdownExport ? "rrAnnotationExportMarkdown" : "rrAnnotationExportHtml";
        Fixture fixture(dark, false, false, true, !pickerExport);
        QVERIFY(fixture.prepared);
        QVERIFY(storage::writeJson(fixture.paths.preferences(), {{"dark_mode", dark}, {"highlight_help_seen", false}}));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        const QPoint word = fixture.wordPoint();
        QVERIFY(word.x() >= 0);
        const QPoint end = fixture.wordPoint(1);
        QVERIFY(end.x() >= 0 && end != word);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto position = fixture.state().value("position");
        const auto image = page->image;
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrAnnotate"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(!fixture.window.readerWindow()->findChild<QWidget *>("rrAnnotationHeader"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QCOMPARE(page->image, image);
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(page->annotationHandleRect(true).isEmpty());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(!fixture.visible("rrAnnotationHeader"));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const QRect draftStart = page->annotationHandleRect(true), draftEnd = page->annotationHandleRect(false);
        QVERIFY(fixture.click(page, QPoint(page->width() / 2, page->height() / 2)));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_COMPARE(page->annotationHandleRect(true), draftStart);
        QTRY_COMPARE(page->annotationHandleRect(false), draftEnd);
        QVERIFY(fixture.click(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationColor_yellow"));
        QVERIFY(fixture.press("rrAnnotationColor_yellow"));
        QVERIFY(fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        const auto saved = fixture.stored("highlights.json", "highlights").first().toObject();
        QVERIFY(!saved.value("text").toString().isEmpty());
        QVERIFY(!saved.value("start").toString().isEmpty());
        QVERIFY(!saved.value("end").toString().isEmpty());
        QCOMPARE(fixture.state().value("position"), position);
        QCOMPARE(page->image, image);
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationEdit"));
        QTRY_VERIFY(fixture.visible("rrAnnotationColor_blue"));
        QVERIFY(fixture.press("rrAnnotationColor_blue"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject(), saved);
        {
            BlockedFile blocked(fixture.storedPath("highlights.json"));
            QVERIFY(blocked.active());
            // The change shows at once. A failed save returns to the saved
            // highlight and says so.
            QVERIFY(fixture.press("rrAnnotationApply"));
            QTRY_VERIFY(fixture.visible("rrHighlightError"));
            QVERIFY(blocked.restore());
        }
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject(), saved);
        QVERIFY(fixture.press("rrHighlightErrorConfirm"));
        QTRY_VERIFY(!fixture.visible("rrHighlightError"));
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationColor_blue"));
        QVERIFY(fixture.press("rrAnnotationColor_blue"));
        QVERIFY(fixture.press("rrAnnotationApply"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("color").toString(), QString("blue"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("id"), saved.value("id"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationEdit"));
        QVERIFY(!fixture.visible("rrAnnotationSave"));
        QVERIFY(fixture.press("rrAnnotationEdit"));
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        const auto handle = page->annotationHandleRect(false).center();
        // Expansion requires crossing the next word's far edge. A distance
        // between word centres can stop inside a longer word after reflow.
        const QPoint target = handle + fixture.selectionHandleTarget(end, false) -
                              fixture.selectionHandleTarget(word, false) + QPoint(ui::px(8), 0);
        QVERIFY(fixture.drag(page, handle, target));
        QTRY_VERIFY(!fixture.state().value("annotation_dragging").toBool());
        QTRY_VERIFY(!page->annotationHandleRect(false).isEmpty());
        QTRY_VERIFY(page->annotationContains(end - QPoint(0, page->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.click(page, end));
        QTRY_VERIFY(fixture.visible("rrAnnotationStyle_dotted"));
        QVERIFY(fixture.press("rrAnnotationStyle_dotted"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QTRY_VERIFY(fixture.stored("highlights.json", "highlights").first().toObject().value("text") != saved.value("text"));
        QVERIFY(fixture.stored("highlights.json", "highlights").first().toObject().value("text").toString().contains(saved.value("text").toString()));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("id"), saved.value("id"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("style").toString(), QString("dotted"));
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationColor_blue"));
        QVERIFY(fixture.press("rrAnnotationColor_blue"));
        QVERIFY(fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.press("rrAnnotationApply"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").first().toObject().value("style").toString(), QString("dotted"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationHeader"));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrControlsTop"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrAnnotationHeader"));
        QVERIFY(!qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"))->isChecked());
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBookmarks"));
        QVERIFY(fixture.press("rrBookmarks"));
        auto *annotationsTab = qobject_cast<QPushButton *>(fixture.widget("rrAnnotationsTab"));
        auto *bookmarksTab = qobject_cast<QPushButton *>(fixture.widget("rrBookmarksTab"));
        QVERIFY(annotationsTab && bookmarksTab);
        QVERIFY(annotationsTab->isChecked());
        QTRY_VERIFY(fixture.widget("rrAnnotationMore")->isEnabled());
        QVERIFY(!fixture.widget("rrAnnotationExportHtml"));
        QVERIFY(fixture.widget("rrAnnotationMore")->x() > fixture.widget("rrMarksSort")->x());
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportHtml"));
        QCOMPARE(qobject_cast<QPushButton *>(fixture.widget("rrAnnotationExportHtml"))->text(), strings::get("annotations.export_html"));
        auto *actions = fixture.widget("rrAnnotationActions");
        auto *reader = fixture.window.readerWindow();
        QVERIFY(actions && actions->parentWidget() == reader);
        QCOMPARE(actions->geometry(), reader->rect());
        QVERIFY(!fixture.widget("rrBookmarkExport"));
        QVERIFY(fixture.widget("rrAnnotationExportMarkdown")->isEnabled());
        if (!pickerExport) {
            auto *canvas = fixture.window.findChild<QGraphicsView *>();
            QVERIFY(canvas);
            for (const char *name : {"rrAnnotationExportMarkdown", "rrAnnotationExportHtml"}) {
                QPointer<QPushButton> action = qobject_cast<QPushButton *>(fixture.widget(name));
                QVERIFY(action);
                const auto sample = [&] {
                    const auto image = canvas->viewport()->grab().toImage();
                    const auto point = canvas->mapFromScene(action->mapTo(reader, QPoint(action->width() - 10, action->height() / 3)));
                    return image.pixelColor(qRound(point.x() * image.devicePixelRatio()), qRound(point.y() * image.devicePixelRatio()));
                };
                const auto normal = sample();
                const auto point = canvas->mapFromScene(action->mapTo(reader, action->rect().center()));
                const QSignalBlocker holdAction(action.data());
                QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
                QTRY_VERIFY(action->isDown());
                QTest::qWait(50);
                const auto pressed = sample();
                QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
                QTRY_VERIFY(action && !action->isDown());
                QTRY_COMPARE(sample(), normal);
                QCOMPARE(pressed, ui::themeColors(dark).pressedPaper);
            }
        }
        const int currentPage = fixture.state().value("page").toInt();
        reader->pageButton(1);
        QCOMPARE(fixture.state().value("page").toInt(), currentPage);
        QVERIFY(fixture.click(actions, QPoint(5, 5)));
        QTRY_VERIFY(!fixture.widget("rrAnnotationActions"));
        QVERIFY(!fixture.visible("rrAnnotationExportConfirm"));
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportHtml"));
        actions = fixture.widget("rrAnnotationActions");
        auto *bookmarksTabWidget = fixture.widget("rrBookmarksTab");
        QVERIFY(fixture.click(actions, actions->mapFrom(reader, bookmarksTabWidget->mapTo(reader, bookmarksTabWidget->rect().center()))));
        QTRY_VERIFY(!fixture.widget("rrAnnotationActions"));
        QVERIFY(annotationsTab->isChecked());
        const int moreX = fixture.widget("rrAnnotationMore")->x();
        const int sortX = fixture.widget("rrMarksSort")->x();
        QVERIFY(fixture.press("rrBookmarksTab"));
        QTRY_VERIFY(bookmarksTab->isChecked());
        QVERIFY(fixture.widget("rrAnnotationMore")->isVisible());
        QCOMPARE(fixture.widget("rrAnnotationMore")->x(), moreX);
        QCOMPARE(fixture.widget("rrMarksSort")->x(), sortX);
        QVERIFY(fixture.press("rrAnnotationMore"));
        QTRY_VERIFY(fixture.visible("rrBookmarkRemoveCurrent"));
        QVERIFY(!fixture.visible("rrAnnotationExportHtml"));
        QVERIFY(fixture.click(fixture.widget("rrBookmarkActions"), QPoint(5, 5)));
        QTRY_VERIFY(!fixture.widget("rrBookmarkActions"));
        QVERIFY(fixture.press("rrAnnotationsTab"));
        QVERIFY(fixture.pressAnnotationExport(exportAction));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
        QVERIFY(!fixture.widget("rrAnnotationExportPicker"));
        QVERIFY(fixture.press("rrAnnotationExportCancel"));
        QTRY_VERIFY(!fixture.widget("rrAnnotationExportDialog"));
        QVERIFY(fixture.press("rrAnnotationsTab"));
        QTRY_VERIFY(annotationsTab->isChecked());
        if (pickerExport) {
            QVERIFY(fixture.pressAnnotationExport(exportAction));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
            QVERIFY(fixture.press("rrAnnotationExportConfirm"));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportPicker"));
            auto *picker = qobject_cast<QFileDialog *>(fixture.widget("rrAnnotationExportPicker"));
            QVERIFY(picker && picker->selectedFiles().value(0).endsWith(".zip"));
            QVERIFY(!picker->testOption(QFileDialog::DontConfirmOverwrite));
            QCOMPARE(picker->parentWidget(), static_cast<QWidget *>(&fixture.window));
            QVERIFY(!picker->graphicsProxyWidget());
            QPointer<QFileDialog> cancelledPicker = picker;
            picker->reject();
            QTRY_VERIFY(cancelledPicker.isNull());
            QVERIFY(!fixture.visible("rrAnnotationExportDone"));
        }
        QVERIFY(!QDir(fixture.paths.directory + "/exports/annotations").exists());
        QVERIFY(fixture.pressAnnotationExport(exportAction));
        QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
        QVERIFY(fixture.press("rrAnnotationExportConfirm"));
        QString exportedPath;
        QPointer<QFileDialog> exportPicker;
        if (pickerExport) {
            QTRY_VERIFY(fixture.visible("rrAnnotationExportPicker"));
            auto *picker = qobject_cast<QFileDialog *>(fixture.widget("rrAnnotationExportPicker"));
            QVERIFY(picker);
            exportPicker = picker;
            QCOMPARE(picker->parentWidget(), static_cast<QWidget *>(&fixture.window));
            QVERIFY(!picker->graphicsProxyWidget());
            exportedPath = fixture.directory.path() + "/chosen annotations.zip";
            auto *filename = picker->findChild<QLineEdit *>("fileNameEdit");
            QVERIFY(filename);
            filename->setFocus();
            filename->setText(exportedPath);
            QTRY_COMPARE(picker->selectedFiles().value(0), exportedPath);
            QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection));
        } else {
            const QDir exportFolder(fixture.paths.directory + "/exports/annotations");
            const auto exportedFiles = exportFolder.entryList({"*.zip"}, QDir::Files);
            QCOMPARE(exportedFiles.size(), 1);
            exportedPath = exportFolder.filePath(exportedFiles.first());
        }
        QTRY_VERIFY(fixture.visible("rrAnnotationExportDone"));
        QTRY_VERIFY(!fixture.widget("rrAnnotationExportDialog"));
        QTRY_VERIFY(exportPicker.isNull());
        QFile exportedFile(exportedPath);
        QVERIFY(exportedFile.open(QIODevice::ReadOnly));
        const auto originalZip = exportedFile.readAll();
        QVERIFY(originalZip.startsWith("PK"));
        exportedFile.close();
        ZipResource exportedHtml;
        QVERIFY(exportedHtml.open(exportedPath, markdownExport ? "annotations.md" : "annotations.html"));
        QByteArray exportedMarkup;
        while (!exportedHtml.finished() && exportedHtml.valid()) exportedMarkup += exportedHtml.next();
        QVERIFY(exportedHtml.valid() && exportedHtml.finished());
        QCOMPARE(originalZip.mid(originalZip.size() - 14, 4), QByteArray::fromHex("01000100"));
        if (markdownExport) {
            QVERIFY(exportedMarkup.startsWith("# ") && exportedMarkup.contains("### Annotation 001"));
        } else {
            QVERIFY(exportedMarkup.contains("data:font/ttf;base64,"));
            QVERIFY(exportedMarkup.contains(">#1</a>"));
        }
        QVERIFY(fixture.press("rrAnnotationExportDone"));
        if (!pickerExport) {
            QVERIFY(fixture.pressAnnotationExport(exportAction));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportReplace"));
            QVERIFY(!fixture.visible("rrAnnotationExportConfirm"));
            QVERIFY(fixture.press("rrAnnotationExportCancel"));
            QTRY_VERIFY(!fixture.widget("rrAnnotationExportReplaceDialog"));
            QFile preserved(exportedPath);
            QVERIFY(preserved.open(QIODevice::ReadOnly));
            QCOMPARE(preserved.readAll(), originalZip);
            preserved.close();
            QFile replaceTarget(exportedPath);
            QVERIFY(replaceTarget.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(replaceTarget.write("replace this existing export"), qint64(28));
            replaceTarget.close();
            QVERIFY(fixture.pressAnnotationExport(exportAction));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportReplace"));
            QVERIFY(fixture.press("rrAnnotationExportReplace"));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportDone"));
            QTRY_VERIFY(!fixture.widget("rrAnnotationExportReplaceDialog"));
            QFile replaced(exportedPath);
            QVERIFY(replaced.open(QIODevice::ReadOnly));
            const auto replacementZip = replaced.readAll();
            QVERIFY(replacementZip.startsWith("PK"));
            QVERIFY(replacementZip != QByteArray("replace this existing export"));
            replaced.close();
            QVERIFY(fixture.press("rrAnnotationExportDone"));
            // A file created after the new-export prompt must also need approval.
            QVERIFY(QFile::remove(exportedPath));
            QVERIFY(fixture.pressAnnotationExport(exportAction));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportConfirm"));
            QFile appeared(exportedPath);
            QVERIFY(appeared.open(QIODevice::WriteOnly));
            QCOMPARE(appeared.write("keep this existing file"), qint64(23));
            appeared.close();
            QVERIFY(fixture.press("rrAnnotationExportConfirm"));
            QTRY_VERIFY(fixture.visible("rrAnnotationExportReplace"));
            QTRY_VERIFY(!fixture.widget("rrAnnotationExportDialog"));
            QVERIFY(fixture.press("rrAnnotationExportCancel"));
            QTRY_VERIFY(!fixture.widget("rrAnnotationExportReplaceDialog"));
            QVERIFY(appeared.open(QIODevice::ReadOnly));
            QCOMPARE(appeared.readAll(), QByteArray("keep this existing file"));
        }
        QVERIFY(fixture.press("rrMarksSort"));
        for (const auto *name : {"rrBookmarkList", "rrMarksSortPopup", "rrModalCard", "rrMarksSort_newest"}) {
            auto *probe = fixture.widget(name);
            QVERIFY(probe);
            if (QString::fromLatin1(name) == "rrMarksSortPopup")
                continue;
            QCOMPARE(probe->palette().color(QPalette::Window), QColor(dark ? Qt::black : Qt::white));
            QCOMPARE(probe->palette().color(QPalette::WindowText), QColor(dark ? Qt::white : Qt::black));
        }
        QVERIFY(fixture.press("rrMarksSort_newest"));
        QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("marks_sort").toString(), QString("newest"));
        QTRY_VERIFY(!fixture.widget("rrMarksSortPopup"));
        QVERIFY(fixture.press("rrBookmarksTab"));
        QTRY_VERIFY(fixture.visible("rrBookmark0"));
        QVERIFY(fixture.press("rrBookmarkListClose"));
        QVERIFY(fixture.press("rrBookmarks"));
        QVERIFY(annotationsTab->isChecked());
        QVERIFY(fixture.press("rrMarksSort"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrMarksSort_newest"))->isChecked());
        QVERIFY(fixture.press("rrMarksSort_newest"));
        QTRY_VERIFY(fixture.visible("rrAnnotationRemove0"));
        QVERIFY(fixture.press("rrAnnotationRemove0"));
        QTRY_VERIFY(fixture.visible("rrKeepAnnotation"));
        const auto savedPassage = fixture.stored("highlights.json", "highlights").first().toObject().value("text").toString();
        auto *removeDetail = qobject_cast<QLabel *>(fixture.widget("rrRemoveAnnotationDetail"));
        QVERIFY(removeDetail);
        QVERIFY(removeDetail->text().contains(savedPassage.left(40)));
        QVERIFY(!removeDetail->text().contains("will be removed"));
        QVERIFY(fixture.press("rrKeepAnnotation"));
        QTRY_VERIFY(!fixture.visible("rrRemoveAnnotation"));
        QCOMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QVERIFY(fixture.press("rrAnnotationRemove0"));
        QTRY_VERIFY(fixture.visible("rrConfirmRemoveAnnotation"));
        QVERIFY(fixture.press("rrConfirmRemoveAnnotation"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 0);
        QVERIFY(fixture.visible("rrBookmarkListEmpty"));
        QVERIFY(fixture.press("rrBookmarkListClose"));
        QVERIFY(fixture.press("rrBookmarks"));
        QVERIFY(bookmarksTab->isChecked());
        QTRY_VERIFY(fixture.visible("rrBookmark0"));
        QVERIFY(fixture.bookmarkAction("rrBookmarkDelete"));
        QTRY_VERIFY(fixture.visible("rrConfirmRemoveBookmark"));
        QVERIFY(fixture.press("rrConfirmRemoveBookmark"));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QVERIFY(fixture.press("rrBookmarkListClose"));
        QVERIFY(fixture.press("rrBookmarks"));
        QVERIFY(bookmarksTab->isChecked());
        QVERIFY(fixture.visible("rrBookmarkListEmpty"));
        const auto other = fixture.directory.path() + "/other-marks.epub";
        QVERIFY(createBook(fixture.directory.path(), "other-marks.epub"));
        int expectedOpens = opened.count();
        for (const auto &path : {other, book}) {
            ++expectedOpens;
            fixture.window.openBook(path);
            QTRY_COMPARE(opened.count(), expectedOpens);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrBookmarks"));
            QVERIFY(fixture.press("rrBookmarks"));
            QVERIFY(fixture.press("rrMarksSort"));
            const auto expected = path == other ? "rrMarksSort_newest" : "rrMarksSort_oldest";
            QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget(expected))->isChecked());
            QVERIFY(fixture.press("rrMarksSort_oldest"));
            QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("marks_sort").toString(), QString("oldest"));
        }
    }
    void links_data() { bookmarks_data(); }
    void links() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QVERIFY(writeBook(fixture.directory.path(), "links.epub", test::linkBook()));
        const auto path = fixture.directory.path() + "/links.epub";
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = fixture.widget("rrPage");
        const auto origin = fixture.state().value("position");
        const auto note = fixture.linkPoint(path, "short note");
        QVERIFY(note.x() >= 0);
        QVERIFY(fixture.selectWord(page, note));
        QTRY_VERIFY(fixture.visible("rrSelectionLink"));
        QVERIFY(!fixture.visible("rrDictionary"));
        auto *lookup = fixture.widget("rrSelectionLookup"), *follow = fixture.widget("rrSelectionLink"),
             *clear = fixture.widget("rrAnnotationCancel");
        QCOMPARE(lookup->geometry().top(), follow->geometry().top());
        QVERIFY(!fixture.visible("rrSelectionAnnotate"));
        QVERIFY(lookup->geometry().left() < follow->geometry().left());
        QCOMPARE(lookup->geometry().top(), clear->geometry().top());
        QVERIFY(follow->geometry().left() < clear->geometry().left());
        QVERIFY(fixture.click(page, QPoint(10, page->height() / 2)));
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        auto *reading = static_cast<ReadingPage *>(page);
        QVERIFY(reading->annotationHandleRect(true).isEmpty() && reading->annotationHandleRect(false).isEmpty());
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.selectWord(page, note));
        QTRY_VERIFY(fixture.visible("rrSelectionLookup"));
        QVERIFY(fixture.press("rrSelectionLookup"));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        auto *headword = qobject_cast<QLabel *>(fixture.widget("rrDictionaryWord"));
        QVERIFY(headword);
        QVERIFY(headword->text() == "short" || headword->text() == "note");
        QTRY_VERIFY(!reading->annotationHandleRect(true).isEmpty() && !reading->annotationHandleRect(false).isEmpty());
        QVERIFY(fixture.press("rrDictionaryClose"));
        QTRY_VERIFY(!reading->annotationContains(note - QPoint(0, reading->topHeight())));
        QVERIFY(!fixture.visible("rrAnnotationPopover"));
        // Annotation tools require enabling the mode in the reading menu.
        QVERIFY(fixture.startAnnotation(page, note));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(!fixture.visible("rrSelectionLink"));
        QVERIFY(fixture.press("rrAnnotationCancel"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.selectWord(page, note));
        QTRY_VERIFY(fixture.visible("rrSelectionLink"));
        QVERIFY(fixture.stored("highlights.json", "highlights").isEmpty());
        auto *reader = fixture.window.readerWindow();
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        QVERIFY(reader && canvas);
        const QRegion damage(page->rect());
        android::takeRefreshFrame(reader, canvas->viewport(), damage);
        QVERIFY(fixture.press("rrSelectionLink"));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
        QVERIFY(!fixture.widget("rrLinkActions"));
        QVERIFY(!android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
        QVERIFY(fixture.press("rrLinkClose"));
        QTRY_VERIFY(!fixture.visible("rrLinkPanel"));
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
        QVERIFY(!android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
        const auto passage = fixture.linkPoint(path, "later passage");
        QVERIFY(passage.x() >= 0);
        QVERIFY(fixture.hold(page, passage));
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("position") != origin);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrScrubberReturn"));
        QVERIFY(fixture.press("rrScrubberReturn"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("position"), origin);
        if (fixture.visible("rrControlsTop"))
            QVERIFY(fixture.press("rrPage"));
        const auto website = fixture.linkPoint(path, "website");
        QVERIFY(website.x() >= 0);
        QVERIFY(fixture.hold(page, website));
        QTRY_VERIFY(fixture.visible("rrBrowserConfirmation"));
        QVERIFY(!fixture.widget("rrLinkActions"));
        QCOMPARE(fixture.state().value("position"), origin);
        // Cancel through the actual reader. External browser dispatch is
        // checked separately by the component's controlled browser service.
        QVERIFY(fixture.press("rrLinkNo"));
        QTRY_VERIFY(!fixture.visible("rrBrowserConfirmation"));
        QCOMPARE(fixture.state().value("position"), origin);
    }
    void footnoteHighlightShapes() {
        ReadingPage page(nullptr, [] {});
        page.resize(300, 400);
        auto palette = page.palette();
        palette.setColor(QPalette::Window, Qt::white);
        palette.setColor(QPalette::WindowText, Qt::black);
        page.setPalette(palette);
        const QVector<QRect> notes{QRect(100, 100, 8, 32), QRect(120, 100, 8, 32)};
        page.revealLinks(notes, notes);
        const auto image = page.snapshot();
        const int top = page.topHeight();
        const auto gray = [&](int x, int y) { return qGray(image.pixel(x, y + top)); };
        QVERIFY(gray(94, 94) > 240); // Old rectangle corner is outside the circle.
        QVERIFY(gray(90, 116) < 255); // Lateral extension is highlighted.
        QVERIFY(gray(110, 116) > 0 && gray(110, 116) < 255);
        QCOMPARE(gray(110, 116), gray(118, 116)); // No double tint in the overlap.
        QCOMPARE(gray(102, 116), gray(110, 116)); // No leftover arc from the neighboring circle.
        QVERIFY(qMin(gray(114, 116), gray(115, 116)) < gray(110, 116)); // Shared clipped edge.
        for (int y = 90; y < 145; ++y)
            for (int x = 80; x < 150; ++x) {
                const auto pixel = image.pixel(x, y + top);
                QCOMPARE(qRed(pixel), qGreen(pixel));
                QCOMPARE(qGreen(pixel), qBlue(pixel));
            }
        page.revealLinks({});
        QCOMPARE(qGray(page.snapshot().pixel(90, 116 + top)), 255);
    }
    void revealedLinkPress_data() { bookmarks_data(); }
    void revealedLinkPress() {
        QFETCH(bool, dark);
        ReadingPage page(nullptr, [] {});
        page.resize(500, 600);
        page.setPalette(ui::readerPalette(page.palette(), dark));
        page.setProperty("reader_dark", dark);
        const QRect note(100, 100, 8, 32), ordinary(100, 200, 150, 32);
        page.revealLinks({note, ordinary}, {note});
        int holds = 0, taps = 0, swipes = 0, annotations = 0;
        page.held = [&](QPoint) { ++holds; };
        page.tapped = [&](QPoint) { ++taps; };
        page.swiped = [&](int) { ++swipes; };
        page.annotationRequested = [&] { ++annotations; };
        const int top = page.topHeight();
        for (const auto &rect : {note, ordinary}) {
            const QPoint at = rect.center() + QPoint(0, top);
            const auto before = page.snapshot();
            QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, at);
            const auto pressed = page.snapshot();
            const int idleGray = qGray(before.pixel(at)), pressedGray = qGray(pressed.pixel(at));
            QVERIFY(dark ? pressedGray > idleGray : pressedGray < idleGray);
            const QPoint other = (rect == note ? ordinary : note).center() + QPoint(0, top);
            QCOMPARE(pressed.pixel(other), before.pixel(other));
            QTest::qWait(ui::metrics::lookup::holdMilliseconds + 50);
            QCOMPARE(holds, 0);
            QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, at);
            QCOMPARE(page.snapshot(), before);
        }
        QCOMPARE(taps, 2);
        const QPoint at = note.center() + QPoint(0, top);
        const auto idle = page.snapshot();
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseMove(&page, at + QPoint(150, 0));
        QCOMPARE(page.snapshot(), idle);
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, at + QPoint(150, 0));
        QCOMPARE(taps, 2);
        QCOMPARE(swipes, 0);
        QTest::mouseClick(&page, Qt::RightButton, Qt::NoModifier, at);
        QCOMPARE(annotations, 0);
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, at);
        page.cancelHold();
        QCOMPARE(page.snapshot(), idle);
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(taps, 2);
        page.revealLinks({});
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_COMPARE(holds, 1);
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, at);
    }
    void footnoteHitTargets() {
        const QVector<PageLink> links{
            {"first", {QRect(100, 100, 8, 32)}, LinkKind::Footnote},
            {"second", {QRect(120, 100, 8, 32)}, LinkKind::Footnote},
            {"passage", {QRect(99, 91, 10, 8)}, LinkKind::Passage}};
        const auto hit = [&](QPoint point, const QString &mode = "footnotes") {
            return linkAtPoint(links, point, mode, 6).index;
        };
        QCOMPARE(footnoteTarget(links[0].rectangles[0], 6), QRectF(82, 94, 44, 44));
        QCOMPARE(hit(QPoint(82, 116)), 0);
        QCOMPARE(hit(QPoint(81, 116)), -1);
        QCOMPARE(hit(QPoint(83, 95)), -1); // Outside the circle, inside its bounding square.
        QCOMPARE(hit(QPoint(104, 116)), 0); // Direct taps win inside both circles.
        QCOMPARE(hit(QPoint(124, 116)), 1);
        QCOMPARE(hit(QPoint(113, 116)), 0);
        QCOMPARE(hit(QPoint(115, 116)), 1);
        QCOMPARE(hit(QPoint(143, 116)), 1);
        QCOMPARE(hit(QPoint(104, 96)), -1); // Disabled passage text blocks a nearby note.
        QCOMPARE(hit(QPoint(104, 96), "all"), 2);
        QCOMPARE(hit(QPoint(82, 116), "off"), -1);
        QCOMPARE(hit(QPoint(104, 116), "off"), -1);
        // The wider second reference is closer by its edge, despite its farther center.
        const QVector<PageLink> unequal{
            {"narrow", {QRect(100, 100, 8, 60)}, LinkKind::Footnote},
            {"wide", {QRect(120, 100, 30, 60)}, LinkKind::Footnote}};
        QCOMPARE(linkAtPoint(unequal, QPoint(116, 130), "footnotes", 6).index, 1);
        const QVector<PageLink> wide{{"wide note", {QRect(100, 100, 120, 32)}, LinkKind::Footnote}};
        QVERIFY(!narrowFootnote(wide[0].rectangles[0]));
        QCOMPARE(linkAtPoint(wide, QPoint(160, 98), "footnotes", 6).index, -1);
        QCOMPARE(linkAtPoint(wide, QPoint(160, 110), "footnotes", 6).index, 0);
    }
    void tappableLinks_data() { bookmarks_data(); }
    void tappableLinks() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QVERIFY(writeBook(fixture.directory.path(), "direct.epub", test::linkBook()));
        const auto path = fixture.directory.path() + "/direct.epub";
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = fixture.widget("rrPage");
        const auto origin = fixture.state().value("position");
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("tappable_links"));
        if (fixture.visible("rrControlsTop"))
            QVERIFY(fixture.press("rrPage"));
        const auto defaultNote = fixture.linkPoint(path, "short note");
        QVERIFY(defaultNote.x() >= 0);
        QVERIFY(fixture.click(page, defaultNote));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.press("rrLinkClose"));
        QTRY_VERIFY(!fixture.visible("rrLinkPanel"));
        for (const auto *mode : {"off", "footnotes", "all"}) {
            if (!fixture.visible("rrControlsTop"))
                QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrSettings"));
            QVERIFY(fixture.press("rrSettings"));
            QVERIFY(fixture.press("rrSettingsControls"));
            auto *linkRow = fixture.widget("rrTappableLinks");
            QVERIFY(linkRow);
            for (const auto *name : {"rrPageNavigation", "rrTapZones"}) {
                auto *neighbor = fixture.widget(name);
                QVERIFY(neighbor);
                QCOMPARE(linkRow->height(), neighbor->height());
                QCOMPARE(linkRow->font(), neighbor->font());
                QCOMPARE(linkRow->styleSheet(), neighbor->styleSheet());
            }
            QVERIFY(fixture.press("rrTappableLinks"));
            QTRY_VERIFY(fixture.visible("rrTappableLinks_off"));
            QVERIFY(fixture.widget("rrTappableLinks_footnotes")->y() < fixture.widget("rrTappableLinks_all")->y());
            QVERIFY(fixture.widget("rrTappableLinks_all")->y() < fixture.widget("rrTappableLinks_off")->y());
            if (QString(mode) == "off") {
                QVERIFY(qobject_cast<QCheckBox *>(fixture.widget("rrTappableLinks_footnotes"))->isChecked());
            }
            QVERIFY(fixture.press(qPrintable(QString("rrTappableLinks_") + mode)));
            QTRY_VERIFY(!fixture.visible("rrTappableLinksPopup"));
            // Disable page-turn taps so non-link taps can be tested without leaving this page.
            QVERIFY(fixture.press("rrPageNavigation"));
            QTRY_VERIFY(fixture.visible("rrNavigation_swipes_only"));
            QVERIFY(fixture.press("rrNavigation_swipes_only"));
            QVERIFY(fixture.press("rrSettingsClose"));
            QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
            QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("tappable_links").toString(), QString(mode));
            if (fixture.visible("rrControlsTop"))
                QVERIFY(fixture.press("rrPage"));
            for (const auto *label : {"short note", "website"}) {
                const auto point = fixture.linkPoint(path, label);
                QVERIFY(point.x() >= 0);
                QVERIFY(fixture.click(page, point));
                const bool follows = QString(mode) == "all" || (QString(mode) == "footnotes" && QString(label) == "short note");
                if (follows) {
                    const bool note = QString(label) == "short note";
                    QTRY_VERIFY(fixture.visible(note ? "rrLinkPanel" : "rrBrowserConfirmation"));
                    QVERIFY(!fixture.visible("rrControlsTop"));
                    QVERIFY(fixture.press(note ? "rrLinkClose" : "rrLinkNo"));
                } else {
                    QVERIFY(!fixture.visible("rrLinkPanel"));
                    QVERIFY(!fixture.visible("rrBrowserConfirmation"));
                    if (fixture.visible("rrControlsTop"))
                        QVERIFY(fixture.press("rrPage"));
                }
                QCOMPARE(fixture.state().value("position"), origin);
            }
            const auto outside = fixture.coordinateQuery(path, QPoint(-1, -1), [top = static_cast<ReadingPage *>(page)->topHeight()](ReaderModel &coordinates) {
                for (const auto &link : coordinates.pageLinks())
                    if (coordinates.linkDetails(link.source).label == "short note") {
                        const auto r = link.rectangles.first();
                        return QPoint(r.center().x(), r.top() - 2 + top);
                    }
                return QPoint(-1, -1);
            });
            QVERIFY(outside.x() >= 0);
            QVERIFY(fixture.click(page, outside));
            QVERIFY(!fixture.visible("rrLinkPanel"));
            QCOMPARE(fixture.state().value("position"), origin);
            if (fixture.visible("rrControlsTop"))
                QVERIFY(fixture.press("rrPage"));
            // A long press keeps the connected selection menu in every mode.
            const auto note = fixture.linkPoint(path, "short note");
            QVERIFY(fixture.selectWord(page, note));
            QTRY_VERIFY(fixture.visible("rrSelectionLink"));
            QVERIFY(fixture.press("rrAnnotationCancel"));
            // Manual reveal still includes ordinary links with the persistent option off or footnotes-only.
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrScrubberLinks"));
            QVERIFY(fixture.press("rrScrubberLinks"));
            QVERIFY(fixture.click(page, fixture.linkPoint(path, "website")));
            QTRY_VERIFY(fixture.visible("rrBrowserConfirmation"));
            QVERIFY(fixture.press("rrLinkNo"));
            QCOMPARE(fixture.state().value("position"), origin);
        }
        // Direct links also win over normal tap zones, but annotation mode keeps its input.
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsControls"));
        QVERIFY(fixture.press("rrPageNavigation"));
        QTRY_VERIFY(fixture.visible("rrNavigation_taps_and_swipes"));
        QVERIFY(fixture.press("rrNavigation_taps_and_swipes"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        if (fixture.visible("rrControlsTop"))
            QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.click(page, fixture.linkPoint(path, "short note")));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.press("rrLinkClose"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QVERIFY(fixture.click(page, fixture.linkPoint(path, "short note")));
        QVERIFY(!fixture.visible("rrLinkPanel"));
        if (!fixture.visible("rrControlsTop"))
            QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        // A fresh reader restores the global choice.
        QVERIFY(writeBook(fixture.directory.path(), "second.epub", test::linkBook()));
        fixture.window.openBook(fixture.directory.path() + "/second.epub");
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.click(fixture.widget("rrPage"), fixture.linkPoint(fixture.directory.path() + "/second.epub", "short note")));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
    }
    void bookmarkCornerLinks_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("footnote");
        for (bool dark : {false, true})
            for (const auto *mode : {"off", "footnotes", "all"})
                for (bool footnote : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark ? "dark" : "light", mode,
                                                                     footnote ? "note" : "website")))
                        << dark << QString(mode) << footnote;
    }
    void bookmarkCornerLinks() {
        QFETCH(bool, dark);
        QFETCH(QString, mode);
        QFETCH(bool, footnote);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["tappable_links"] = mode;
        preferences["page_margins"] = QJsonObject{{"margin_pixels", 24}, {"top_margin_pixels", 0},
                                                 {"bottom_margin_pixels", 24}};
        preferences["last_text_layout"] = QJsonObject{{"font_pixels", 36}, {"text_alignment", "publisher"}};
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        auto files = test::linkBook();
        files["OEBPS/first.xhtml"] =
            QByteArray("<html xmlns='http://www.w3.org/1999/xhtml' xmlns:epub='http://www.idpf.org/2007/ops'>"
                       "<head><title>Corner link</title></head><body><p style='text-align:right!important;margin:0'>") +
            (footnote ? "<a epub:type='noteref' href='Notes/foot%20notes.xhtml#short'>Corner link</a>"
                      : "<a href='https://example.org/book'>Corner link</a>") + "</p></body></html>";
        QVERIFY(writeBook(fixture.directory.path(), "corner.epub", files));
        const auto path = fixture.directory.path() + "/corner.epub";
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
        const auto point = fixture.linkPoint(path, "Corner link");
        QVERIFY2(page->bookmarkCorner().contains(point), qPrintable(QString("Link at %1,%2 missed bookmark corner")
                                                                     .arg(point.x()).arg(point.y())));
        const auto origin = fixture.state().value("position");
        const bool follows = mode == "all" || (mode == "footnotes" && footnote);
        const auto *panel = footnote ? "rrLinkPanel" : "rrBrowserConfirmation";
        const auto *close = footnote ? "rrLinkClose" : "rrLinkNo";
        QVERIFY(fixture.click(page, point));
        if (follows) {
            QTRY_VERIFY(fixture.visible(panel));
            QCOMPARE(fixture.state().value("bookmark_count").toInt(), 0);
            QVERIFY(fixture.press(close));
            QTRY_VERIFY(!fixture.visible(panel));
        } else {
            QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
            QVERIFY(!fixture.visible(panel));
            QVERIFY(fixture.click(page, point));
            QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        }
        // The empty part of the corner still toggles a bookmark in every mode.
        const auto emptyCorner = page->bookmarkCorner().topRight() + QPoint(-ui::px(4), ui::px(4));
        QVERIFY(fixture.click(page, emptyCorner));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        if (follows) {
            // Link priority must also preserve an existing bookmark.
            QVERIFY(fixture.click(page, point));
            QTRY_VERIFY(fixture.visible(panel));
            QCOMPARE(fixture.state().value("bookmark_count").toInt(), 1);
            QVERIFY(fixture.press(close));
            QTRY_VERIFY(!fixture.visible(panel));
        }
        QVERIFY(fixture.click(page, emptyCorner));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 0);
        QCOMPARE(fixture.state().value("position"), origin);
    }
    void publisherFontDefault_data() { bookmarks_data(); }
    void publisherFontDefault() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences[appearance::inheritTextLayoutKey] = false;
        preferences["default_font_face"] = "NV Ancizar Serif";
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto originalFace = fixture.state().value("font_face");
        QCOMPARE(originalFace.toString(), QString("NV Ancizar Serif"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDefaults"));
        QVERIFY(revealSettingsOption(fixture, "rrDefaultFont"));
        QVERIFY(fixture.press("rrDefaultFont"));
        QTRY_VERIFY(fixture.visible("rrSettingsFontChoice0"));
        auto *publisher = qobject_cast<QAbstractButton *>(fixture.widget("rrSettingsFontChoice0"));
        QVERIFY(publisher);
        QCOMPARE(publisher->text(), strings::get("reader.publisher_fonts"));
        QVERIFY(publisher->property("reader_font_family").toString().isEmpty());
        QVERIFY(fixture.press("rrSettingsFontChoice0"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings") && !fixture.state().value("busy").toBool());
        auto stored = storage::readJson(fixture.paths.preferences());
        QCOMPARE(stored.value("schema").toInt(), 1);
        QCOMPARE(stored.value("default_font_face"), QJsonValue(""));
        QCOMPARE(fixture.state().value("font_face"), originalFace);
        QCOMPARE(fixture.state().value("default_font_face").toString(), QString(defaultFontFace));
        // Reopening Settings must retain Publisher rather than repair it to Libron.
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDefaults"));
        QVERIFY(revealSettingsOption(fixture, "rrDefaultFont"));
        auto *defaultButton = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrDefaultFont"));
        QVERIFY(defaultButton);
        QCOMPARE(defaultButton->caption(), strings::get("reader.publisher_fonts").toUpper());
        QVERIFY(fixture.press("rrDefaultFont"));
        QTRY_VERIFY(fixture.visible("rrSettingsFontChoice0"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrSettingsFontChoice0"))->isChecked());
        QVERIFY(fixture.press("rrSettingsFontChoice0"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto other = fixture.directory.path() + "/publisher-default.epub";
        QVERIFY(QFile::copy(book, other));
        fixture.window.openBook(other);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("font_face"), QJsonValue(""));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.press("rrFont"));
        QTRY_VERIFY(fixture.visible("rrFontChoice0"));
        auto *picker = dynamic_cast<ChoiceScreen *>(fixture.widget("rrChoices"));
        QVERIFY(picker);
        QVERIFY(picker->findChild<QLabel *>("rrFontChoice0Detail")->text().contains(strings::get("reader.default_font").toUpper()));
        auto *selectedFont = qobject_cast<QAbstractButton *>(fixture.widget("rrFontChoice0"));
        QVERIFY(selectedFont && selectedFont->isChecked());
        for (int repeat = 0; repeat < 2; ++repeat) {
            QVERIFY(fixture.press("rrFontChoice0"));
            QVERIFY(selectedFont->isChecked());
            QCOMPARE(fixture.state().value("font_face"), QJsonValue(""));
            QVERIFY(!fixture.state().value("preview_busy").toBool());
        }
        // The help slot is separate from the empty Publisher font value.
        QVERIFY(fixture.press("rrFontDefaultSlot"));
        QTRY_VERIFY(fixture.visible("rrFontDefaultDialog"));
        QVERIFY(!fixture.visible("rrFontDefaultCancel"));
        QVERIFY(fixture.press("rrFontDefaultConfirm"));
        QTRY_VERIFY(!fixture.widget("rrFontDefaultDialog"));
        QTest::keyClick(fixture.widget("rrFontChoice1"), Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(fixture.visible("rrFontDefaultCancel"));
        QVERIFY(fixture.press("rrFontDefaultConfirm"));
        QTRY_VERIFY(!fixture.widget("rrFontDefaultDialog"));
        QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("default_font_face").toString(), QString(defaultFontFace));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QTest::keyClick(fixture.widget("rrFontChoice0"), Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(fixture.visible("rrFontDefaultCancel"));
        QCOMPARE(fixture.widget("rrFontDefaultTitle")->property("text").toString(),
                 strings::get("reader.make_default_font").arg(strings::get("reader.publisher_fonts")));
        QVERIFY(fixture.press("rrFontDefaultCancel"));
        QTRY_VERIFY(!fixture.widget("rrFontDefaultDialog"));
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("default_font_face").toString(), QString(defaultFontFace));
        QTest::keyClick(fixture.widget("rrFontChoice0"), Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(fixture.visible("rrFontDefaultCancel"));
        QVERIFY(fixture.press("rrFontDefaultConfirm"));
        QTRY_VERIFY(!fixture.widget("rrFontDefaultDialog"));
        QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("default_font_face"), QJsonValue(""));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrFontClose"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrMenu") && !fixture.state().value("busy").toBool());
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 3);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("font_face"), originalFace);
    }
    void settings_data() { bookmarks_data(); }
    void dictionarySafeArea() {
        SafeArea safe;
        Fixture fixture(false, false, false, true, false, false, [&](ReaderHostServices &services) {
            services.safeArea = [&safe](QSize) { return safe; };
        });
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        const auto size = reader->size();
        safe = {480, 0, 0};
        reader->refreshHostPresentation();
        QTRY_VERIFY(reader->readyForPresentation());
        QCOMPARE(reader->size(), size);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QCOMPARE(page->y(), safe.top);
        const auto point = fixture.wordPoint();
        QVERIFY(point.y() < reader->height() / 2);
        QVERIFY(page->mapTo(reader, point).y() > reader->height() / 2);
        QVERIFY(fixture.selectWord(page, point));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QCOMPARE(fixture.widget("rrDictionary")->y(), safe.top);
    }
    void dictionaryAppearance_data() { bookmarks_data(); }
    void dictionaryAppearance() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto position = fixture.state().value("position");
        auto bodyFace = fixture.state().value("font_face").toString();
        if (bodyFace.isEmpty())
            bodyFace = fixture.state().value("text_font_face").toString();
        if (bodyFace.isEmpty())
            bodyFace = QString::fromLatin1(defaultFontFace);
        auto *definition = dynamic_cast<ui::PagedText *>(fixture.widget("rrDefinition"));
        QVERIFY(definition);
        QVERIFY(fixture.selectWord(page, fixture.wordPoint()));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        QCOMPARE(definition->textFont().family(), bodyFace);
        QVERIFY(fixture.press("rrDictionaryClose"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFontSize"));
        QVERIFY(fixture.press("rrDictionaryFontSize"));
        QTRY_VERIFY(fixture.visible("rrDictionaryFontSizePopup"));
        auto *defaultPreview = fixture.widget("rrDictionaryFontPreview100");
        QVERIFY(defaultPreview);
        auto *defaultSample = defaultPreview->findChild<QLabel *>();
        QVERIFY(defaultSample);
        QCOMPARE(defaultSample->font().family(), bodyFace);
        QVERIFY(fixture.press("rrDictionaryFontSize100"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFont"));
        auto *fontOption = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrDictionaryFont"));
        QVERIFY(fontOption);
        QCOMPARE(fontOption->typefacePreview(), bodyFace);
        QVERIFY(fixture.press("rrDictionaryFont"));
        QTRY_VERIFY(fixture.visible("rrSettingsFontPopup"));
        QAbstractButton *chosen = nullptr;
        for (int attempt = 0; attempt < 12 && !chosen; ++attempt) {
            for (auto *option : reader->findChildren<QAbstractButton *>())
                if (option->isVisible() && option->objectName().startsWith("rrSettingsFontChoice") &&
                    option->property("reader_font_family").toString() == "Libron")
                    chosen = option;
            if (!chosen)
                QVERIFY(fixture.press("rrSettingsFontNext"));
        }
        QVERIFY(chosen);
        QVERIFY(fixture.press(qPrintable(chosen->objectName())));
        QTRY_VERIFY(!fixture.visible("rrSettingsFontPopup"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFontSize"));
        QVERIFY(fixture.press("rrDictionaryFontSize"));
        QTRY_VERIFY(fixture.visible("rrDictionaryFontSizePopup"));
        auto *popup = fixture.widget("rrDictionaryFontSizePopup");
        for (int percent : appearance::dictionaryFontSizes) {
            auto *option = fixture.widget(qPrintable(QString("rrDictionaryFontSize%1").arg(percent)));
            QVERIFY(option && option->isVisible());
            QVERIFY(popup->rect().contains(QRect(option->mapTo(popup, QPoint()), option->size())));
        }
        QVERIFY(fixture.press("rrDictionaryFontSize140"));
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("dictionary_font_face"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("dictionary_font_face").toString(), QString("Libron"));
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("dictionary_font_size").toInt(), 140);
        QCOMPARE(fixture.state().value("position"), position);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(fixture.selectWord(page, fixture.wordPoint()));
        QTRY_VERIFY(fixture.visible("rrDictionary"));
        definition = dynamic_cast<ui::PagedText *>(fixture.widget("rrDefinition"));
        QVERIFY(definition);
        QCOMPARE(definition->textFont().family(), QString("Libron"));
        QCOMPARE(definition->textFont().pixelSize(), DictionaryPanel::definitionFont("Libron", 140).pixelSize());
        QVERIFY(fixture.press("rrDictionaryClose"));
        reader->close();
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        definition = dynamic_cast<ui::PagedText *>(fixture.widget("rrDefinition"));
        QVERIFY(definition);
        QCOMPARE(definition->textFont().family(), QString("Libron"));
        QCOMPARE(definition->textFont().pixelSize(), DictionaryPanel::definitionFont("Libron", 140).pixelSize());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDefaults"));
        QVERIFY(fixture.press("rrRestoreDefaults"));
        QTRY_VERIFY(fixture.visible("rrRestoreConfirmation"));
        QVERIFY(fixture.press("rrConfirmDefaults"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto defaults = storage::readJson(fixture.paths.preferences());
        QVERIFY(defaults.value("dictionary_font_face").toString().isEmpty());
        QCOMPARE(defaults.value("dictionary_font_size").toInt(), 100);
    }
    void dictionaryRedraw_data() { bookmarks_data(); }
    void dictionaryRedraw() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDictionaries"));
        QTRY_VERIFY(fixture.visible("rrDictionaryRow0"));
        auto *dictionaries = dynamic_cast<reader::DictionariesPage *>(fixture.widget("rrDictionaries"));
        QVERIFY(dictionaries);
        dictionaries->setStorage({}, 32000000000LL);
        QCoreApplication::processEvents();
        auto *row = fixture.widget("rrDictionaryRow0");
        QVERIFY(row);
        bool found = false;
        for (auto *label : row->findChildren<QLabel *>()) {
            if (!label->text().contains("English"))
                continue;
            found = true;
            QCOMPARE(label->palette().color(QPalette::WindowText), dark ? QColor(Qt::white) : QColor(Qt::black));
            auto *canvas = fixture.window.findChild<QGraphicsView *>();
            auto *stage = fixture.window.centralWidget();
            const auto at = [&](QPoint point) {
                return canvas->viewport()->mapTo(stage, canvas->mapFromScene(label->mapTo(fixture.window.readerWindow(), point)));
            };
            const QImage frame(fixture.window.saveScreenshot(fixture.directory.path()));
            QVERIFY(!frame.isNull());
            const qreal scale = frame.devicePixelRatio();
            const QRect bounds(at(label->rect().topLeft()) * scale, at(label->rect().bottomRight()) * scale);
            QVERIFY(frame.rect().contains(bounds));
            int ink = 0;
            const auto pixels = frame.convertToFormat(QImage::Format_ARGB32);
            for (int y = bounds.top() + 2; y < bounds.bottom() - 2; ++y)
                for (int x = bounds.left() + 2; x < bounds.right() - 2; ++x) {
                    const auto pixel = pixels.pixel(x, y);
                    const int gray = qGray(pixel);
                    ink += qAlpha(pixel) > 200 && (dark ? gray > 200 : gray < 80);
                }
            QVERIFY2(ink > 10, "Dictionary title disappeared from the actual canvas capture after rebuilding the list");
        }
        QVERIFY(found);
    }
    void lightGestureFeedback_data() {
        QTest::addColumn<int>("feedback");
        QTest::addColumn<bool>("warmth");
        QTest::addColumn<bool>("save");
        for (int feedback : {-1, 0, 1})
            for (bool warmth : {false, true})
                for (bool save : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(feedback).arg(warmth).arg(save)))
                        << feedback << warmth << save;
    }
    void lightGestureFeedback() {
        QFETCH(int, feedback);
        QFETCH(bool, warmth);
        QFETCH(bool, save);
        auto light = std::make_shared<GestureLightState>();
        Fixture fixture(false, false, false, true, false, false, [light](ReaderHostServices &services) {
            services.lightAvailable = [] { return true; };
            services.light = [light](QWidget *) { return std::make_unique<GestureLight>(light); };
        });
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        if (feedback >= 0) preferences["show_light_gesture_feedback"] = bool(feedback);
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        auto *notice = fixture.widget("rrBrightnessNotice");
        auto *reader = fixture.window.readerWindow();
        QVERIFY(page && notice && page->brightnessEdge && page->warmthEdge);
        reader->setProperty("reader_full_flash", false);
        LightFeedbackEvents events(reader, notice);
        QVERIFY(page->lightStarted(warmth));
        page->lightChanged(.1);
        page->lightChanged(.2);
        // Input queues a light command without changing the feedback widgets.
        QCOMPARE(warmth ? light->warmth : light->brightness, warmth ? 30 : 40);
        QVERIFY(!notice->isVisible());
        QCoreApplication::sendPostedEvents(reader);
        QCOMPARE(light->changes, 1);
        QVERIFY((warmth ? light->warmth : light->brightness) > (warmth ? 30 : 40));
        QVERIFY(!notice->isVisible());
        // A continuous swipe must not repaint the notice at each delivered level.
        for (int i = 0; i < 5; ++i) {
            QTest::qWait(40);
            page->lightChanged(i % 2 ? .1 : .2);
            QCoreApplication::sendPostedEvents(reader);
            QVERIFY(!notice->isVisible());
        }
        if (feedback != 0) QTRY_VERIFY(notice->isVisible());
        auto *value = notice->findChild<QLabel *>("rrLightSwipeValue");
        QVERIFY(value);
        const auto displayed = value->text();
        page->lightChanged(.1);
        QCoreApplication::sendPostedEvents(reader);
        QCOMPARE(warmth ? light->warmth : light->brightness, warmth ? 50 : 55);
        QCOMPARE(value->text(), displayed);
        // Release must not lose the last input or let a cancelled command run later.
        page->lightChanged(.3);
        page->lightChanged(.4);
        const int adjusted = warmth ? 90 : 100;
        if (warmth) QVERIFY(!light->automatic);
        reader->setProperty("reader_full_flash", false);
        page->lightFinished(save);
        QCOMPARE(warmth ? light->warmth : light->brightness, save ? adjusted : (warmth ? 30 : 40));
        QCOMPARE(warmth ? light->savedWarmth : light->savedBrightness, save ? adjusted : -1);
        if (warmth) QCOMPARE(light->automatic, !save);
        if (save && feedback != 0) QTRY_COMPARE(value->text(), QString::number(adjusted) + '%');
        if (!save) QCOMPARE(reader->property("reader_full_flash").toBool(), feedback != 0);
        QTRY_VERIFY(!notice->isVisible());
        // With no pop-up, neither release nor its delayed timer requests cleanup.
        if (feedback == 0) {
            QTest::qWait(1100);
            QVERIFY(!reader->property("reader_full_flash").toBool());
        }
        QCOMPARE(events.shows, feedback != 0 ? 1 : 0);
        QCOMPARE(events.hides, feedback != 0 ? 1 : 0);
        QCOMPARE(events.flashes, feedback != 0 ? 1 : 0);
        // Also finish before either the command or feedback has been delivered.
        const int original = warmth ? light->warmth : light->brightness;
        QVERIFY(page->lightStarted(warmth));
        page->lightChanged(-.2);
        page->lightFinished(save);
        const int final = save ? original - 30 : original;
        QCOMPARE(warmth ? light->warmth : light->brightness, final);
        if (warmth) QCOMPARE(light->automatic, !save);
        if (save && feedback != 0) {
            QTRY_VERIFY(notice->isVisible());
            QTRY_COMPARE(value->text(), QString::number(final) + '%');
            QTRY_VERIFY(!notice->isVisible());
        } else {
            QTest::qWait(200);
            QVERIFY(!notice->isVisible());
        }
        QCOMPARE(warmth ? light->warmth : light->brightness, final);
    }
    void lightGestureUnavailable_data() {
        QTest::addColumn<bool>("feedback");
        QTest::addColumn<QString>("failure");
        for (bool feedback : {false, true})
            for (const auto *failure : {"host", "disabled", "initialize", "warmth", "range", "brightness-write", "warmth-write"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(feedback).arg(failure))) << feedback << QString(failure);
    }
    void lightGestureUnavailable() {
        QFETCH(bool, feedback);
        QFETCH(QString, failure);
        auto light = std::make_shared<GestureLightState>();
        light->available = failure != "initialize";
        light->warmthSupported = failure != "warmth";
        light->maximumWarmth = failure == "range" ? 0 : 100;
        light->failChanges = failure.endsWith("-write");
        Fixture fixture(false, false, false, true, false, false, [light, failure](ReaderHostServices &services) {
            services.lightAvailable = [failure] { return failure != "host"; };
            services.light = [light](QWidget *) { return std::make_unique<GestureLight>(light); };
        });
        QVERIFY(fixture.prepared);
        QVERIFY(storage::writeJson(fixture.paths.preferences(), {{"show_light_gesture_feedback", feedback},
            {"edge_light_gestures", failure != "disabled"}}));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
        if (failure == "host" || failure == "disabled") {
            QVERIFY(!page->brightnessEdge && !page->warmthEdge);
        } else if (failure == "initialize" || failure == "warmth") {
            QVERIFY(!page->warmthEdge);
            QVERIFY(!page->lightStarted(true));
            if (failure == "initialize") QVERIFY(!page->lightStarted(false));
            else {
                QVERIFY(page->brightnessEdge);
                QVERIFY(page->lightStarted(false));
                page->lightChanged(.2);
                QCoreApplication::sendPostedEvents(fixture.window.readerWindow());
                QVERIFY(light->brightness > 40);
                if (feedback) QTRY_VERIFY(fixture.visible("rrBrightnessNotice"));
                page->lightFinished(false);
            }
        } else {
            QVERIFY(page->lightStarted(failure != "brightness-write"));
            page->lightChanged(.2);
            QCoreApplication::sendPostedEvents(fixture.window.readerWindow());
            QTest::qWait(200);
            QVERIFY(!fixture.visible("rrBrightnessNotice"));
            page->lightFinished(false);
        }
        QCOMPARE(light->brightness, 40);
        QCOMPARE(light->warmth, 30);
        QCOMPARE(light->savedBrightness, -1);
        QCOMPARE(light->savedWarmth, -1);
        QVERIFY(!fixture.visible("rrBrightnessNotice"));
    }
    void lightGestureFeedbackPersistence() {
        auto light = std::make_shared<GestureLightState>();
        Fixture fixture(false, false, false, true, false, false, [light](ReaderHostServices &services) {
            services.lightAvailable = [] { return true; };
            services.light = [light](QWidget *) { return std::make_unique<GestureLight>(light); };
        });
        QVERIFY(fixture.prepared);
        for (bool feedback : {false, true}) {
            QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
            fixture.window.openBook(book);
            QTRY_COMPARE(opened.count(), 1);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrSettings"));
            QVERIFY(fixture.press("rrSettingsControls"));
            QVERIFY(revealSettingsOption(fixture, "rrLightGestureFeedback"));
            auto *option = qobject_cast<QCheckBox *>(fixture.widget("rrLightGestureFeedback"));
            auto *edge = qobject_cast<QCheckBox *>(fixture.widget("rrEdgeLight"));
            QVERIFY(option && edge && edge->isChecked());
            QCOMPARE(option->isChecked(), !feedback);
            QVERIFY(fixture.press("rrLightGestureFeedback"));
            QCOMPARE(option->isChecked(), feedback);
            QVERIFY(fixture.press("rrSettingsClose"));
            QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
            QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("show_light_gesture_feedback"), QJsonValue(feedback));
            QVERIFY(storage::readJson(fixture.paths.preferences()).value("edge_light_gestures").toBool());
            fixture.window.readerWindow()->close();
            QTRY_VERIFY(!fixture.window.readerWindow());
        }
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page->lightStarted(false));
        page->lightChanged(.1);
        QTRY_VERIFY(fixture.visible("rrBrightnessNotice"));
        page->lightFinished(false);
    }
    void settings() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["native_hide_status_bar"] = false;
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(!fixture.visible("rrMenuClock"));
        QVERIFY(!fixture.visible("rrMenuBattery"));
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("native_hide_status_bar"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(!fixture.widget("rrSettingsUI"));
        QVERIFY(!fixture.widget("rrHideStatusBar"));
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrSessionTimeout"));
        auto *timeout = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrSessionTimeout"));
        QVERIFY(timeout);
        QCOMPARE(timeout->caption(), strings::get("settings.session_timeout_15").toUpper());
        for (const int minutes : {60, 240, 360, 720, 0, 15}) {
            QVERIFY(fixture.press("rrSessionTimeout"));
            QTRY_VERIFY(fixture.visible("rrSessionTimeoutPopup"));
            QVERIFY(fixture.press(qPrintable(QString("rrSessionTimeout_%1").arg(minutes))));
            QTRY_VERIFY(!fixture.visible("rrSessionTimeoutPopup"));
            QCOMPARE(storage::readJson(fixture.paths.preferences()).value("session_timeout_minutes").toInt(-1), minutes);
            QCOMPARE(timeout->caption(), strings::get(qPrintable(QString("settings.session_timeout_%1").arg(minutes))).toUpper());
        }
        QVERIFY(fixture.press("rrSettingsDisplay"));
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrTimeRemainingFormat"));
        auto *timeFormat = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrTimeRemainingFormat"));
        QVERIFY(timeFormat);
        QCOMPARE(timeFormat->height(), fixture.widget("rrHeaderFooterWeight")->height());
        QCOMPARE(timeFormat->font(), fixture.widget("rrHeaderFooterWeight")->font());
        QCOMPARE(timeFormat->caption(), strings::get("settings.time_remaining_detailed").toUpper());
        const auto timeCapture = qEnvironmentVariable("KOBALT_TIME_FORMAT_CAPTURE");
        if (!timeCapture.isEmpty())
            QVERIFY(fixture.window.readerWindow()->grab().save(timeCapture + (dark ? "-row-dark.png" : "-row-light.png")));
        QVERIFY(fixture.press("rrTimeRemainingFormat"));
        QTRY_VERIFY(fixture.visible("rrTimeRemainingFormatPopup"));
        if (!timeCapture.isEmpty())
            QVERIFY(fixture.window.readerWindow()->grab().save(timeCapture + (dark ? "-picker-dark.png" : "-picker-light.png")));
        QVERIFY(fixture.press("rrTimeRemainingFormat_approximate"));
        QTRY_VERIFY(!fixture.visible("rrTimeRemainingFormatPopup"));
        QCOMPARE(timeFormat->caption(), strings::get("settings.time_remaining_approximate").toUpper());
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("time_remaining_format"));
        struct Option { const char *tab, *name, *key; };
        QJsonObject expected{{"time_remaining_format", "approximate"}};
        for (const auto &option : {Option{"rrSettingsControls", "rrShowLinkIndicator", "show_link_indicator"},
                                   Option{"rrSettingsDefaults", "rrInheritTextLayout", "inherit_text_layout"}}) {
            QVERIFY(fixture.press(option.tab));
            QTRY_VERIFY(fixture.visible(option.name));
            auto *box = fixture.window.readerWindow()->findChild<QCheckBox *>(option.name);
            QVERIFY(box);
            const bool previous = box->isChecked();
            QVERIFY(fixture.press(option.name));
            QTRY_COMPARE(box->isChecked(), !previous);
            expected[option.key] = !previous;
        }
        // Settings are a draft until the panel closes.
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        for (auto it = expected.begin(); it != expected.end(); ++it)
            QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value(it.key()), it.value());
        const auto saved = storage::readJson(fixture.paths.preferences());
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrTimeRemainingFormat"));
        QCOMPARE(timeFormat->caption(), strings::get("settings.time_remaining_approximate").toUpper());
        QVERIFY(fixture.press("rrSettingsControls"));
        QTRY_VERIFY(fixture.visible("rrShowLinkIndicator"));
        QCOMPARE(fixture.window.readerWindow()->findChild<QCheckBox *>("rrShowLinkIndicator")->isChecked(),
                 saved.value("show_link_indicator").toBool());
        QVERIFY(fixture.press("rrSettingsDefaults"));
        QTRY_VERIFY(fixture.visible("rrRestoreDefaults"));
        QVERIFY(fixture.press("rrRestoreDefaults"));
        QTRY_VERIFY(fixture.visible("rrRestoreConfirmation"));
        QVERIFY(fixture.press("rrKeepDefaults"));
        QTRY_VERIFY(!fixture.visible("rrRestoreConfirmation"));
        QCOMPARE(storage::readJson(fixture.paths.preferences()), saved);
        QVERIFY(fixture.press("rrSettingsControls"));
        QTRY_VERIFY(fixture.visible("rrShowLinkIndicator"));
        auto *indicator = fixture.window.readerWindow()->findChild<QCheckBox *>("rrShowLinkIndicator");
        const bool previous = indicator->isChecked();
        {
            BlockedFile blocked(fixture.paths.preferences());
            QVERIFY(blocked.active());
            QVERIFY(fixture.press("rrShowLinkIndicator"));
            QCOMPARE(indicator->isChecked(), !previous);
            QVERIFY(fixture.press("rrSettingsClose"));
            QTRY_VERIFY(fixture.visible("rrSettingsWriteError"));
            QVERIFY(blocked.restore());
        }
        QCOMPARE(storage::readJson(fixture.paths.preferences()), saved);
        QVERIFY(fixture.press("rrSettingsWriteErrorConfirm"));
        QTRY_VERIFY(!fixture.visible("rrSettingsWriteError"));
        QTRY_VERIFY(fixture.visible("rrGeneralSettings"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_COMPARE(storage::readJson(fixture.paths.preferences()).value("show_link_indicator").toBool(), !previous);
    }
    void verticalJustification() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        verticalJustificationTour(fixture, "six-inch-light", false);
    }
    void advancedText_data() {
        QTest::addColumn<int>("dpi");
        QTest::addColumn<QString>("bookFont");
        QTest::addColumn<QString>("alignment");
        QTest::newRow("300dpi") << 300 << QString("NV Literata") << QString("left");
        QTest::newRow("native") << 0 << QString("NV Literata") << QString("justify");
        QTest::newRow("publisher-300dpi") << 300 << QString() << QString("publisher");
        QTest::newRow("publisher-native") << 0 << QString() << QString("right");
    }
    void advancedText() {
        QFETCH(int, dpi);
        QFETCH(QString, bookFont);
        QFETCH(QString, alignment);
        Fixture fixture;
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["last_text_layout"] = QJsonObject{{"font_face", bookFont}, {"text_alignment", alignment}};
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        auto *resolution = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        QVERIFY(resolution);
        resolution->setCurrentIndex(resolution->findData(dpi));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("font_face").toString(), bookFont);
        const auto sampleFace = bookFont.isEmpty() ? fixture.state().value("text_font_face").toString() : bookFont;
        QVERIFY(!sampleFace.isEmpty());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(fixture.press("rrSettings"));
        // Entering Styling must not request samples. Opening a sheet requests
        // only that feature, and reopening it reuses the prepared choices.
        {
            auto *panel = static_cast<reader::SettingsPanel *>(fixture.widget("rrGeneralSettings"));
            QVERIFY(panel);
            const auto prepare = panel->stylingRequested;
            QVector<reader::SettingsPanel::AdvancedText> requested;
            panel->stylingRequested = [&](reader::SettingsPanel::AdvancedText feature) {
                requested.append(feature);
                prepare(feature);
            };
            auto restorePrepare = qScopeGuard([&] { panel->stylingRequested = prepare; });
            struct Gate {
                QSemaphore release;
                std::atomic_bool entered{false};
            };
            auto gate = std::make_shared<Gate>();
            auto unblock = qScopeGuard([gate] { gate->release.release(); });
            qt::invoke(engineWorker(), [gate] {
                EngineJob activity;
                gate->entered.store(true);
                gate->release.acquire();
            }, Qt::QueuedConnection);
            QTRY_VERIFY(gate->entered.load());
            QVERIFY(fixture.press("rrSettingsStyling"));
            QVERIFY(requested.isEmpty());
            QVERIFY(!fixture.window.readerWindow()->busy());
            // Opening a page illustration must not wait for the engine.
            for (const auto *row : {"rrWidowsOrphans", "rrVerticalSpacing"}) {
                for (int page = 0; page < 4 && !fixture.widget(row)->visibleRegion().contains(fixture.widget(row)->rect().center()); ++page)
                    QVERIFY(fixture.press("rrSettingsOptionsNext"));
                QVERIFY(fixture.press(row));
                QCOMPARE(requested.size(), QString(row) == "rrWidowsOrphans" ? 1 : 2);
                QVERIFY(fixture.widget("rrStylingPreview")->property("sample_ready").toBool());
                const QPointer<QWidget> closedSheet = fixture.widget(qPrintable(QString(row) + "Popup"));
                QVERIFY(fixture.press(qPrintable(QString(row) + "Close")));
                QTRY_VERIFY(closedSheet.isNull());
                const int prepared = requested.size();
                QVERIFY(fixture.press(row));
                QCOMPARE(requested.size(), prepared);
                QVERIFY(fixture.widget("rrStylingPreview")->property("sample_ready").toBool());
                QVERIFY(fixture.press(qPrintable(QString(row) + "Close")));
            }
            for (int page = 0; page < 4 && fixture.visible("rrSettingsOptionsPrevious") && fixture.widget("rrSettingsOptionsPrevious")->isEnabled(); ++page)
                QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
            gate->release.release();
            QTRY_VERIFY_WITH_TIMEOUT(!fixture.window.readerWindow()->busy(), 10000);
        }
        QTRY_VERIFY(fixture.visible("rrSettingsClose"));
        QVERIFY(fixture.visible("rrSettingsTitle"));
        QCOMPARE(fixture.widget("rrSettingsTitle")->font().family(),
                 ui::brandFont(ui::metrics::chrome::titleText).family());
        QCOMPARE(fixture.widget("rrSettingsTitle")->font().pixelSize(), int(ui::metrics::chrome::titleText));
        for (const auto *name : {"rrReturn", "rrSettings", "rrSearchOpen", "rrBookmarks", "rrAnnotate"})
            QVERIFY2(!fixture.visible(name), name);
        auto *continuation = qobject_cast<QLabel *>(fixture.widget("rrSettingsOptionsContinuationHint"));
        QVERIFY(continuation);
        QTRY_VERIFY(continuation->isVisible());
        QVERIFY(continuation->font().italic());
        QVERIFY(fixture.press("rrSettingsOptionsNext"));
        QTRY_VERIFY(!continuation->isVisible());
        QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
        QTRY_VERIFY(continuation->isVisible());
        const auto reveal = [&fixture](const char *name) {
            for (int page = 0; page < 8; ++page) {
                auto *row = fixture.widget(name);
                if (row && row->visibleRegion().contains(row->rect().center()))
                    return true;
                if (!fixture.press("rrSettingsOptionsNext"))
                    return false;
                QCoreApplication::processEvents();
            }
            return false;
        };
        QVERIFY(reveal("rrWordSpacing"));
        auto *wordSpacing = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrWordSpacing"));
        QVERIFY(wordSpacing);
        QCOMPARE(wordSpacing->caption(), strings::get("settings.word_spacing_normal").toUpper());
        QVERIFY(fixture.press("rrWordSpacing"));
        QTRY_VERIFY(fixture.visible("rrWordSpacing_wide"));
        // The sheet shows CREngine pages of a bundled sample. They arrive from
        // the engine worker, and every option must look different.
        const auto preview = [&fixture] {
            auto *widget = fixture.widget("rrStylingPreview");
            return widget ? widget->grab().toImage() : QImage();
        };
        // Some options can lay out these lines the same way, for example two
        // spacings on justified text. At least two options must differ.
        const auto compareOptions = [&](const char *popup, const QStringList &names) {
            QVector<QImage> images;
            int descriptionHeight = -1;
            for (const auto &name : names) {
                QVERIFY(fixture.press(qPrintable(name)));
                QTRY_VERIFY(qobject_cast<QPushButton *>(fixture.widget(qPrintable(name)))->isChecked());
                // Choosing keeps the sheet open, so the options can be compared.
                QVERIFY(fixture.visible(popup));
                // A longer description must not move the controls below it.
                const int height = fixture.widget("rrAdvancedTextDescription")->height();
                QVERIFY2(descriptionHeight < 0 || height == descriptionHeight, qPrintable(name));
                descriptionHeight = height;
                QCoreApplication::processEvents();
                const auto image = preview();
                QVERIFY(!image.isNull());
                if (!images.contains(image))
                    images.append(image);
            }
            QVERIFY2(images.size() > 1, popup);
        };
        QTRY_VERIFY_WITH_TIMEOUT(fixture.widget("rrStylingPreview")->property("sample_ready").toBool(), 10000);
        compareOptions("rrWordSpacingPopup", {"rrWordSpacing_narrow", "rrWordSpacing_normal", "rrWordSpacing_wide"});
        auto *description = qobject_cast<QLabel *>(fixture.widget("rrAdvancedTextDescription"));
        QVERIFY(description);
        QVERIFY(description->text().startsWith(strings::get("settings.word_spacing_wide")));
        QVERIFY(fixture.press("rrWordSpacingClose"));
        QTRY_VERIFY(!fixture.visible("rrWordSpacingPopup"));
        QCOMPARE(wordSpacing->caption(), strings::get("settings.word_spacing_wide").toUpper());
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("word_spacing"));
        QVERIFY(reveal("rrJustificationTracking"));
        auto *tracking = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrJustificationTracking"));
        auto *widows = dynamic_cast<ui::CaptionButton *>(fixture.widget("rrWidowsOrphans"));
        QVERIFY(tracking && widows);
        QCOMPARE(tracking->accessibleDescription(), strings::get("settings.justification_tracking_summary"));
        QCOMPARE(widows->accessibleDescription(), strings::get("settings.widows_orphans_summary"));
        QVERIFY(tracking->height() >= tracking->heightForWidth(tracking->width()));
        const auto *advancedHeading = fixture.widget("rrAdvancedTextSection");
        const auto *breaksHeading = fixture.widget("rrVerticalSpacingSection");
        const auto *normalizationHeading = fixture.widget("rrNormalizationSection");
        const auto *contentHeading = fixture.widget("rrAdvancedContentSection");
        QVERIFY(advancedHeading && breaksHeading && normalizationHeading && contentHeading);
        QVERIFY(advancedHeading->y() < breaksHeading->y() && breaksHeading->y() < normalizationHeading->y() &&
                normalizationHeading->y() < contentHeading->y());
        // Fallback fonts are font defaults, in the Defaults tab with Default font.
        auto *defaultsPage = fixture.widget("rrDefaultsOptions");
        QVERIFY(defaultsPage);
        for (const auto *name : {"rrFontsFamiliesSection", "rrSerifFont", "rrSansFont", "rrMonoFont"})
            QVERIFY2(defaultsPage->isAncestorOf(fixture.widget(name)), name);
        QCOMPARE(tracking->caption(), strings::get("settings.justification_tracking_off").toUpper());
        QCOMPARE(widows->caption(), strings::get("settings.widows_orphans_publisher").toUpper());
        QVERIFY(fixture.press("rrJustificationTracking"));
        QTRY_VERIFY(fixture.visible("rrTracking_more"));
        auto *notice = qobject_cast<QLabel *>(fixture.widget("rrTrackingAlignmentNotice"));
        QVERIFY(notice && notice->isVisible());
        const auto suffix = alignment == "left" ? "left" : alignment == "justify" ? "justified"
                            : alignment == "publisher" ? "publisher" : "other";
        QCOMPARE(notice->text(), strings::get((QString("settings.justification_tracking_current_") + suffix).toUtf8().constData()));
        QVERIFY(notice->visibleRegion().contains(notice->rect()));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.widget("rrStylingPreview")->property("sample_ready").toBool(), 10000);
        compareOptions("rrTrackingPopup", {"rrTracking_off", "rrTracking_subtle", "rrTracking_more"});
        QVERIFY(fixture.press("rrTrackingClose"));
        QTRY_VERIFY(!fixture.visible("rrTrackingPopup"));
        QCOMPARE(tracking->caption(), strings::get("settings.justification_tracking_more").toUpper());
        QVERIFY(reveal("rrWidowsOrphans"));
        QVERIFY(fixture.press("rrWidowsOrphans"));
        QTRY_VERIFY(fixture.visible("rrWidowsOrphans_avoid"));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.widget("rrStylingPreview")->property("sample_ready").toBool(), 10000);
        auto *sheet = fixture.widget("rrWidowsOrphansPopup");
        auto *samplePages = fixture.widget("rrStylingPreview");
        QVERIFY(sheet && samplePages && samplePages->visibleRegion().contains(samplePages->rect()));
        // Keeping lines together changes the native page-break illustration.
        compareOptions("rrWidowsOrphansPopup", {"rrWidowsOrphans_allow", "rrWidowsOrphans_avoid"});
        QVERIFY(!fixture.visible("rrStylingSameNote"));
        QVERIFY(fixture.press("rrWidowsOrphansClose"));
        QTRY_VERIFY(!fixture.visible("rrWidowsOrphansPopup"));
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("justification_tracking"));
        QVERIFY(!storage::readJson(fixture.paths.preferences()).contains("justification_tracking"));
        auto *closeSettings = qobject_cast<QPushButton *>(fixture.widget("rrSettingsClose"));
        QVERIFY(closeSettings);
        QCOMPARE(closeSettings->accessibleName(), strings::get("settings.close"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(!fixture.window.idle());
        QVERIFY(fixture.visible("rrReturn"));
        QVERIFY(fixture.visible("rrSettings"));
        QVERIFY(!fixture.visible("rrSettingsClose"));
        // Exit-guard timing has its own lifecycle check. Here the settings
        // reflow can outlast that guard; close only after layout has settled.
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 10);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("word_spacing").toInt(), 120);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("justification_tracking").toInt(), 15);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("widows_orphans").toString(), "avoid");
        QTRY_COMPARE(fixture.state().value("word_spacing").toInt(), 120);
        QTRY_COMPARE(fixture.state().value("justification_tracking").toInt(), 15);
        QTRY_COMPARE(fixture.state().value("widows_orphans").toString(), "avoid");
        if (!fixture.visible("rrReturn"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrReturn"));
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        opened.clear();
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("word_spacing").toInt(), 120);
        QTRY_COMPARE(fixture.state().value("justification_tracking").toInt(), 15);
        QTRY_COMPARE(fixture.state().value("widows_orphans").toString(), "avoid");
    }
    void fullWidthPanels_data() { bookmarks_data(); }
    void fullWidthPanels() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.resize(2200, 1000);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(0);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavChapters"));
        QVERIFY(fixture.press("rrNavChapters"));
        QTRY_VERIFY(fixture.visible("rrChapter1"));
        QVERIFY(fixture.press("rrChapter1"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto checkBounds = [&](QWidget *panel) {
            const auto *reader = fixture.window.readerWindow();
            QCOMPARE(panel->width(), reader->width());
            QCOMPARE(panel->x(), 0);
        };
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavLayout"));
        checkBounds(fixture.widget("rrControlsTop"));
        checkBounds(fixture.widget("rrBottomNavigation"));
        QVERIFY(fixture.press("rrNavLayout"));
        QTRY_VERIFY(fixture.visible("rrMargins"));
        checkBounds(fixture.widget("rrMenu"));
        const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QCoreApplication::processEvents();
            QVERIFY(fixture.window.grab().save(output + (dark ? "/full-width-layout-dark.png" : "/full-width-layout-light.png")));
        }
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrNavLayout"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(fixture.press("rrSettings"));
        QTRY_VERIFY(fixture.visible("rrGeneralSettings"));
        checkBounds(fixture.widget("rrGeneralSettings"));
        QVERIFY(fixture.press("rrSettingsStyling"));
        for (const auto *row : {"rrParagraphSpacing", "rrWordSpacing", "rrJustificationTracking", "rrWidowsOrphans", "rrVerticalSpacing"}) {
            QVERIFY(revealSettingsOption(fixture, row));
            QVERIFY(fixture.press(row));
            const QString prefix = QString(row) == "rrJustificationTracking" ? "rrTracking" : row;
            auto *popup = fixture.widget(qPrintable(prefix + "Popup"));
            QVERIFY(popup);
            auto *overlay = dynamic_cast<ui::ModalOverlay *>(popup);
            QVERIFY(overlay);
            QCOMPARE(overlay->card()->width(), int(ui::px(1264)));
            QVERIFY(overlay->rect().contains(overlay->card()->geometry()));
            QTRY_VERIFY_WITH_TIMEOUT(fixture.widget("rrStylingPreview")->property("sample_ready").toBool(), 10000);
            if (QString(row) == "rrParagraphSpacing")
                QVERIFY(fixture.widget("rrStylingPreview")->geometry().bottom() <
                        fixture.widget("rrParagraphControls")->geometry().top());
            if (!output.isEmpty()) {
                QCoreApplication::processEvents();
                QVERIFY(fixture.window.readerWindow()->grab().save(output + "/wide-" + row + (dark ? "-dark.png" : "-light.png")));
            }
            QVERIFY(fixture.press(qPrintable(prefix + "Close")));
        }
        if (!output.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(fixture.window.grab().save(output + (dark ? "/full-width-settings-dark.png" : "/full-width-settings-light.png")));
        }
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        if (!fixture.visible("rrNavLayout"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavLayout"));
        auto *reader = fixture.window.readerWindow();
        QCOMPARE(fixture.widget("rrControlsTop")->width(), reader->width());
        QCOMPARE(fixture.widget("rrBottomNavigation")->width(), reader->width());
        reader->resize(720, 1000);
        QTRY_COMPARE(fixture.widget("rrControlsTop")->width(), 720);
        QTRY_COMPARE(fixture.widget("rrBottomNavigation")->width(), 720);
    }
    void globalLayout() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        auto globalBeautifier = storage::readJson(fixture.paths.preferences());
        globalBeautifier["beautifier_initials"] = true;
        globalBeautifier["beautifier_spacing"] = false;
        globalBeautifier["beautifier_indent"] = false;
        QVERIFY(storage::writeJson(fixture.paths.preferences(), globalBeautifier));
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QTRY_VERIFY(fixture.visible("rrLarger"));
        QVERIFY(fixture.press("rrLarger"));
        QTRY_VERIFY(fixture.state().value("preview_ready").toBool() && !fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrNavLayout"));
        QTRY_VERIFY(fixture.visible("rrWider"));
        QVERIFY(fixture.press("rrWider"));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrVerticalMarginsMore"));
        QTRY_VERIFY(!fixture.state().value("preview_busy").toBool());
        QVERIFY(fixture.press("rrOrientation2"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && !fixture.visible("rrMargins"));
        const auto first = fixture.state();
        const auto settings = storage::readJson(fixture.paths.preferences());
        QCOMPARE(settings.value("orientation").toInt(), 2);
        const auto margins = settings.value("page_margins").toObject();
        QCOMPARE(margins.value("margin_pixels").toInt(), marginDesign(first.value("margin_pixels").toInt()));
        QCOMPARE(margins.value("top_margin_pixels").toInt(), marginDesign(first.value("top_margin_pixels").toInt()));
        QVERIFY(!settings.value("last_text_layout").toObject().contains("margin_pixels"));
        const auto firstSaved = storage::readBookState(fixture.paths, first.value("book").toString()).object;
        QVERIFY(!firstSaved.contains("margin_pixels") && !firstSaved.contains("orientation"));
        const auto other = fixture.directory.path() + "/other-layout.epub";
        QVERIFY(QFile::copy(book, other));
        const auto identity = QUrl::fromLocalFile(other).toString();
        QVERIFY(storage::writeJson(fixture.paths.bookState(identity),
                                  {{"book", identity}, {"fingerprint", first.value("fingerprint")},
                                   {"font_pixels", 34}, {"margin_pixels", 3}, {"top_margin_pixels", 6},
                                   {"bottom_margin_pixels", 9}, {"orientation", 1}, {"book_beautifier", false}, {"beautifier_spacing", true}}));
        fixture.window.openBook(other);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const auto second = fixture.state();
        QVERIFY(!second.contains("book_beautifier"));
        QVERIFY(second.value("beautifier_initials").toBool());
        QVERIFY(!second.value("beautifier_spacing").toBool());
        QVERIFY(!second.value("beautifier_indent").toBool());

        QCOMPARE(second.value("font_pixels").toInt(), 34);
        QCOMPARE(second.value("margin_pixels"), first.value("margin_pixels"));
        QCOMPARE(second.value("top_margin_pixels"), first.value("top_margin_pixels"));
        QCOMPARE(second.value("bottom_margin_pixels"), first.value("bottom_margin_pixels"));
        QCOMPARE(second.value("orientation").toInt(), 2);
        fixture.window.close();
        QTRY_VERIFY(fixture.window.idle());
        desktop::Window relaunched(fixture.paths, fixture.fontInputs, placeholderFontFamily(*assets, *copies));
        relaunched.resize(920, 800);
        relaunched.show();
        QSignalSpy reopened(&relaunched, &desktop::Window::bookOpened);
        relaunched.openBook(book);
        QTRY_COMPARE(reopened.count(), 1);
        QTRY_VERIFY(relaunched.readerWindow());
        const auto state = [&] {
            relaunched.readerWindow()->checkpoint();
            return storage::readJson(fixture.paths.stateReport);
        };
        QTRY_VERIFY(!state().value("busy").toBool());
        QCOMPARE(state().value("font_pixels"), first.value("font_pixels"));
        QCOMPARE(state().value("margin_pixels"), first.value("margin_pixels"));
        QCOMPARE(state().value("orientation").toInt(), 2);
        relaunched.close();
        QTRY_VERIFY(relaunched.idle());
    }
    void columns_data() { bookmarks_data(); }
    void columns() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        const auto reveal = [&fixture](const char *name) {
            for (int page = 0; page < 8; ++page) {
                auto *row = fixture.widget(name);
                if (row && row->visibleRegion().contains(row->rect().center()))
                    return true;
                if (!fixture.press("rrSettingsOptionsNext"))
                    return false;
                QCoreApplication::processEvents();
            }
            return false;
        };
        const auto capture = [&fixture, dark](const QString &name) {
            const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
            if (output.isEmpty()) return true;
            QCoreApplication::processEvents();
            return QDir().mkpath(output) && fixture.window.grab().save(output + "/" + name + (dark ? "-dark.png" : "-light.png"));
        };
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavLayout"));
        QVERIFY(!fixture.widget("rrColumns1"));
        QVERIFY(!fixture.widget("rrColumnSpacing"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrNavLayout"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(reveal("rrLandscapeColumns"));
        QVERIFY(reveal("rrColumnSpacingSetting"));
        QVERIFY(!fixture.widget("rrColumnSpacingSetting")->isEnabled());
        QVERIFY(capture("columns-disabled"));
        QVERIFY(fixture.widget("rrDisplayColumnsSection"));
        QVERIFY(fixture.press("rrLandscapeColumns"));
        QVERIFY(fixture.widget("rrColumnSpacingSetting")->isEnabled());
        QVERIFY(reveal("rrColumnSpacingSetting"));
        QVERIFY(fixture.press("rrColumnSpacingSetting"));
        QTRY_VERIFY(fixture.visible("rrColumnSpacing200"));
        QVERIFY(qobject_cast<QAbstractButton *>(fixture.widget("rrColumnSpacing150"))->isChecked());
        QCOMPARE(qobject_cast<QAbstractButton *>(fixture.widget("rrColumnSpacing150"))->text(), strings::get("settings.column_spacing_default"));
        QVERIFY(capture("column-spacing-choices"));
        QVERIFY(fixture.press("rrColumnSpacing200"));
        QVERIFY(capture("landscape-column-settings"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 1);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("two_columns_landscape").toBool(), true);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("column_spacing_percent").toInt(), 200);
        auto *rotate = fixture.window.findChild<QAction *>("desktopRotateDevice");
        QVERIFY(rotate);
        const auto portrait = fixture.window.readerWindow()->size();
        rotate->trigger();
        QTRY_COMPARE(fixture.window.readerWindow()->size(), portrait.transposed());
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 2);
        QCOMPARE(fixture.state().value("column_spacing").toInt(), fixture.state().value("font_pixels").toInt() * 2);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.visible("rrNavLayout"));
        QVERIFY(capture("columns-landscape"));
        // A short landscape list can leave enough space for a hint on its
        // final page. That space must stay empty on both reader sizes.
        for (int screen : {1, 2}) {
            fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(screen);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            if (!fixture.visible("rrSettings"))
                QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrSettings"));
            auto *hint = fixture.widget("rrSettingsOptionsContinuationHint");
            auto *next = fixture.widget("rrSettingsOptionsNext");
            auto *previous = fixture.widget("rrSettingsOptionsPrevious");
            QVERIFY(hint && next && previous);
            for (int page = 0; page < 8 && previous->isEnabled(); ++page)
                QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
            QTRY_VERIFY(hint->isVisible());
            for (int page = 0; page < 8 && next->isEnabled(); ++page)
                QVERIFY(fixture.press("rrSettingsOptionsNext"));
            QVERIFY(!next->isEnabled());
            QTRY_VERIFY(!hint->isVisible());
            QVERIFY(capture(QString("display-last-landscape-%1").arg(screen)));
            QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
            QTRY_VERIFY(hint->isVisible());
            QVERIFY(fixture.press("rrSettingsOptionsNext"));
            QTRY_VERIFY(!hint->isVisible());
            QVERIFY(fixture.press("rrSettingsClose"));
        }
        rotate->trigger();
        QTRY_COMPARE(fixture.window.readerWindow()->size(), portrait);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 1);
        if (!fixture.visible("rrReturn"))
            QVERIFY(fixture.press("rrPage"));
        // Settings guards Exit for the release of its Close tap.
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 20);
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        QVERIFY(createBook(fixture.directory.path(), "other-columns.epub"));
        fixture.window.openBook(fixture.directory.path() + "/other-columns.epub");
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("columns").toInt(), 1);
        QCOMPARE(fixture.state().value("column_spacing_percent").toInt(), 200);
        rotate->trigger();
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 2);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(reveal("rrLandscapeColumns"));
        QVERIFY(fixture.press("rrLandscapeColumns"));
        QVERIFY(reveal("rrColumnSpacingSetting"));
        QVERIFY(!fixture.widget("rrColumnSpacingSetting")->isEnabled());
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("columns").toInt(), 1);
        if (!fixture.visible("rrReturn"))
            QVERIFY(fixture.press("rrPage"));
        // Settings guards Exit for the release of its Close tap.
        QTest::qWait(ui::metrics::contents::exitGuardMilliseconds + 20);
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 3);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("columns").toInt(), 1);
    }
    void opticalSize() {
        Fixture fixture(false, true);
        QVERIFY(fixture.prepared);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavText"));
        QVERIFY(fixture.press("rrNavText"));
        QTRY_VERIFY(fixture.visible("rrFont"));
        QVERIFY(fixture.press("rrFont"));
        QWidget *choice = nullptr;
        for (int page = 0; page < 20 && !choice; ++page) {
            QCoreApplication::processEvents();
            for (auto *button : fixture.window.readerWindow()->findChildren<QPushButton *>())
                if (button->isVisible() && button->objectName().startsWith("rrFontChoice") &&
                    button->accessibleName().contains("Amstelvar", Qt::CaseInsensitive))
                    choice = button;
            if (!choice) {
                QVERIFY(fixture.widget("rrFontPagerNext")->isEnabled());
                QVERIFY(fixture.press("rrFontPagerNext"));
            }
        }
        QVERIFY(choice);
        QVERIFY(fixture.click(choice));
        QVERIFY(fixture.press("rrFontClose"));
        QTRY_VERIFY(fixture.visible("rrOpticalSizeMore"));
        QTRY_VERIFY(fixture.state().value("preview_ready").toBool());
        QTRY_VERIFY(fixture.state().value("preview_settings").toObject().value("font_face").toString().contains("Amstelvar"));
        QVERIFY(fixture.press("rrOpticalSizeMore"));
        QTRY_COMPARE(fixture.state().value("preview_settings").toObject().value("font_optical_size").toInt(), 15);
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_VERIFY(fixture.state().value("font_face").toString().contains("Amstelvar", Qt::CaseInsensitive));
        const auto selected = fixture.state();
        QCOMPARE(selected.value("font_optical_size").toInt(), 15); // Font declares a 14 pt default.
        QCOMPARE(selected.value("font_pixels").toInt(), defaultFontPixels());
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrReturn"));
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(fixture.window.idle());
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("font_optical_size"), selected.value("font_optical_size"));
        QCOMPARE(fixture.state().value("font_face"), selected.value("font_face"));
    }
    void desktopControls() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(2);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *resolution = fixture.window.findChild<QComboBox *>("desktopResolutionPicker");
        auto *rotate = fixture.window.findChild<QAction *>("desktopRotateDevice");
        auto *toolbar = fixture.window.findChild<QAction *>("desktopShowToolbar");
        QVERIFY(resolution && rotate && toolbar);
        const auto original = fixture.state();
        for (int index : {0, 2, 1}) {
            resolution->setCurrentIndex(index);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QCOMPARE(fixture.state().value("position"), original.value("position"));
            QCOMPARE(fixture.state().value("pages"), original.value("pages"));
        }
        const auto portrait = fixture.window.readerWindow()->size();
        rotate->trigger();
        QTRY_COMPARE(fixture.window.readerWindow()->size(), portrait.transposed());
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(opened.count(), 1); // Reflow reuses the open publication.
        rotate->trigger();
        QTRY_COMPARE(fixture.window.readerWindow()->size(), portrait);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("position"), original.value("position"));
        toolbar->trigger();
        QTRY_VERIFY(!fixture.window.findChild<QWidget *>("desktopPreviewToolbar")->isVisible());
        toolbar->trigger();
        QTRY_VERIFY(fixture.window.findChild<QWidget *>("desktopPreviewToolbar")->isVisible());
        auto *screen = fixture.window.findChild<QComboBox *>("desktopScreenPicker");
        for (int preset : {3, 1}) {
            screen->setCurrentIndex(preset);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            auto *phoneCard = fixture.widget("rrHeaderFooterPreset_phone");
            QVERIFY(phoneCard);
            QCOMPARE(!phoneCard->isHidden(), preset == 3);
            QCOMPARE(fixture.state().value("header_footer_preset").toString(),
                     preset == 3 ? QString("phone") : QString("default"));
            QCOMPARE(opened.count(), 1);
        }
        screen->setCurrentIndex(2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        const int page = fixture.state().value("page").toInt();
        auto *next = fixture.window.findChild<QPushButton *>("desktopDeviceNext");
        QVERIFY(next && next->isVisible());
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("page").toInt() > page);
        auto *previous = fixture.window.findChild<QPushButton *>("desktopDevicePrevious");
        QVERIFY(previous);
        QTest::qWait(210);
        QTest::mouseClick(previous, Qt::LeftButton);
        QTRY_VERIFY(!fixture.state().value("busy").toBool() && fixture.state().value("page").toInt() == page);
    }
    void sameProcessReadersDoNotRecoverEachOther() {
        QTemporaryDir directory;
        storage::Paths paths{directory.path()};
        const auto services = [] {
            auto result = desktop::readerServices();
            result.suspended = [] { return false; };
            return result;
        };
        android::ReaderController first(paths, engineFontInputs(*assets, {}, {}),
            placeholderFontFamily(*assets, *copies), services, [](Qt::ScreenOrientation) {});
        android::ReaderController second(paths, engineFontInputs(*assets, {}, {}),
            placeholderFontFamily(*assets, *copies), services, [](Qt::ScreenOrientation) {});
        const auto drain = qScopeGuard([&] {
            second.close(); first.close();
            if (!QTest::qWaitFor([&] { return first.idle() && second.idle(); }, 10000))
                QTest::qFail("Readers did not drain", __FILE__, __LINE__);
        });
        connect(&first, &android::ReaderController::opened, &first, [&] { first.window()->show(); });
        connect(&second, &android::ReaderController::opened, &second, [&] { second.window()->show(); });
        QSignalSpy firstOpened(&first, &android::ReaderController::opened);
        QSignalSpy secondOpened(&second, &android::ReaderController::opened);
        first.open(book, QSize(600, 800), "first-window");
        QTRY_COMPARE_WITH_TIMEOUT(firstOpened.count(), 1, 10000);
        QTRY_VERIFY(first.window()->readyForPresentation());
        // Put an active interval on disk before testing recovery ownership.
        first.window()->checkpoint();
        second.open(book, QSize(600, 800), "second-window");
        QTRY_COMPARE_WITH_TIMEOUT(secondOpened.count(), 1, 10000);
        QTRY_VERIFY(second.window()->readyForPresentation());
        int live = 0;
        QVERIFY(AnnotationDatabase::transaction(paths.directory, [&](QSqlDatabase &db) {
            QSqlQuery query(db);
            if (!query.exec("SELECT COUNT(*) FROM reading_sessions WHERE source_key='first-window' AND status='active'") || !query.next()) return false;
            live = query.value(0).toInt();
            return true;
        }));
        QCOMPARE(live, 1);
    }
    void standaloneHistorySourceKeys() {

        QTemporaryDir directory;
        storage::Paths paths{directory.path()};
        {
            ReadingHistory interrupted(paths.directory, "old-book", {}, {}, "Old", "Author");
            interrupted.start(0, "/body");
            interrupted.update(100, "/body/p[2]");
            QVERIFY(interrupted.save());
        }
        const auto value = [&](const QString &statement) {
            QVariant result;
            const bool ok = AnnotationDatabase::transaction(paths.directory, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                if (!query.exec(statement) || !query.next()) return false;
                result = query.value(0);
                return true;
            });
            if (!ok) QTest::qFail("Cannot inspect controller history", __FILE__, __LINE__);
            return result;
        };
        android::ReaderController controller(paths, engineFontInputs(*assets, {}, {}),
            placeholderFontFamily(*assets, *copies), [] {
                auto services = desktop::readerServices();
                services.suspended = [] { return false; };
                return services;
            }, [](Qt::ScreenOrientation) {});
        const auto drain = qScopeGuard([&] {
            controller.close();
            if (!QTest::qWaitFor([&] { return controller.idle(); }, 10000))
                QTest::qFail("History controller did not drain", __FILE__, __LINE__);
        });
        connect(&controller, &android::ReaderController::opened, &controller, [&] {
            controller.window()->show();
        });
        QSignalSpy opened(&controller, &android::ReaderController::opened);
        controller.open(book, QSize(600, 800), "content://library/first");
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(controller.window()->readyForPresentation());
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='interrupted'").toInt(), 1);
        QCOMPARE(value("SELECT active_ms FROM reading_sessions WHERE identity='old-book'").toLongLong(), qint64(100));
        const QString second = directory.path() + "/second.epub";
        QVERIFY(QFile::copy(book, second));
        controller.open(second, QSize(600, 800), "content://library/second");
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 10000);
        QTRY_VERIFY(controller.window()->readyForPresentation());
        controller.close();
        QTRY_VERIFY(controller.idle());
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE source_key='content://library/first' AND status='completed'").toInt(), 1);
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE source_key='content://library/second' AND status='completed'").toInt(), 1);
        QCOMPARE(value("SELECT COUNT(*) FROM reading_sessions WHERE status='active'").toInt(), 0);
    }
    void booxRetirementFrame() {

        QTemporaryDir directory;
        storage::Paths paths{directory.path()};
        QVERIFY(storage::writeJson(paths.preferences(), {{"default_menu_view", "none"}, {"highlight_help_seen", true}}));
        android::ReaderController controller(paths, engineFontInputs(*assets, {}, {}),
                                             placeholderFontFamily(*assets, *copies), desktop::readerServices,
                                             [](Qt::ScreenOrientation) {});
        const auto drain = qScopeGuard([&] {
            controller.close();
            if (!QTest::qWaitFor([&] { return controller.idle(); }, 10000))
                QTest::qFail("Reader did not drain after retirement", __FILE__, __LINE__);
        });
        QWidget window;
        auto *layout = new QStackedLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *canvas = new android::WidgetCanvas(&window);
        layout->addWidget(canvas);
        window.resize(600, 800);
        window.show();
        android::SurfaceHandoff handoff(window);
        connect(&controller, &android::ReaderController::opened, &window, [&] {
            canvas->setContent(controller.window());
            canvas->fitContent();
            controller.window()->show();
        });
        QSignalSpy opened(&controller, &android::ReaderController::opened);
        QSignalSpy retiring(&controller, &android::ReaderController::retiring);
        bool visibleAtRetirement = false;
        QImage lastFrame;
        connect(&controller, &android::ReaderController::retiring, &window, [&] {
            visibleAtRetirement = controller.window() && controller.window()->isVisible();
            handoff.hold();
            lastFrame = window.grab().toImage();
        });
        controller.open(book, QSize(600, 800));
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(controller.window()->readyForPresentation());
        QCoreApplication::processEvents();
        canvas->viewport()->repaint();
        const auto before = window.grab().toImage();
        int beforeInk = 0;
        for (int y = 0; y < before.height(); ++y)
            for (int x = 0; x < before.width(); ++x)
                beforeInk += qGray(before.pixel(x, y)) < 100;
        QVERIFY(beforeInk > 100);
        handoff.capture();
        controller.window()->close();
        QTRY_VERIFY_WITH_TIMEOUT(controller.idle(), 10000);
        QCOMPARE(retiring.count(), 1);
        QVERIFY(visibleAtRetirement);
        QVERIFY(handoff.active());
        QVERIFY(!lastFrame.isNull());
        int ink = 0;
        for (int y = 0; y < lastFrame.height(); ++y)
            for (int x = 0; x < lastFrame.width(); ++x)
                ink += qGray(lastFrame.pixel(x, y)) < 100;
        QVERIFY(ink > 100);
    }
    void booxMenuDismissalRefresh_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void booxMenuDismissalRefresh() {
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["default_menu_view"] = "none";
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        auto *reader = fixture.window.readerWindow();
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        QVERIFY(reader && canvas);
        const QRegion damage(QRect(0, 0, 100, 100));
        android::takeRefreshFrame(reader, canvas->viewport(), damage);
        for (int cycle = 0; cycle < 3; ++cycle) {
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrControlsTop"));
            const auto opening = android::takeRefreshFrame(reader, canvas->viewport(), damage);
            QCOMPARE(opening.clean, dark);
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(!fixture.visible("rrControlsTop"));
            QTRY_VERIFY(reader->property("reader_full_flash").toBool());
            // An unavailable Android surface must retain the cleanup request.
            bool rejectedClean = false;
            QVERIFY(!android::prepareRefreshFrame(reader, canvas->viewport(), damage, [&](const auto &frame) {
                rejectedClean = frame.clean;
                return false;
            }));
            QVERIFY(rejectedClean);
            QVERIFY(reader->property("reader_full_flash").toBool());
            android::BooxFrameQueue queue;
            int cleanFrames = 0;
            QVERIFY(android::prepareRefreshFrame(reader, canvas->viewport(), damage, [&](const auto &frame) {
                return queue.prepare(frame, [&](const auto &submitted) {
                    cleanFrames += submitted.clean;
                    return true;
                });
            }));
            QCOMPARE(cleanFrames, 1);
            QVERIFY(!queue.pending());
            QVERIFY(!android::takeRefreshFrame(reader, canvas->viewport(), damage).clean);
        }
    }
    void annotationDefaultView_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("menuView");
        for (bool dark : {false, true})
            for (const auto *view : {"stats", "seek"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(view, dark ? "dark" : "light")))
                    << dark << QString::fromLatin1(view);
    }
    void annotationDefaultView() {
        QFETCH(bool, dark);
        QFETCH(QString, menuView);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["default_menu_view"] = menuView;
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        auto *toggle = qobject_cast<QAbstractButton *>(fixture.widget("rrAnnotate"));
        QVERIFY(page && toggle);
        const auto position = fixture.state().value("position");
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        QTRY_VERIFY(!fixture.visible("rrControlsTop"));
        QVERIFY(toggle->isChecked());
        for (int pass = 0; pass < 2; ++pass) {
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible(menuView == "stats" ? "rrChapterGraph" : "rrPageScrubber"));
            QVERIFY(toggle->isChecked());
            QCOMPARE(fixture.state().value("position"), position);
            QVERIFY(fixture.press("rrNavChapters"));
            QTRY_VERIFY(fixture.visible("rrChoices"));
            QVERIFY(!fixture.visible("rrChapterGraph"));
            QVERIFY(!fixture.visible("rrPageScrubber"));
            QVERIFY(fixture.press("rrContentsClose"));
            QTRY_VERIFY(!fixture.visible("rrChoices"));
            QTRY_VERIFY(fixture.visible(menuView == "stats" ? "rrChapterGraph" : "rrPageScrubber"));
            if (menuView == "seek") {
                auto *scrubber = dynamic_cast<ui::PageScrubber *>(fixture.widget("rrPageScrubber"));
                QVERIFY(scrubber);
                QTRY_VERIFY(scrubber->previewReady());
                QVERIFY(fixture.click(scrubber, scrubber->previewBounds().center()));
            } else {
                QVERIFY(fixture.press("rrPage"));
            }
            QTRY_VERIFY(!fixture.visible("rrControlsTop"));
            QCOMPARE(fixture.state().value("position"), position);
            QVERIFY(toggle->isChecked());
            const auto currentWord = fixture.wordPoint(0);
            QVERIFY(currentWord.x() >= 0);
            QVERIFY(fixture.selectWord(page, currentWord));
            QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
            QVERIFY(!fixture.visible("rrChapterGraph"));
            QVERIFY(!fixture.visible("rrChoices"));
            QVERIFY(!fixture.visible("rrDictionary"));
            QVERIFY(fixture.click(page, currentWord));
            QTRY_VERIFY(fixture.visible("rrAnnotationCancel"));
            QVERIFY(fixture.press("rrAnnotationCancel"));
            QTRY_VERIFY(page->annotationHandleRect(true).isEmpty());
            QCOMPARE(fixture.state().value("position"), position);
        }
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("default_menu_view").toString(), menuView);
    }
    void bookmarkReturn_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<QString>("menuView");
        QTest::addColumn<QString>("detour");
        for (bool dark : {false, true}) {
            const auto theme = dark ? "dark" : "light";
            for (const auto &mode : {"none", "stats", "seek"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode, theme))) << dark << QString(mode) << QString();
            for (const auto &tab : {"Stats", "Chapters"})
                QTest::newRow(qPrintable(QString("default-via-%1-%2").arg(tab, theme)))
                    << dark << QString("none") << QString("rrNav%1").arg(tab);
        }
    }
    void bookmarkReturn() {
        QFETCH(bool, dark);
        QFETCH(QString, menuView);
        QFETCH(QString, detour);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["default_menu_view"] = menuView;
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_COMPARE(fixture.state().value("bookmark_count").toInt(), 1);
        const auto destination = fixture.state().value("position");
        for (int i = 0; i < 4; ++i) {
            const int previous = fixture.state().value("page").toInt();
            QVERIFY(fixture.turnPage(1));
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QTRY_VERIFY(fixture.state().value("page").toInt() > previous);
        }
        const auto origin = fixture.state().value("position");
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrBookmarks"));
        QVERIFY(fixture.press("rrBookmark0"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("position"), destination);
        QVERIFY(fixture.press("rrPage"));
        if (!detour.isEmpty()) {
            QVERIFY(fixture.press(qPrintable(detour)));
            QTRY_VERIFY(fixture.visible(detour == "rrNavStats" ? "rrChapterGraph" : "rrChoices"));
        }
        if (menuView != "seek")
            QVERIFY(fixture.press("rrNavSeek"));
        auto *scrubber = dynamic_cast<ui::PageScrubber *>(fixture.widget("rrPageScrubber"));
        QVERIFY(scrubber);
        QTRY_VERIFY(scrubber->previewReady());
        QTRY_VERIFY(fixture.visible("rrScrubberReturn"));
        QVERIFY(fixture.press("rrScrubberReturn"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(!fixture.visible("rrScrubberReturn"));
    }
    // Kobo's Large Print Mode enlarges interface text, at most to 150%.
    // Controls keep their size.
    void largeInterfaceScale() {
        // Kobo shows the Light tab and names the exit action Exit Kobalt.
        Fixture fixture(false, false, false, true, false, false, [](ReaderHostServices &services) {
            services.lightAvailable = [] { return true; };
            services.backButtonLabel = strings::get("reader.exit");
        });
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(1);
        auto *textSize = fixture.window.findChild<QComboBox *>("desktopTextSizePicker");
        textSize->setCurrentIndex(textSize->findData(150));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        if (!fixture.visible("rrBottomNavigation"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBottomNavigation"));
        // The desktop picker sets the same scales as Kobo and Android.
        QCOMPARE(ui::textScale(), 1.5);
        QCOMPARE(ui::metrics::chrome::tabIcon, qRound(ui::px(48) * 1.2));
        const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
        const auto save = [&](const QString &name) {
            QCoreApplication::processEvents();
            return output.isEmpty() || fixture.window.readerWindow()->grab().save(output + "/large-" + name + ".png");
        };
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(fixture.window.grab().save(output + "/large-window.png"));
        }
        QVERIFY(save("menu"));
        // The tabs keep their size, so their larger labels give way to icons.
        for (const char *name : {"rrNavChapters", "rrNavSeek", "rrNavText", "rrNavLayout", "rrNavLight", "rrNavStats"})
            QVERIFY2(fixture.widget(name)->property("labels_hidden").toBool(), name);
        QVERIFY(fixture.widget("rrReturn")->width() > int(ui::metrics::chrome::cellWidth));
        // The status line grows with its text.
        auto *clock = qobject_cast<QLabel *>(fixture.widget("rrMenuClock"));
        QVERIFY(clock);
        QCOMPARE(clock->height(), qRound(ui::px(40) * 1.5));
        QVERIFY(ui::interfaceTextWidth(clock->text(), clock->font()) <= clock->contentsRect().width());
        QCOMPARE(clock->font().pixelSize(), qRound(ui::px(30) * 1.5));
        QVERIFY(fixture.press("rrNavText"));
        QTRY_VERIFY(fixture.visible("rrFont"));
        QVERIFY(fixture.visible("rrTextSize"));
        QVERIFY(save("text"));
        QVERIFY(fixture.press("rrNavLayout"));
        QVERIFY(save("layout"));
        QVERIFY(fixture.press("rrNavStats"));
        QVERIFY(save("stats"));
        QVERIFY(fixture.press("rrNavChapters"));
        QVERIFY(save("chapters"));
        QVERIFY(fixture.press("rrNavChapters"));
        QVERIFY(fixture.press("rrSettings"));
        QTRY_VERIFY(fixture.visible("rrSettingsTitle"));
        QCOMPARE(fixture.widget("rrSettingsTitle")->contentsMargins().left(), int(ui::metrics::chrome::inset));
        // A larger option title moves its description down instead of covering it.
        auto *option = dynamic_cast<ui::DescribedOption *>(fixture.widget("rrDarkMode"));
        QVERIFY(option);
        QFont label = option->font();
        label.setPixelSize(option->presentation().labelPixels);
        QVERIFY(option->height() >= ui::interfaceLineHeight(label) + int(ui::metrics::settings::descriptionBottom));
        QVERIFY(save("settings"));
        QVERIFY(fixture.press("rrSettingsOptionsNext"));
        QVERIFY(save("settings-2"));
    }
    // A caption that does not fit beside the toolbar actions leaves Exit with
    // its arrow alone, in a cell of the actions' width.
    void largeInterfaceScaleLongExit() {
        Fixture fixture(false, false, false, true, false, false, [](ReaderHostServices &services) {
            services.backButtonLabel = QString(QStringLiteral("Return to the library ")).repeated(2).trimmed();
        });
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(1);
        auto *textSize = fixture.window.findChild<QComboBox *>("desktopTextSizePicker");
        textSize->setCurrentIndex(textSize->findData(150));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        if (!fixture.visible("rrBottomNavigation"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBottomNavigation"));
        QCOMPARE(fixture.widget("rrReturn")->width(), int(ui::metrics::chrome::cellWidth));
        // The tab bar grows with the icon and label, so the label keeps the
        // space below it that it has at the standard size.
        auto *tab = fixture.widget("rrNavChapters");
        QVERIFY(!tab->property("labels_hidden").toBool());
        const auto tabImage = tab->grab().toImage();
        int inkBottom = -1;
        for (int y = 0; y < tabImage.height(); ++y)
            for (int x = tabImage.width() / 4; x < tabImage.width() * 3 / 4; ++x)
                if (qGray(tabImage.pixel(x, y)) < 128)
                    inkBottom = y;
        const int standardCapital =
            ui::interfaceInkBounds(QStringLiteral("H"), ui::interfaceFont(ui::px(26))).height();
        const int standardSpace = (ui::px(112) - ui::px(3) - ui::px(48) - ui::px(10) - standardCapital) / 2;
        QVERIFY2(tabImage.height() - 1 - inkBottom >= standardSpace - 1,
                 qPrintable(QStringLiteral("%1 below the label, %2 at the standard size")
                                .arg(tabImage.height() - 1 - inkBottom)
                                .arg(standardSpace)));
        const auto caption = fixture.widget("rrReturnTitle")->grab().toImage();
        for (int y = 0; y < caption.height(); ++y)
            for (int x = 0; x < caption.width(); ++x)
                QVERIFY2(qAlpha(caption.pixel(x, y)) <= 128 || qGray(caption.pixel(x, y)) > 128,
                         "the exit caption must not draw in the arrow's cell");
        // The Library and the reader use one text size.
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QVERIFY(start);
        const auto queryPixels = [start] {
            auto *query = start->library()->findChild<QLineEdit *>("librarySearchQuery");
            query->ensurePolished();
            return query->font().pixelSize();
        };
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY(!fixture.window.readerWindow());
        QTRY_VERIFY(start->libraryShown());
        QCOMPARE(ui::textScale(), 1.5);
        QCOMPARE(queryPixels(), qRound(ui::px(36) * 1.5));
        // A failed opening shows its error in the Library at the same size.
        const auto broken = fixture.directory.path() + "/broken.epub";
        QFile file(broken);
        QVERIFY(file.open(QIODevice::WriteOnly) && file.write("not a book") > 0);
        file.close();
        qreal failedScale = 0;
        QObject::connect(&fixture.window, &desktop::Window::bookFailed, &fixture.window,
                         [&failedScale] { failedScale = ui::textScale(); });
        fixture.window.openBook(broken);
        QTRY_VERIFY(failedScale > 0);
        QCOMPARE(failedScale, 1.5);
        QCOMPARE(queryPixels(), qRound(ui::px(36) * 1.5));
        // Without an open book, the picker restyles the Library at once.
        textSize->setCurrentIndex(textSize->findData(100));
        QCOMPARE(ui::textScale(), 1.0);
        QCOMPARE(queryPixels(), int(ui::px(36)));
        QVERIFY(start->library()->isVisible());
    }
    void navigation_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("defaultNavigate");
        QTest::newRow("light") << false << false;
        QTest::newRow("dark") << true << false;
        QTest::newRow("default-navigate-light") << false << true;
        QTest::newRow("default-navigate-dark") << true << true;
    }
    void navigation() {
        QFETCH(bool, dark);
        QFETCH(bool, defaultNavigate);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["default_menu_view"] = defaultNavigate ? "seek" : "none";
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        if (!defaultNavigate)
            openPrologueBody(fixture);
        const auto origin = fixture.state().value("position");
        if (!fixture.visible("rrBottomNavigation"))
            QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrBottomNavigation"));
        if (!defaultNavigate) {
            auto *bar = fixture.widget("rrBottomNavigation");
            QCOMPARE(bar->grab().toImage().pixelColor(bar->width() / 2, 0), bar->palette().color(QPalette::WindowText));
            QVERIFY(fixture.press("rrNavSeek"));
        }
        QTRY_VERIFY(fixture.visible("rrBookPosition"));
        auto *scrubber = dynamic_cast<ui::PageScrubber *>(fixture.widget("rrPageScrubber"));
        QVERIFY(scrubber);
        QTRY_VERIFY(scrubber->previewReady());
        if (defaultNavigate) {
            auto *bar = fixture.widget("rrBottomNavigation");
            QCOMPARE(bar->grab().toImage().pixelColor(bar->width() / 2, 0), bar->palette().color(QPalette::WindowText));
            auto *seek = fixture.widget("rrNavSeek");
            QVERIFY(seek && seek->isHidden());
            const int x = seek->mapTo(bar, seek->rect().center()).x();
            QCOMPARE(bar->grab().toImage().pixelColor(x, int(ui::metrics::control::border)),
                     bar->palette().color(QPalette::Window));
        }
        const auto output = qEnvironmentVariable("KOBALT_TEST_CAPTURE_DIR");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(fixture.window.grab().save(output + (dark ? "/navigate-dark.png" : "/navigate-light.png")));
        }
        const int initial = scrubber->selectedPage();
        if (defaultNavigate) {
            QCOMPARE(initial, 0);
            fixture.window.readerWindow()->pageButton(-1);
            QCOMPARE(scrubber->selectedPage(), 0);
            QCOMPARE(fixture.state().value("position"), origin);
        }
        fixture.window.readerWindow()->pageButton(1);
        QTRY_COMPARE(scrubber->selectedPage(), initial + 1);
        QCOMPARE(fixture.state().value("position"), origin);
        fixture.window.readerWindow()->pageButton(-1);
        QTRY_COMPARE(scrubber->selectedPage(), initial);
        QCOMPARE(fixture.state().value("position"), origin);
        if (defaultNavigate) {
            fixture.window.readerWindow()->pageButton(1);
            QTRY_COMPARE(scrubber->selectedPage(), initial + 1);
            QTRY_VERIFY(scrubber->previewReady());
            QVERIFY(fixture.click(scrubber, scrubber->previewBounds().center()));
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QTRY_COMPARE(fixture.state().value("page").toInt(), initial + 1);
            return;
        }
        QVERIFY(fixture.click(scrubber->slider(), {scrubber->slider()->width() * 3 / 4, scrubber->slider()->height() / 2}));
        QTRY_VERIFY(scrubber->previewReady());
        const int destination = scrubber->selectedPage();
        QVERIFY(destination != fixture.state().value("page").toInt());
        QCOMPARE(fixture.state().value("position"), origin);
        const auto center = scrubber->previewBounds().center();
        const int distance = scrubber->previewBounds().width() / 3;
        QVERIFY(fixture.drag(scrubber, center + QPoint(distance / 2, 0), center - QPoint(distance / 2, 0)));
        QTRY_COMPARE(scrubber->selectedPage(), destination + 1);
        QTRY_VERIFY(scrubber->previewReady());
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.drag(scrubber, center - QPoint(distance / 2, 0), center + QPoint(distance / 2, 0)));
        QTRY_COMPARE(scrubber->selectedPage(), destination);
        QTRY_VERIFY(scrubber->previewReady());
        // A vertical drag must neither open the preview nor turn a page.
        QVERIFY(fixture.drag(scrubber, center, center + QPoint(0, distance)));
        QCOMPARE(scrubber->selectedPage(), destination);
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.click(fixture.widget("rrPreviewNextPage"), {10, fixture.widget("rrPreviewNextPage")->height() / 2}));
        QTRY_COMPARE(scrubber->selectedPage(), destination + 1);
        QTRY_VERIFY(scrubber->previewReady());
        QVERIFY(fixture.click(fixture.widget("rrPreviewPreviousPage"), {fixture.widget("rrPreviewPreviousPage")->width() - 10, fixture.widget("rrPreviewPreviousPage")->height() / 2}));
        QTRY_COMPARE(scrubber->selectedPage(), destination);
        QTRY_VERIFY(scrubber->previewReady());
        QVERIFY(fixture.press("rrNavSeek"));
        QTRY_VERIFY(!scrubber->selecting());
        QCOMPARE(fixture.state().value("position"), origin);
        QVERIFY(fixture.press("rrNavSeek"));
        QTRY_VERIFY(fixture.visible("rrBookPosition"));
        QVERIFY(fixture.click(scrubber->slider(), {scrubber->slider()->width() * 3 / 4, scrubber->slider()->height() / 2}));
        QTRY_VERIFY(scrubber->previewReady());
        const int accepted = scrubber->selectedPage();
        QVERIFY(fixture.click(scrubber, scrubber->previewBounds().center()));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_COMPARE(fixture.state().value("page").toInt(), accepted);
        QCOMPARE(opened.count(), 1);

        // Exercise real touch sequences independently of mouse emulation.
        ui::PageScrubber touchPreview(nullptr);
        touchPreview.resize(800, 1200);
        touchPreview.setBook(1, 3, {}, {600, 900});
        touchPreview.show();
        QVERIFY(QTest::qWaitForWindowExposed(&touchPreview));
        touchPreview.previewRequested = [&](int page, QSize size) {
            QImage image(size, QImage::Format_RGB32);
            image.fill(Qt::white);
            touchPreview.setPreview(page, image);
        };
        int committed = -1;
        touchPreview.accepted = [&](int page) { committed = page; };
        touchPreview.showCurrentPage();
        QTRY_VERIFY(touchPreview.previewReady());
        auto *device = QTest::createTouchDevice();
        auto touch = QTest::touchEvent(&touchPreview, device, false);
        const auto middle = touchPreview.previewBounds().center();
        const QPoint travel(100, 0);
        const auto swipe = [&](QPoint from, QPoint to) {
            touch.press(0, from).commit();
            touch.move(0, to).commit();
            touch.release(0, to).commit();
        };
        swipe(middle + travel, middle - travel);
        QCOMPARE(touchPreview.selectedPage(), 2);
        QTRY_VERIFY(touchPreview.previewReady());
        QCOMPARE(committed, -1);
        swipe(middle + travel, middle - travel);
        QCOMPARE(touchPreview.selectedPage(), 2); // Last page clamps.
        swipe(middle - travel, middle + travel);
        QCOMPARE(touchPreview.selectedPage(), 1);
        QTRY_VERIFY(touchPreview.previewReady());
        swipe(middle - travel, middle + travel);
        QCOMPARE(touchPreview.selectedPage(), 0);
        QTRY_VERIFY(touchPreview.previewReady());
        swipe(middle - travel, middle + travel);
        QCOMPARE(touchPreview.selectedPage(), 0); // First page clamps.
        auto *side = touchPreview.findChild<QWidget *>("rrPreviewNextPage");
        QVERIFY(side && side->isVisible());
        const auto sideStart = side->pos() + QPoint(10, side->height() / 2);
        swipe(sideStart, sideStart - travel);
        QCOMPARE(touchPreview.selectedPage(), 1);
        QTRY_VERIFY(touchPreview.previewReady());
        QCOMPARE(committed, -1);
        // Multiple fingers cancel the whole sequence, even after one lifts.
        touch.press(0, middle).commit();
        touch.stationary(0).press(1, middle + travel).commit();
        touch.move(0, middle - travel).stationary(1).commit();
        touch.release(0, middle - travel).release(1, middle + travel).commit();
        QCOMPARE(touchPreview.selectedPage(), 1);
        QCOMPARE(committed, -1);
        touch.press(0, middle).commit();
        QEvent cancel(QEvent::TouchCancel);
        QApplication::sendEvent(&touchPreview, &cancel);
        touch.release(0, middle).commit();
        QCOMPARE(committed, -1);
        touch.press(0, middle).commit();
        touch.release(0, middle).commit();
        QCOMPARE(committed, 1);
    }
    void annotationTwoFingerTapConsumesWholeSequence() {
        ReadingPage page(nullptr, [] {});
        page.resize(600, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *device = QTest::createTouchDevice();
        auto touch = QTest::touchEvent(&page, device, false);
        const QPoint first(240, 420), second(340, 470);
        int requests = 0, taps = 0, holds = 0, swipes = 0, lightChanges = 0;
        page.annotationRequested = [&] {
            ++requests;
        };
        page.tapped = [&](QPoint) {
            ++taps;
        };
        page.held = [&](QPoint) {
            ++holds;
        };
        page.swiped = [&](int) {
            ++swipes;
        };
        page.lightChanged = [&](double) {
            ++lightChanges;
        };
        auto releaseBoth = [&] {
            touch.release(0, first, &page).release(1, second, &page).commit();
            QCoreApplication::processEvents();
        };
        touch.press(0, first, &page).press(1, second, &page).commit();
        releaseBoth();
        QCOMPARE(requests, 1);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        // Releasing one finger must not turn the remaining finger into a tap.
        touch.press(0, first, &page).commit();
        touch.move(0, first + QPoint(1, 0), &page).commit();
        touch.stationary(0).press(1, second, &page).commit();
        touch.release(1, second, &page).stationary(0).commit();
        touch.release(0, first, &page).commit();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        // A failed two-finger swipe is consumed, including its last release.
        touch.press(0, first, &page).press(1, second, &page).commit();
        touch.move(0, first + QPoint(180, 0), &page).stationary(1).commit();
        releaseBoth();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        // Three contacts and late second contacts never become shortcuts.
        touch.press(0, first, &page).press(1, second, &page).press(2, QPoint(300, 430), &page).commit();
        touch.release(0, first, &page).release(1, second, &page).release(2, QPoint(300, 430), &page).commit();
        touch.press(0, first, &page).commit();
        QTest::qWait(170);
        touch.stationary(0).press(1, second, &page).commit();
        releaseBoth();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        touch.press(0, first, &page).press(1, second, &page).commit();
        QTest::qWait(370);
        releaseBoth();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        touch.press(0, QPoint(60, 420), &page).press(1, second, &page).commit();
        touch.release(0, QPoint(60, 420), &page).release(1, second, &page).commit();
        touch.press(0, first, &page).press(1, second, &page).commit();
        QTouchEvent cancelled(QEvent::TouchCancel, device);
        QApplication::sendEvent(&page, &cancelled);
        releaseBoth();
        QCOMPARE(requests, 2);
        QCOMPARE(taps + holds + swipes + lightChanges, 0);
        // Single-finger reading remains available after failed gestures.
        touch.press(0, first, &page).commit();
        touch.release(0, first, &page).commit();
        QCOMPARE(taps, 1);
        QCOMPARE(holds + swipes + lightChanges, 0);
    }
    void annotationRightClickRejectsChords() {
        ReadingPage page(nullptr, [] {});
        page.resize(600, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        int requests = 0;
        page.annotationRequested = [&] {
            ++requests;
        };
        const QPoint point(300, 450);
        QTest::mouseRelease(&page, Qt::RightButton, Qt::NoModifier, point);
        QCOMPARE(requests, 0);
        QTest::mousePress(&page, Qt::RightButton, Qt::NoModifier, point);
        QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(&page, Qt::RightButton, Qt::NoModifier, point);
        QCOMPARE(requests, 0);
        QTest::mousePress(&page, Qt::RightButton, Qt::NoModifier, point);
        QTest::mouseRelease(&page, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(&page, Qt::RightButton, Qt::NoModifier, point);
        QCOMPARE(requests, 0);
        QTest::mouseClick(&page, Qt::RightButton, Qt::NoModifier, point);
        QCOMPARE(requests, 1);
    }
    void stylesheet() {
        Fixture fixture;
        QVERIFY(fixture.prepared);
        // Isolate CSS reload from the publisher styles of the general book.
        QVERIFY(createBook(fixture.directory.path()));
        const auto styledBook = fixture.directory.path() + "/text.epub";
        const auto cssPath = fixture.paths.directory + "/userstyle.css";
        QFile css(cssPath);
        const QByteArray rules = "p { color: #ff0000 !important; }";
        QVERIFY(css.open(QIODevice::WriteOnly));
        QCOMPARE(css.write(rules), rules.size());
        css.close();
        const auto redPixels = [&] {
            const auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
            if (!page)
                return 0;
            const auto image = page->image.convertToFormat(QImage::Format_RGB32);
            int red = 0;
            for (int y = 0; y < image.height(); ++y) {
                const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                for (int x = 0; x < image.width(); ++x)
                    red += qRed(row[x]) > 180 && qGreen(row[x]) < 80 && qBlue(row[x]) < 80;
            }
            return red;
        };
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(styledBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        QVERIFY(redPixels() > 100);
        QVERIFY(QFile::remove(cssPath));
        QVERIFY(redPixels() > 100); // Changes apply on the next opening.
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrReturn"));
        QVERIFY(fixture.press("rrReturn"));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.window.idle(), 30000);
        fixture.window.openBook(styledBook);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        QCOMPARE(redPixels(), 0);
    }
};

void checkPageAnimationCompatibility(int preset, bool dark) {
    for (const auto support : {PageAnimationSupport::Supported, PageAnimationSupport::Uncertain,
                               PageAnimationSupport::Unavailable}) {
        Fixture fixture(dark, false, false, false, false, false, [support](ReaderHostServices &services) {
            services.animationSupport = [support] { return support; };
        });
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        if (!fixture.visible("rrSettings")) QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        // A narrow screen opens Settings on its section list.
        QTRY_VERIFY(fixture.visible("rrPageAnimations") || fixture.visible("rrSettingsRail"));
        if (!fixture.visible("rrPageAnimations")) QVERIFY(fixture.press("rrSettingsDisplay"));
        QTRY_VERIFY(fixture.visible("rrPageAnimations"));
        auto *option = dynamic_cast<ui::DescribedOption *>(fixture.widget("rrPageAnimations"));
        auto *darkOption = dynamic_cast<ui::DescribedOption *>(fixture.widget("rrDarkMode"));
        QVERIFY(option && darkOption);
        QCOMPARE(option->isEnabled(), pageAnimationsAvailable(support));
        QCOMPARE(option->accessibleDescription(), strings::get(support == PageAnimationSupport::Uncertain
            ? "settings.animations_uncertain_description" : support == PageAnimationSupport::Supported
            ? "settings.animations_description" : "settings.unsupported_description"));
        auto *accessible = QAccessible::queryAccessibleInterface(option);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), strings::get("reader.enable_page_animations"));
        QCOMPARE(option->font(), darkOption->font());
        QCOMPARE(option->presentation().labelPixels, darkOption->presentation().labelPixels);
        QCOMPARE(option->presentation().descriptionPixels, darkOption->presentation().descriptionPixels);
        QCOMPARE(option->presentation().horizontalInset, darkOption->presentation().horizontalInset);
        QVERIFY(option->height() >= option->heightForWidth(option->width()));
        QVERIFY(!option->isChecked());
        const auto state = support == PageAnimationSupport::Supported ? "supported"
            : support == PageAnimationSupport::Uncertain ? "uncertain" : "unsupported";
        QVERIFY(capture(fixture, screenshotVariant(preset, dark) + "-settings-animations-" + state));
        if (option->isEnabled()) {
            QVERIFY(fixture.press("rrPageAnimations"));
            QVERIFY(option->isChecked());
            QVERIFY(fixture.press("rrSettingsClose"));
            QVERIFY(fixture.press("rrSettings"));
            QTRY_VERIFY(fixture.visible("rrPageAnimations") || fixture.visible("rrSettingsRail"));
            if (!fixture.visible("rrPageAnimations")) QVERIFY(fixture.press("rrSettingsDisplay"));
            QTRY_VERIFY(fixture.visible("rrPageAnimations"));
            QVERIFY(option->isChecked());
        } else {
            option->click();
            QVERIFY(!option->isChecked());
        }
    }
}

class ScreenshotTests final : public QObject {
    Q_OBJECT
private slots:
    void pressFeedback_data() { screenshotVariants(); }
    void pressFeedback() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        const auto name = screenshotVariant(preset, dark);
        Fixture fixture(dark, false, false, true, false, false, [](ReaderHostServices &services) {
            // The color palette is available on color readers, independently of theme.
            services.colorDisplay = [] { return true; };
        });
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        const auto states = [&](const char *target, const QString &scene) {
            auto *button = qobject_cast<QAbstractButton *>(fixture.widget(target));
            QVERIFY(button && button->isVisible() && button->isEnabled());
            QVERIFY(capture(fixture, name + '-' + scene));
            // Use the existing reader canvas so input follows the production path.
            auto *canvas = fixture.window.findChild<QGraphicsView *>();
            QVERIFY(canvas);
            auto *reader = fixture.window.readerWindow();
            const auto point = canvas->mapFromScene(button->mapTo(reader, button->rect().center()));
            const bool checked = button->isChecked();
            // Keep action side effects out of a visual state comparison.
            const QSignalBlocker block(button);
            QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTRY_VERIFY(button->isDown());
            QVERIFY(capture(fixture, name + '-' + scene + "-pressed"));
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            button->setChecked(checked);
            QVERIFY(!button->isDown());
            QCOMPARE(button->isChecked(), checked);
            QVERIFY(capture(fixture, name + '-' + scene + "-released"));
        };
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsKosync"));
        QVERIFY(revealSettingsOption(fixture, "rrKosyncLogin"));
        states("rrKosyncLogin", "kosync-login-feedback");
        QVERIFY(fixture.press("rrSettingsClose"));
        if (!fixture.visible("rrAnnotate")) QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        const auto word = fixture.wordPoint();
        QVERIFY(word.x() >= 0);
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QVERIFY(fixture.click(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationColor_yellow"));
        states("rrAnnotationColor_yellow", "annotation-color-feedback");
        // The Library owns a separate native canvas and the same checkbox control.
        Fixture library(dark, false, true);
        QVERIFY(library.prepared);
        library.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        auto *start = library.window.findChild<android::StartScreen *>();
        QVERIFY(start);
        start->showLibrary();
        auto *view = start->library();
        QTRY_COMPARE(view->width(), screenshotSize(preset).width());
        auto *more = view->findChild<QAbstractButton *>("libraryMore");
        QVERIFY(more);
        more->click();
        view->findChild<QAbstractButton *>("libraryMoreFolders")->click();
        auto *check = view->findChild<QAbstractButton *>("libraryCheckAtStart");
        QTRY_VERIFY(check && check->isVisible());
        QVERIFY(capture(library, name + "-library-option-feedback"));
        auto *canvas = start->libraryCanvas();
        const auto point = canvas->mapFromScene(check->mapTo(view, check->rect().center()));
        const bool checked = check->isChecked();
        const QSignalBlocker block(check);
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY(check->isDown());
        QVERIFY(capture(library, name + "-library-option-feedback-pressed"));
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        check->setChecked(checked);
        QVERIFY(!check->isDown());
        QVERIFY(capture(library, name + "-library-option-feedback-released"));
    }

    void imageViewer_data() { screenshotVariants(); }
    void imageViewer() { QFETCH(int, preset); QFETCH(bool, dark); checkImageViewer(preset, dark, true); }
    void pageAnimationCompatibility_data() { screenshotVariants(); }
    void pageAnimationCompatibility() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        checkPageAnimationCompatibility(preset, dark);
    }

    void pinnedBookmarks_data() { screenshotVariants(); }
    void pinnedBookmarks() { QFETCH(int, preset); QFETCH(bool, dark); checkPinnedBookmarks(preset, dark); }
    void headerFooterSizes_data() { screenshotVariants(); }
    void headerFooterSizes() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        checkHeaderFooterSizes(preset, dark);
    }
    void publisherFontDefault_data() { screenshotVariants(); }
    void publisherFontDefault() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        auto preferences = storage::readJson(fixture.paths.preferences());
        preferences["default_font_face"] = "";
        preferences[appearance::inheritTextLayoutKey] = false;
        QVERIFY(storage::writeJson(fixture.paths.preferences(), preferences));
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotVariant(preset, dark);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.press("rrFont"));
        QTRY_VERIFY(fixture.visible("rrFontChoice0"));
        if (phonePreset(preset)) {
            auto *cell = fixture.widget("rrFontChoice0");
            QVERIFY(cell->width() >= ui::px(300));
            QVERIFY(!fixture.visible("rrFontDefaultSlot"));
        }
        QVERIFY(capture(fixture, name + "-publisher-font-default"));
        QVERIFY(fixture.press("rrFontClose"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDefaults"));
        QVERIFY(revealSettingsOption(fixture, "rrDefaultFont"));
        QVERIFY(fixture.press("rrDefaultFont"));
        QTRY_VERIFY(fixture.visible("rrSettingsFontChoice0"));
        QVERIFY(capture(fixture, name + "-default-font-choices"));
    }
    void kosyncManual_data() { screenshotVariants(); }
    void kosyncManual() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        QVERIFY(storage::writeJson(fixture.paths.syncAccount(), {
            {"schema", 1}, {"server", "https://sync.example"}, {"username", "reader"},
            {"key", QString(32, 'a')}, {"device_id", "gallery-device"}, {"automatic", false}}));
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotDevice(preset) + ('-' + screenshotTheme(dark));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        if (!fixture.visible("rrControlsTop")) QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrKosyncManual"));
        const auto *sync = fixture.widget("rrKosyncManual");
        const auto *search = fixture.widget("rrSearchOpen");
        QVERIFY(!sync->geometry().intersects(search->geometry()));
        QVERIFY(capture(fixture, name + "-kosync-manual-toolbar"));
        QVERIFY(fixture.press("rrKosyncManual"));
        QTRY_VERIFY(fixture.visible("rrKosyncManualPrompt"));
        QVERIFY(capture(fixture, name + "-kosync-manual-prompt"));
        QVERIFY(fixture.press("rrKosyncManualCancel"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsKosync"));
        QVERIFY(capture(fixture, name + "-kosync-manual-settings"));
        auto *notice = new reader::KosyncNotice(fixture.window.readerWindow());
        notice->show();
        notice->raise();
        notice->fitCard();
        QVERIFY(capture(fixture, name + "-kosync-syncing"));
        notice->complete();
        QVERIFY(capture(fixture, name + "-kosync-completed"));
        notice->hide();
        delete notice;
    }
    void primaryViews_data() { screenshotVariants(); }
    void primaryViews() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark, false, true);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotVariant(preset, dark);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QTRY_VERIFY(start && start->libraryShown());
        // The portrait phone keeps its 60-pixel camera band above the Library.
        QTRY_COMPARE(start->library()->size(), librarySize(preset));
        QVERIFY(capture(fixture, name + "-library-home"));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(capture(fixture, name + "-reading"));
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(fixture.click(page, page->bookmarkCorner().center()));
        QTRY_VERIFY(fixture.state().value("bookmarked").toBool());
        fixBookmarkDates(fixture);
        if (QTest::currentTestFailed()) return;
        page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page);
        const auto scene = [&](const char *action, const char *visible, const char *suffix) {
            if (!fixture.press(action)) return false;
            if (!QTest::qWaitFor([&] { return fixture.visible(visible); }, 5000)) return false;
            return capture(fixture, name + '-' + suffix);
        };
        QVERIFY(scene("rrPage", "rrSettings", "menu"));
        QCOMPARE(!fixture.widget("rrHeaderFooterPreset_phone")->isHidden(), phonePreset(preset));
        if (phonePreset(preset)) {
            QCOMPARE(fixture.state().value("header_footer_preset").toString(), QString("phone"));
            QVERIFY(!page->chrome.header());
            // Narrow screens show tab icons only; a phone in landscape has room for labels.
            for (const auto *tab : {"rrNavChapters", "rrNavSeek", "rrNavText", "rrNavLayout", "rrNavStats"})
                QCOMPARE(fixture.widget(tab)->property("labels_hidden").toBool(), preset == 3);
        }

        QVERIFY(scene("rrNavChapters", "rrContents", "chapters"));
        QVERIFY(fixture.press("rrChapter1"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        advanceIntoBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(scene("rrNavStats", "rrChapterGraph", "stats"));
        QVERIFY(fixture.press("rrNavStats"));
        QVERIFY(scene("rrNavText", "rrFont", "text"));
        QVERIFY(scene("rrFont", "rrChoices", "fonts"));
        QVERIFY(fixture.press("rrFontClose"));
        QVERIFY(scene("rrNavLayout", "rrHeaderFooter", "layout"));
        QVERIFY(scene("rrSettings", "rrGeneralSettings", "settings"));
        QVERIFY(fixture.press("rrSettingsClose"));
        if (!fixture.visible("rrBookmarks")) QVERIFY(fixture.press("rrPage"));
        QVERIFY(scene("rrBookmarks", "rrBookmarkList", "bookmarks"));
        QVERIFY(fixture.press("rrBookmarkListClose"));
        if (!fixture.visible("rrSearchOpen")) QVERIFY(fixture.press("rrPage"));
        QVERIFY(scene("rrSearchOpen", "rrSearchQuery", "search-empty"));
        auto *query = qobject_cast<QLineEdit *>(fixture.widget("rrSearchQuery"));
        QVERIFY(query);
        query->setText("Trevelyan");
        QVERIFY(scene("rrSearchSubmit", "rrSearchResult0", "search"));
        QVERIFY(fixture.press("rrSearchResult0"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrAnnotate"));
        const auto word = fixture.wordPoint();
        QVERIFY(word.x() >= 0);
        QVERIFY(fixture.selectWord(page, word));
        QTRY_VERIFY(!page->annotationHandleRect(true).isEmpty());
        QVERIFY(fixture.click(page, word));
        QTRY_VERIFY(fixture.visible("rrAnnotationSave"));
        QVERIFY(capture(fixture, name + "-annotation-actions"));
        QVERIFY(fixture.press("rrAnnotationStyle_shaded"));
        QVERIFY(fixture.press("rrAnnotationSave"));
        QTRY_COMPARE(fixture.stored("highlights.json", "highlights").size(), 1);
        QTRY_VERIFY(!fixture.visible("rrAnnotationPopover"));
        captureReturnPressed(fixture, name);
    }
    void widgetKeyboard_data() { screenshotVariants(); }
    void widgetKeyboard() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        const QLocale originalLocale;
        const auto restoreLocale = qScopeGuard([originalLocale] { QLocale::setDefault(originalLocale); });
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        Fixture fixture(dark, false, true);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        auto *start = fixture.window.findChild<android::StartScreen *>();
        auto *view = start->library();
        QTRY_VERIFY(view->isVisible() && view->width() > 600);
        view->findChild<QPushButton *>("librarySearch")->click();
        auto *editor = view->findChild<QLineEdit *>("librarySearchQuery");
        QVERIFY(editor && editor->isVisible());
        auto *keyboard = view->findChild<QWidget *>("widgetKeyboard");
        QVERIFY(keyboard && keyboard->isVisible());
        auto *characterKey = keyboard->findChild<QPushButton *>("widgetKey1_0");
        QVERIFY(characterKey);
        QCOMPARE(characterKey->font().family(), ui::interfaceFont().family());
        QCOMPARE(characterKey->font().pixelSize(), ui::px(48));
        auto *shift = keyboard->findChild<QPushButton *>("widgetKeyboardShift");
        QVERIFY(shift);
        // A short screen settles the keyboard after the results above it reflow.
        QSize settled;
        QTRY_VERIFY(std::exchange(settled, shift->size()) == shift->size());
        const auto original = shift->grab().toImage();
        view->setDark(!dark);
        const auto inverted = shift->grab().toImage();
        QCOMPARE(inverted.pixelColor(5, 5), ui::themeColors(!dark).paper);
        QVERIFY(inverted != original);
        int ink = 0;
        for (int y = inverted.height() / 4; y < 3 * inverted.height() / 4; ++y)
            for (int x = inverted.width() / 4; x < 3 * inverted.width() / 4; ++x)
                if (inverted.pixelColor(x, y) == ui::themeColors(!dark).ink) ++ink;
        QVERIFY(ink > 4);
        view->setDark(dark);
        QCOMPARE(shift->grab().toImage(), original);
        const auto name = screenshotDevice(preset) +
                          ('-' + screenshotTheme(dark)) + "-widget-keyboard";
        QVERIFY(capture(fixture, name));
        shift->click();
        QVERIFY(capture(fixture, name + "-shift"));
        shift->click();
        QVERIFY(capture(fixture, name + "-caps-lock"));
        shift->click();
        {
            const QSize expected = librarySize(preset);
            QCOMPARE(view->size(), expected);
            QVERIFY(capture(fixture, name + "-300ppi", view));
            const QImage native(captureDirectory + '/' + name + "-300ppi.png");
            QCOMPARE(native.size(), screenshotSize(preset));
            QCOMPARE(native.dotsPerMeterX(), qRound(300 / 0.0254));
            QCOMPARE(native.dotsPerMeterY(), qRound(300 / 0.0254));
            const auto family = ui::chromeFont().family();
            QMap<QPushButton *, QString> originalStyles;
            for (auto *key : keyboard->findChildren<QPushButton *>()) {
                if (!key->isVisible() || !key->property("keyboard.character").isValid()) continue;
                originalStyles[key] = key->property("reader_style").toString();
                ui::readerStyle(key, QStringLiteral("QPushButton{font-family:\"%1\";font-size:%2px;border-radius:%3px;}")
                                        .arg(family).arg(ui::px(48)).arg(ui::px(12)));
                QCOMPARE(key->font().family(), family);
            }
            QVERIFY(capture(fixture, name + "-mono-300ppi", view));
            QVERIFY(QFontDatabase::families().contains(QStringLiteral("Libron")));
            for (auto it = originalStyles.begin(); it != originalStyles.end(); ++it) {
                ui::readerStyle(it.key(), QStringLiteral("QPushButton{font-family:\"Libron\";font-size:%1px;border-radius:%2px;}")
                                             .arg(ui::px(48)).arg(ui::px(12)));
                QCOMPARE(QFontInfo(it.key()->font()).family(), QString("Libron"));
            }
            QVERIFY(capture(fixture, name + "-libron-300ppi", view));
            for (auto it = originalStyles.begin(); it != originalStyles.end(); ++it)
                ui::readerStyle(it.key(), it.value());
        }
        // The search caret blinks, so these captures could show it or not. A
        // zero flash time keeps it drawn; restore the setting for later scenes.
        auto *hints = QGuiApplication::styleHints();
        const int flashTime = hints->cursorFlashTime();
        hints->setCursorFlashTime(0);
        const auto restoreFlash = qScopeGuard([hints, flashTime] { hints->setCursorFlashTime(flashTime); });
        auto *letter = view->findChild<QPushButton *>("widgetKey2_0");
        QVERIFY(letter);
        QTest::mousePress(letter, Qt::LeftButton);
        QTRY_VERIFY(view->findChild<QWidget *>("widgetKeyboardAccents"));
        QTest::mouseRelease(letter, Qt::LeftButton);
        QVERIFY(capture(fixture, name + "-accents"));
        auto *symbols = view->findChild<QPushButton *>("widgetKeyboardSymbols");
        QVERIFY(symbols);
        symbols->click();
        QVERIFY(capture(fixture, name + "-symbols"));
        const auto submitText = keyboard->findChild<QPushButton *>("widgetKeyboardDone")->text();
        keyboard->hide();
        for (const auto &locale : {QStringLiteral("de_DE"), QStringLiteral("fr_FR")}) {
            QLocale::setDefault(QLocale(locale));
            auto regional = ui::makeWidgetKeyboard(view, editor);
            regional->setSubmitText(submitText);
            QVERIFY(regional->show());
            QVERIFY(capture(fixture, name + (locale == "de_DE" ? "-qwertz" : "-azerty")));
        }
    }
    void lightGestureFeedback_data() { screenshotVariants(); }
    void lightGestureFeedback() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        auto light = std::make_shared<GestureLightState>();
        Fixture fixture(dark, false, false, true, false, false, [light](ReaderHostServices &services) {
            services.lightAvailable = [] { return true; };
            services.light = [light](QWidget *) { return std::make_unique<GestureLight>(light); };
        });
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotVariant(preset, dark);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsControls"));
        QVERIFY(revealSettingsOption(fixture, "rrLightGestureFeedback"));
        QVERIFY(capture(fixture, name + "-settings-light-feedback"));
        QVERIFY(fixture.press("rrLightGestureFeedback"));
        QVERIFY(capture(fixture, name + "-settings-light-feedback-hidden"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        if (fixture.visible("rrControlsTop")) QVERIFY(fixture.press("rrPage"));
        auto *page = static_cast<ReadingPage *>(fixture.widget("rrPage"));
        QVERIFY(page->lightStarted(false));
        page->lightChanged(.2);
        QVERIFY(!fixture.visible("rrBrightnessNotice"));
        QVERIFY(capture(fixture, name + "-light-feedback-hidden"));
        page->lightFinished(true);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsControls"));
        QVERIFY(revealSettingsOption(fixture, "rrLightGestureFeedback"));
        QVERIFY(fixture.press("rrLightGestureFeedback"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        if (fixture.visible("rrControlsTop")) QVERIFY(fixture.press("rrPage"));
        for (bool warmth : {false, true}) {
            QVERIFY(page->lightStarted(warmth));
            page->lightChanged(.2);
            QTRY_VERIFY(fixture.visible("rrBrightnessNotice"));
            QVERIFY(capture(fixture, name + (warmth ? "-warmth-gesture-feedback" : "-brightness-gesture-feedback")));
            page->lightFinished(false);
        }
    }
    void dictionarySettings_data() { screenshotVariants(); }
    void dictionarySettings() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark, false, false, true, false, true);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotVariant(preset, dark);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDictionaries"));
        auto *dictionaries = dynamic_cast<reader::DictionariesPage *>(fixture.widget("rrDictionaries"));
        QVERIFY(dictionaries);
        dictionaries->setStorage({}, 32000000000LL);
        QTRY_VERIFY(fixture.visible("rrDictionaryRow0"));
        QCoreApplication::processEvents();
        QVERIFY(capture(fixture, name + "-dictionaries"));
        for (auto *label : dictionaries->findChildren<QLabel *>(QRegularExpression("^rrDictionaryName[0-9]+$"))) {
            if (!label->isVisible()) continue;
            QVERIFY2(label->height() >= ui::wrappedInterfaceTextHeight(label->text(), label->font(), label->width()), qPrintable(label->text()));
            QVERIFY(label->parentWidget()->rect().contains(label->geometry()));
            auto metadataName = label->objectName();
            metadataName.replace("Name", "Metadata");
            auto *metadata = label->parentWidget()->findChild<QLabel *>(metadataName);
            QVERIFY(metadata);
            QVERIFY(metadata->y() > label->geometry().bottom());
            QVERIFY(metadata->y() - label->geometry().bottom() <= ui::px(8));
            const int groupHeight = metadata->geometry().bottom() + 1 - label->y();
            QCOMPARE(label->y(), (label->parentWidget()->height() - groupHeight) / 2);
            auto *action = label->parentWidget()->findChildren<QPushButton *>(QRegularExpression("^rrDictionaryAction[0-9]+$")).value(0);
            QVERIFY(action);
            QVERIFY2(label->geometry().right() < action->geometry().left(), qPrintable(QString("%1: label right %2, action left %3, row width %4").arg(label->text()).arg(label->geometry().right()).arg(action->geometry().left()).arg(label->parentWidget()->width())));
            QVERIFY(action->width() >= ui::px(60) && action->height() >= ui::px(60));
        }
        QVERIFY(fixture.press("rrSettingsSystem"));
        QVERIFY(revealSettingsOption(fixture, "rrDictionaryFontSize"));
        QVERIFY(capture(fixture, name + "-dictionary-ui"));
        QVERIFY(fixture.press("rrDictionaryFontSize"));
        QTRY_VERIFY(fixture.visible("rrDictionaryFontSizePopup"));
        QVERIFY(capture(fixture, name + "-dictionary-font-sizes"));
    }
    void loadingScreens_data() { screenshotVariants(); }
    void loadingScreens() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotVariant(preset, dark);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        auto *reader = fixture.window.readerWindow();
        auto *start = fixture.window.findChild<android::StartScreen *>();
        auto *view = start->openingCanvas()->content->findChild<QDialog *>("rrOpening");
        QVERIFY(view);
        // Keep the production opening, including the cover delivered by its worker.
        // Reparent it onto the visible reader so the gallery can capture it.
        view->setParent(reader, Qt::Widget);
        view->setGeometry(reader->rect());
        view->show();
        view->raise();
        QVERIFY(!view->findChild<QLabel *>("rrOpeningBrand")->isVisible());
        auto *progress = view->findChild<QProgressBar *>("rrOpeningProgress");
        progress->setValue(45);
        if (phonePreset(preset)) {
            auto *label = view->findChild<QLabel *>("rrOpeningLabel");
            QVERIFY(label->font().pixelSize() >= int(ui::px(32)));
            QVERIFY(label->height() >= ui::interfaceLineHeight(label->font()));
        }
        QVERIFY(capture(fixture, name + "-loading-cover", view));
        auto *hint = view->findChild<QLabel *>("rrOpeningHint");
        QTRY_VERIFY_WITH_TIMEOUT(hint->isVisible(), 5000);
        if (phonePreset(preset)) QVERIFY(hint->font().pixelSize() >= int(ui::px(22)));
        QVERIFY(capture(fixture, name + "-loading-cover-slow", view));
        view->hide();
        auto baseline = createEmbeddedOpening(reader, desktop::readerServices().presentation);
        baseline->window()->setGeometry(reader->rect());
        baseline->setCover({});
        baseline->setProgress(45);
        baseline->window()->show();
        baseline->window()->raise();
        QVERIFY(capture(fixture, name + "-loading-no-cover", baseline->window()));
    }
    void saving_data() { screenshotVariants(); }
    void saving() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotVariant(preset, dark);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *reader = fixture.window.readerWindow();
        QTRY_VERIFY(reader->readyForPresentation());
        const auto page = reader->grab().toImage();
        auto saving = std::make_unique<SavingView>(reader);
        saving->setGeometry(reader->rect());
        saving->setProgress(50);
        saving->show();
        saving->raise();
        QVERIFY(saving->isVisible());
        QVERIFY2(!saving->isWindow(), "Saving must be embedded in the reader");
        QVERIFY(!saving->graphicsProxyWidget());
        QVERIFY(saving->inputBarrier());
        QVERIFY(saving->card()->width() < reader->width());
        QVERIFY(saving->card()->height() < reader->height() / 2);
        QVERIFY(saving->findChild<QLabel *>("rrSavingHint")->isVisible());
        QTest::mouseClick(saving.get(), Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
        QTest::keyClick(saving.get(), Qt::Key_Escape);
        QVERIFY(saving->isVisible());
        const auto composed = reader->grab().toImage();
        const auto card = saving->card()->geometry();
        QVERIFY(composed.copy(card) != page.copy(card));
        const QRect exposed(0, 0, reader->width(), card.top());
        QVERIFY(!exposed.isEmpty());
        QCOMPARE(composed.copy(exposed), page.copy(exposed));
        QVERIFY(capture(fixture, name + "-saving"));
        saving.reset();
        for (bool retried : {false, true}) {
            auto failure = std::make_unique<SaveErrorView>(reader, QString(), retried);
            failure->setGeometry(reader->rect());
            failure->show();
            failure->raise();
            QCOMPARE(failure->quitButton() != nullptr, retried);
            if (retried)
                QVERIFY(failure->quitButton()->height() >= failure->secondaryButton()->height());
            QVERIFY(failure->card()->height() < reader->height());
            QVERIFY(capture(fixture, name + (retried ? "-save-retry-failed" : "-save-failed")));
        }
    }

    void finish_data() { screenshotVariants(); }
    void finish() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotDevice(preset) + ('-' + screenshotTheme(dark));
        const auto files = test::publisherPackage("en", "Finish confirmation", {
            {"text.xhtml", test::publisherHtml("<h1>The end</h1><p>A short book for the finish confirmation.</p>")}},
            "<itemref idref=\"text.xhtml\"/>");
        QVERIFY(writeBook(fixture.directory.path(), "finish.epub", files));
        captureBook = {{"title", "Finish confirmation"}, {"fixture", "offline single-page EPUB"}};
        const auto resetBook = qScopeGuard([] { captureBook = {}; });
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(fixture.directory.path() + "/finish.epub");
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("pages").toInt(), 1);
        fixture.window.readerWindow()->pageButton(1);
        QTRY_VERIFY(fixture.visible("rrFinishBook"));
        auto *option = fixture.window.readerWindow()->findChild<QCheckBox *>("rrSuppressFinishPrompt");
        QVERIFY(option && option->isVisible() && !option->isChecked());
        const auto *dialog = static_cast<ui::ModalOverlay *>(fixture.window.readerWindow()->findChild<QWidget *>("rrFinishBook"));
        QVERIFY(dialog && dialog->card()->rect().contains(option->geometry()));
        QVERIFY(capture(fixture, name + "-finish-book"));
        QCOMPARE(option->height(), int(ui::metrics::chrome::actionHeight));
        const auto *primary = dialog->findChild<QPushButton *>("rrMarkFinished");
        const auto *secondary = dialog->findChild<QPushButton *>("rrStillReading");
        QCOMPARE(option->height(), primary->height());
        QCOMPARE(option->height(), secondary->height());
        QCOMPARE(secondary->geometry().bottom(), dialog->card()->height() - 3);
        const auto frame = dialog->card()->grab().toImage();
        QCOMPARE(frame.pixelColor(option->x() + 10, option->y()), ui::dividerColor(option));
        if (primary->y() == secondary->y())
            QCOMPARE(frame.pixelColor(primary->x(), primary->geometry().bottom()), ui::dividerColor(primary));
        QVERIFY(fixture.press("rrStillReading"));
    }
    void dictionary_data() { screenshotVariants(); }
    void dictionary() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotDevice(preset) +
            ('-' + screenshotTheme(dark)) + "-dictionary";
        captureBook = {{"title", "Type Tester with controlled dictionary replies"}, {"fixture", "simulated dictionary result and service failures"}};
        const auto resetBook = qScopeGuard([] { captureBook = {}; });
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        openPrologueBody(fixture);
        auto *panel = dynamic_cast<DictionaryPanel *>(fixture.widget("rrDictionary"));
        QVERIFY(panel);
        panel->setLanguage("en");
        panel->setChoices({{"fixture", "English fixture", "en", true}, {"alternate", "Alternate dictionary", "en", false}});
        DictionaryResult result;
        result.word = "type";
        result.dictionary = "English fixture";
        result.dictionaryId = "fixture";
        result.received = result.restored = true;
        result.html = "<p><b>type</b> A printed character, or a design used to print characters.</p>";
        panel->open("type", result, {100, 100, 50, 40});
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(capture(fixture, name + "-definition"));
        QVERIFY(fixture.press("rrDictionaryEdit"));
        QVERIFY(capture(fixture, name + "-edit"));
        QVERIFY(fixture.press("rrDictionaryClose"));
        QVERIFY(fixture.press("rrDictionarySelect"));
        QTRY_VERIFY(fixture.visible("rrDictionaryChoices"));
        QVERIFY(capture(fixture, name + "-choices"));
        // Reopening through the panel's public API resets transient choice state.
        result.html = QString("<p>A long definition uses the production dictionary pager and typography.</p>").repeated(100);
        panel->open("type", result, {100, 100, 50, 40});
        QVERIFY(capture(fixture, name + "-long-definition"));
        QVERIFY(fixture.press("rrDictionaryNext"));
        QVERIFY(capture(fixture, name + "-definition-page-2"));
        result.html.clear();
        panel->open("xyzzy", result, {100, 100, 50, 40});
        QVERIFY(capture(fixture, name + "-no-definition"));
        result.received = false;
        result.error = "Dictionary service unavailable";
        panel->open("type", result, {100, 100, 50, 40});
        QVERIFY(capture(fixture, name + "-unavailable"));
        result.error.clear();
        result.received = result.restored = true;
        result.html = "<p><b>Trevelyan</b> The family name of the author of this book.</p>";
        panel->open("Trevelyan", result, {100, 100, 50, 40});
        QVERIFY(capture(fixture, name + "-search-action"));
        QVERIFY(fixture.press("rrDictionarySearch"));
        auto *query = qobject_cast<QLineEdit *>(fixture.widget("rrSearchQuery"));
        auto *status = qobject_cast<QLabel *>(fixture.widget("rrSearchStatus"));
        QVERIFY(query && status);
        QCOMPARE(query->text(), QString("Trevelyan"));
        QTRY_VERIFY(fixture.visible("rrSearchResult0"));
        QTRY_VERIFY(!status->text().contains("Searching", Qt::CaseInsensitive));
        QVERIFY(capture(fixture, name + "-search-results"));
    }

    void international_data() {
        QTest::addColumn<QJsonObject>("entry");
        QTest::addColumn<int>("preset");
        QTest::addColumn<bool>("vertical");
        QTest::addColumn<bool>("dark");
        QFile catalog(QStringLiteral(KOBALT_TEST_CORPUS) + "/screenshot-catalog.json");
        QVERIFY(catalog.open(QIODevice::ReadOnly));
        const auto entries = QJsonDocument::fromJson(catalog.readAll()).array();
        QCOMPARE(entries.size(), 16);
        QSet<QString> languages;
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            const auto language = entry["language"].toString();
            QVERIFY(!languages.contains(language));
            languages.insert(language);
            for (const bool vertical : {false, true}) {
                if (vertical && !QStringList{"ja", "ko", "zh-Hans", "zh-Hant"}.contains(language)) continue;
                for (const int preset : {1, 2, 3, 4, 5}) {
                    const auto name = QString::number(preset) + '-' + language + (vertical ? "-vertical" : "");
                    for (const bool dark : {false, true})
                        QTest::newRow(qPrintable(name + (dark ? "-dark" : "-light"))) << entry << preset << vertical << dark;
                    if (!phonePreset(preset)) QTest::newRow(qPrintable(name + "-large")) << entry << preset << vertical << false;
                }
            }
        }
    }
    void international() {
        QFETCH(QJsonObject, entry);
        QFETCH(int, preset);
        QFETCH(bool, vertical);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto files = test::literaryBook(QStringLiteral(KOBALT_TEST_CORPUS), entry, vertical, captureBook);
        QVERIFY2(!files.isEmpty(), qPrintable("Literary fixture checksum or metadata failed: " + entry["language"].toString()));
        const auto resetBook = qScopeGuard([] { captureBook = {}; });
        QVERIFY(writeBook(fixture.directory.path(), "literature.epub", files));
        QFile epub(fixture.directory.path() + "/literature.epub");
        QVERIFY(epub.open(QIODevice::ReadOnly));
        captureBook["epub_sha256"] = QString::fromLatin1(QCryptographicHash::hash(epub.readAll(), QCryptographicHash::Sha256).toHex());
        QJsonArray chapters;
        for (const auto &value : captureBook["chapters"].toArray()) {
            auto chapter = value.toObject();
            chapter.remove("paragraphs");
            chapters.append(chapter);
        }
        captureBook["chapters"] = chapters;
        const auto name = screenshotVariant(preset, dark) + "-international-" +
            entry["language"].toString() + (vertical ? "-vertical" : "");
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(epub.fileName());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        QVERIFY(capture(fixture, name + "-reading"));
        QVERIFY(fixture.press("rrPage"));
        QTRY_VERIFY(fixture.visible("rrNavChapters"));
        QVERIFY(fixture.press("rrNavChapters"));
        QTRY_VERIFY(fixture.visible("rrContents"));
        QVERIFY(capture(fixture, name + "-chapters"));
        QVERIFY(fixture.press("rrChapter0"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        if (!fixture.visible("rrNavText")) QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.press("rrFont"));
        QTRY_VERIFY(fixture.visible("rrChoices"));
        // The final font page includes the language's supplemental family.
        auto *counter = qobject_cast<QLabel *>(fixture.widget("rrFontPagerPage"));
        QVERIFY(counter);
        QSet<QString> seen;
        for (int i = 0; i < 20; ++i) {
            const auto label = counter->text();
            QVERIFY(!seen.contains(label));
            seen.insert(label);
            auto *next = fixture.widget("rrFontPagerNext");
            if (!next || !next->isVisible() || !next->isEnabled()) break;
            QVERIFY(fixture.click(next));
            QCoreApplication::processEvents();
            if (counter->text().startsWith("1 ")) {
                QVERIFY(fixture.press("rrFontPagerPrevious"));
                break;
            }
        }
        QVERIFY(capture(fixture, name + "-fonts"));
    }

    void publications_data() {
        QTest::addColumn<QString>("publication");
        QTest::addColumn<int>("preset");
        QTest::addColumn<bool>("dark");
        for (const auto *publication : {"alice", "maths", "moby-dick"})
            for (const int preset : {1, 2, 3, 4, 5}) {
                const auto name = QString::number(preset) + '-' + QString(publication);
                for (const bool dark : {false, true})
                    QTest::newRow(qPrintable(name + (dark ? "-dark" : "-light"))) << QString(publication) << preset << dark;
                if (!phonePreset(preset)) QTest::newRow(qPrintable(name + "-large")) << QString(publication) << preset << false;
            }
    }
    void typography_data() { screenshotVariants(); }
    void paragraphSpacing_data() { screenshotVariants(); }
    void paragraphSpacing() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotVariant(preset, dark);
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(!fixture.widget("rrParagraphSpacingSlider"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsStyling"));
        QVERIFY(revealSettingsOption(fixture, "rrBeautifierSettings"));
        QCOMPARE(fixture.widget("rrBeautifierSettings")->font(), fixture.widget("rrWordSpacing")->font());
        QVERIFY(fixture.press("rrBeautifierSettings"));
        QTRY_VERIFY(fixture.visible("rrBeautifierPopup"));
        auto *normalizationModal = static_cast<ui::ModalOverlay *>(fixture.widget("rrBeautifierPopup"));
        QVERIFY(normalizationModal->rect().contains(normalizationModal->card()->geometry()));
        QVERIFY(!fixture.widget("rrBeautifierEnabled"));
        QVERIFY(!fixture.widget("rrBeautifierInitials")->property("checked").toBool());
        QVERIFY(fixture.widget("rrBeautifierInitials")->isEnabled());
        QVERIFY(!fixture.widget("rrSmartStylingSection"));
        QVERIFY(!fixture.widget("rrAvoidWastefulStyling"));
        QVERIFY(fixture.widget("rrNormalizationRecommended")->property("checked").toBool());
        QVERIFY(fixture.widget("rrNormalizationMargins")->property("checked").toBool());
        QVERIFY(fixture.widget("rrNormalizationChapters")->property("checked").toBool());
        QVERIFY(capture(fixture, name + "-beautifier-off"));
        QVERIFY(fixture.press("rrNormalizationFull"));
        QVERIFY(capture(fixture, name + "-beautifier-full"));
        for (const auto *rule : {"rrNormalizationMargins", "rrNormalizationChapters", "rrBeautifierInitials", "rrBeautifierSpacing", "rrBeautifierIndent"})
            QVERIFY(fixture.widget(rule)->property("checked").toBool());
        QVERIFY(fixture.press("rrNormalizationCustom"));
        QVERIFY(fixture.widget("rrBeautifierIndent")->property("checked").toBool());
        QVERIFY(fixture.press("rrNormalizationRecommended"));
        auto *initials = static_cast<ui::DescribedOption *>(fixture.widget("rrBeautifierInitials"));
        QVERIFY(initials->presentation().leadingIndicator);
        QVERIFY(initials->presentation().dividers.testFlag(Qt::BottomEdge));
        // Tap the description side of the row, well away from its left checkbox.
        // A short screen pages the card, so turn to the row before measuring it.
        QVERIFY(fixture.reveal(initials));
        QVERIFY(fixture.click(initials, QPoint(initials->width() * 3 / 4, initials->height() * 2 / 3)));
        QTRY_VERIFY(initials->isChecked());
        QVERIFY(fixture.widget("rrNormalizationCustom")->property("checked").toBool());
        QVERIFY(capture(fixture, name + "-beautifier-rules"));
        QVERIFY(fixture.press("rrBeautifierCancel"));
        QTRY_VERIFY(!fixture.widget("rrBeautifierPopup"));
        QVERIFY(fixture.press("rrBeautifierSettings"));
        QVERIFY(!fixture.widget("rrBeautifierInitials")->property("checked").toBool());
        QVERIFY(fixture.press("rrBeautifierInitials"));
        QVERIFY(fixture.press("rrBeautifierDone"));
        QTRY_VERIFY(!fixture.widget("rrBeautifierPopup"));
        QVERIFY(capture(fixture, name + "-beautifier-settings"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QTRY_VERIFY(fixture.state().value("beautifier_initials").toBool());
        QVERIFY(!fixture.state().value("beautifier_spacing").toBool());
        QVERIFY(fixture.state().value("normalize_page_margins").toBool());
        QVERIFY(fixture.state().value("reduce_chapter_spacing").toBool());
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsStyling"));
        QVERIFY(revealSettingsOption(fixture, "rrBeautifierSettings"));
        QVERIFY(fixture.press("rrBeautifierSettings"));
        QVERIFY(fixture.widget("rrBeautifierInitials")->property("checked").toBool());
        QVERIFY(!fixture.widget("rrBeautifierSpacing")->property("checked").toBool());
        QVERIFY(fixture.press("rrBeautifierInitials"));
        QVERIFY(fixture.press("rrBeautifierDone"));
        QTRY_VERIFY(!fixture.widget("rrBeautifierPopup"));
        while (fixture.widget("rrSettingsOptionsPrevious")->isEnabled())
            QVERIFY(fixture.press("rrSettingsOptionsPrevious"));
        QVERIFY(revealSettingsOption(fixture, "rrParagraphSpacing"));
        QCOMPARE(fixture.widget("rrParagraphSpacing")->font(), fixture.widget("rrWordSpacing")->font());
        QCOMPARE(fixture.widget("rrParagraphSpacing")->minimumHeight(), fixture.widget("rrWordSpacing")->minimumHeight());
        QCOMPARE(fixture.widget("rrParagraphSpacing")->maximumHeight(), fixture.widget("rrWordSpacing")->maximumHeight());
        for (const auto *name : {"rrWordSpacing", "rrJustificationTracking", "rrParagraphSpacing",
                                 "rrWidowsOrphans", "rrVerticalSpacing", "rrVerticalSpacingScope"}) {
            QVERIFY(revealSettingsOption(fixture, name));
            const auto *row = fixture.widget(name);
            const auto font = ui::interfaceFont(ui::metrics::chrome::descriptionText);
            const int textWidth = row->width() - ui::metrics::chrome::inset - ui::metrics::chrome::actionRight -
                                  ui::metrics::chrome::cellIcon - ui::px(20);
            // Descriptions are written to fit one line at the standard size.
            // Larger text wraps them, and the row grows to fit.
            if (largeTextVariant() || phonePreset(preset))
                QVERIFY2(row->height() >= row->heightForWidth(row->width()), name);
            else
                QVERIFY2(ui::wrappedInterfaceTextHeight(row->accessibleDescription(), font, textWidth) <
                             2 * ui::interfaceLineHeight(font), name);
        }
        QVERIFY(revealSettingsOption(fixture, "rrParagraphSpacing"));
        QVERIFY(capture(fixture, name + "-styling-paragraph-row"));
        QVERIFY(fixture.press("rrParagraphSpacing"));
        QTRY_VERIFY(fixture.visible("rrParagraphSpacingPopup"));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.widget("rrStylingPreview")->property("sample_ready").toBool(), 10000);
        auto *preview = fixture.widget("rrStylingPreview");
        QVERIFY(preview->geometry().bottom() < fixture.widget("rrParagraphControls")->geometry().top());
        const auto publisher = preview->grab().toImage();
        QCOMPARE(fixture.widget("rrParagraphSpacingSlider")->property("value").toInt(), -1);
        QVERIFY(!fixture.visible("rrParagraphSpacingSliderCurrentPrefix"));
        QVERIFY(!fixture.visible("rrParagraphIndentSliderCurrentPrefix"));
        QCOMPARE(qobject_cast<QLabel *>(fixture.widget("rrParagraphSpacingValue"))->text(),
                 strings::get("controls.currently") + " " + strings::get("reader.publisher_spacing"));
        QVERIFY(capture(fixture, name + "-styling-paragraph-publisher"));
        QCOMPARE(preview->property("paragraph_gap_count").toInt(), 2);
        QVERIFY(fixture.press("rrParagraphSpacingMore"));
        QTRY_VERIFY_WITH_TIMEOUT(preview->property("sample_ready").toBool(), 10000);
        QTRY_VERIFY(preview->grab().toImage() != publisher);
        QVERIFY(capture(fixture, name + "-styling-paragraph-custom"));
        QCOMPARE(preview->property("paragraph_gap_count").toInt(), 0);
        auto *gapSlider = static_cast<ui::SettingSlider *>(fixture.widget("rrParagraphSpacingSlider"));
        const auto selectGap = [&](int stop) {
            const int inset = ui::metrics::control::sliderInset;
            return fixture.click(gapSlider, QPoint(inset + (gapSlider->width() - 2 * inset) * stop / 5,
                (gapSlider->height() + ui::metrics::control::sliderLabelHeight) / 2));
        };
        QVERIFY(selectGap(3));
        QTRY_COMPARE(gapSlider->value(), 100);
        QTRY_VERIFY_WITH_TIMEOUT(preview->property("sample_ready").toBool(), 10000);
        QCOMPARE(preview->property("paragraph_gap_count").toInt(), 2);
        QVERIFY(capture(fixture, name + "-styling-paragraph-gaps"));
        QVERIFY(selectGap(1));
        QTRY_COMPARE(gapSlider->value(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(preview->property("sample_ready").toBool(), 10000);
        const auto spaced = preview->grab().toImage();
        QCOMPARE(fixture.widget("rrParagraphIndentSlider")->property("value").toInt(), -1);
        QVERIFY(fixture.press("rrParagraphIndentMore"));
        QTRY_VERIFY_WITH_TIMEOUT(preview->property("sample_ready").toBool(), 10000);
        QTRY_VERIFY(preview->grab().toImage() != spaced);
        QCOMPARE(fixture.widget("rrParagraphSpacingSlider")->property("value").toInt(), 0);
        QVERIFY(capture(fixture, name + "-styling-paragraph-indent"));
        QVERIFY(fixture.press("rrParagraphSpacingClose"));
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("paragraph_spacing").toInt(-2), -1);
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("paragraph_spacing").toInt(-2), 0);
        QCOMPARE(storage::readJson(fixture.paths.preferences()).value("paragraph_indent").toInt(-2), 0);
        QTRY_COMPARE(fixture.state().value("paragraph_spacing").toInt(-2), 0);
        const auto other = fixture.directory.path() + "/other-paragraph-book.epub";
        QVERIFY(QFile::copy(book, other));
        fixture.window.openBook(other);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QCOMPARE(fixture.state().value("paragraph_spacing").toInt(-2), 0);
        QCOMPARE(fixture.state().value("paragraph_indent").toInt(-2), 0);
    }
    void typography() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotDevice(preset) + ('-' + screenshotTheme(dark));
        QFile font(QStringLiteral(KOBALT_TEST_VARIABLE_FONT));
        QVERIFY(font.open(QIODevice::ReadOnly));
        const auto files = test::publisherPackage("en", "Embedded font family", {
            {"Amstelvar.ttf", font.readAll()},
            {"text.xhtml", "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Embedded family</title>"
                "<style>@font-face{font-family:Embedded;src:url(Amstelvar.ttf)}body{font-family:Embedded}</style></head>"
                "<body><h1>Embedded family</h1><p>Publisher font family text with a real embedded font.</p></body></html>"}},
                "<itemref idref=\"text.xhtml\"/>");
        QVERIFY(writeBook(fixture.directory.path(), "embedded.epub", files));
        captureBook = {{"title", "Embedded font family"}, {"fixture", "offline EPUB with pinned Amstelvar font"}};
        const auto resetBook = qScopeGuard([] { captureBook = {}; });
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(fixture.directory.path() + "/embedded.epub");
        QTRY_COMPARE(opened.count(), 1);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(capture(fixture, name + "-embedded-reading"));
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.press("rrFont"));
        QTRY_VERIFY(fixture.visible("rrChoices"));
        QVERIFY(capture(fixture, name + "-embedded-fonts"));
        fixture.window.openBook(book);
        QTRY_COMPARE(opened.count(), 2);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        captureBook = {{"title", "Trevelyan's Type Tester v1.2"}};
        openPrologueBody(fixture);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavText"));
        QVERIFY(fixture.press("rrLarger"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(capture(fixture, name + "-text-size-preview"));
        QVERIFY(fixture.press("rrHeavier"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(capture(fixture, name + "-text-weight-preview"));
        QVERIFY(fixture.press("rrLooser"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(capture(fixture, name + "-text-spacing-preview"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsDisplay"));
        QVERIFY(revealSettingsOption(fixture, "rrBionicReading"));
        QVERIFY(fixture.press("rrBionicReading"));
        QVERIFY(capture(fixture, name + "-bionic-setting"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        if (fixture.visible("rrControlsTop")) QVERIFY(fixture.press("rrPage"));
        QVERIFY(capture(fixture, name + "-bionic-reading"));
    }
    void publications() {
        QFETCH(QString, publication);
        QFETCH(int, preset);
        QFETCH(bool, dark);
        QFile references(QStringLiteral(KOBALT_SCREENSHOT_REFERENCES));
        QVERIFY(references.open(QIODevice::ReadOnly));
        captureBook = QJsonDocument::fromJson(references.readAll()).object()[publication].toObject();
        const auto resetBook = qScopeGuard([] { captureBook = {}; });
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto path = QStringLiteral(KOBALT_SCREENSHOT_BOOKS) + '/' + captureBook["filename"].toString();
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256).toHex()), captureBook["sha256"].toString());
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        const auto name = screenshotVariant(preset, dark) + '-' + (publication == "moby-dick" ? "stats-dense" : publication);
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrNavChapters"));
        QTRY_VERIFY(fixture.visible("rrChapter1"));
        QVERIFY(fixture.press("rrChapter1"));
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        QVERIFY(capture(fixture, name + "-reading"));
        if (publication == "moby-dick") {
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrSettings"));
            QVERIFY(fixture.press("rrSettingsDisplay"));
            QVERIFY(revealSettingsOption(fixture, "rrDenseGraph"));
            auto *dense = qobject_cast<QCheckBox *>(fixture.widget("rrDenseGraph"));
            QVERIFY(dense);
            if (!dense->isChecked()) QVERIFY(fixture.press("rrDenseGraph"));
            QVERIFY(dense->isChecked());
            QVERIFY(capture(fixture, name + "-setting"));
            QVERIFY(fixture.press("rrSettingsClose"));
            QVERIFY(fixture.press("rrNavStats"));
            QTRY_VERIFY(fixture.visible("rrChapterGraph"));
            auto *graph = dynamic_cast<ChapterGraph *>(fixture.widget("rrChapterGraph"));
            QVERIFY(graph);
            // Current Dense mode preserves a minimum bar width. This pinned
            // 145-chapter edition needs more pages at narrower viewports.
            QTRY_VERIFY(graph->graphPages() >= 2);
            QVERIFY(storage::readJson(fixture.paths.preferences()).value("dense_stats_chart").toBool());
            capturePages(fixture, name + "-graph", "rrGraph");
        } else if (publication == "maths") {
            // Search through the real panel for the pinned equation passage.
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrSearchOpen"));
            auto *query = qobject_cast<QLineEdit *>(fixture.widget("rrSearchQuery"));
            QVERIFY(query);
            query->setText("IN this chapter we go to work finding solutions");
            QVERIFY(fixture.press("rrSearchSubmit"));
            QTRY_VERIFY_WITH_TIMEOUT(fixture.visible("rrSearchResult0"), 30000);
            QVERIFY(fixture.press("rrSearchResult0"));
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            // Leave and return to clear the transient search-result highlight.
            const int origin = fixture.state().value("page").toInt();
            QVERIFY(fixture.turnPage(1));
            QTRY_COMPARE(fixture.state().value("page").toInt(), origin + 1);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            QVERIFY(fixture.turnPage(-1));
            QTRY_COMPARE(fixture.state().value("page").toInt(), origin);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            for (int i = 1; i <= 4; ++i) {
                QVERIFY(capture(fixture, name + "-equations-" + QString::number(i)));
                if (i < 4) QVERIFY(fixture.turnPage(1));
                QTRY_VERIFY(!fixture.state().value("busy").toBool());
            }
        }
    }
    void verticalJustification_data() { screenshotVariants(); }
    void verticalJustification() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotDevice(preset) + ('-' + screenshotTheme(dark));
        verticalJustificationTour(fixture, name, true);
    }

    void directLinks_data() { screenshotVariants(); }
    void directLinks() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto name = screenshotDevice(preset) + ('-' + screenshotTheme(dark));
        captureBook = {{"title", "Links and footnotes"}, {"fixture", "offline EPUB with internal, external and footnote links"}};
        const auto resetBook = qScopeGuard([] { captureBook = {}; });
        auto linkFiles = test::linkBook();
        linkFiles["OEBPS/first.xhtml"].replace("<body>",
            "<body><p>Adjacent notes: <sup><a epub:type='noteref' href='Notes/foot%20notes.xhtml#short'>1</a>"
            " <a epub:type='noteref' href='Notes/foot%20notes.xhtml#long'>2</a></sup>.</p>");
        QVERIFY(writeBook(fixture.directory.path(), "links.epub", linkFiles));

        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(fixture.directory.path() + "/links.epub");
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 10000);
        QTRY_VERIFY(!fixture.state().value("busy").toBool());
        QVERIFY(fixture.press("rrPage"));
        QVERIFY(fixture.press("rrScrubberLinks"));
        QVERIFY(capture(fixture, name + "-footnotes-tap-targets"));
        auto *canvas = fixture.window.findChild<QGraphicsView *>();
        auto *paper = fixture.widget("rrPage");
        QVERIFY(canvas && paper);
        for (const auto *label : {"1", "short note"}) {
            const auto local = fixture.linkPoint(fixture.directory.path() + "/links.epub", label);
            QVERIFY(local.x() >= 0);
            const auto at = canvas->mapFromScene(paper->mapTo(fixture.window.readerWindow(), local));
            QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, at);
            QVERIFY(capture(fixture, name + (QString(label) == "1" ? "-footnotes-circle-pressed" : "-links-rectangle-pressed")));
            QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, at);
            QTRY_VERIFY(fixture.visible("rrLinkPanel"));
            QVERIFY(fixture.press("rrLinkClose"));
            QTRY_VERIFY(!fixture.visible("rrLinkPanel"));
            QVERIFY(fixture.press("rrPage"));
            QVERIFY(fixture.press("rrScrubberLinks"));
        }
        QVERIFY(fixture.click(fixture.widget("rrPage"), QPoint(2, 2)));
        for (const auto *mode : {"footnotes", "all"}) {
            QVERIFY(fixture.press("rrPage"));
            QTRY_VERIFY(fixture.visible("rrSettings"));
            QVERIFY(fixture.press("rrSettings"));
            QVERIFY(fixture.press("rrSettingsControls"));
            if (QString(mode) == "footnotes")
                QVERIFY(capture(fixture, name + "-tappable-links-settings"));
            QVERIFY(fixture.press("rrTappableLinks"));
            QTRY_VERIFY(fixture.visible("rrTappableLinks_off"));
            if (QString(mode) == "footnotes")
                QVERIFY(capture(fixture, name + "-tappable-links-choices"));
            QVERIFY(fixture.press(qPrintable(QString("rrTappableLinks_") + mode)));
            QVERIFY(fixture.press("rrSettingsClose"));
            QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
            if (fixture.visible("rrControlsTop"))
                QVERIFY(fixture.press("rrPage"));
            QVERIFY(capture(fixture, name + "-tappable-links-" + mode));
        }
        const auto path = fixture.directory.path() + "/links.epub";
        auto *page = fixture.widget("rrPage");
        QVERIFY(page);
        const auto note = fixture.linkPoint(path, "short note");
        QVERIFY(note.x() >= 0);
        QVERIFY(fixture.click(page, note));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
        QVERIFY(capture(fixture, name + "-footnotes-preview"));
        QVERIFY(fixture.press("rrLinkClose"));
        QTRY_VERIFY(!fixture.visible("rrLinkPanel"));
        const auto longNote = fixture.linkPoint(path, "long note");
        QVERIFY(longNote.x() >= 0);
        QVERIFY(fixture.click(page, longNote));
        QTRY_VERIFY(fixture.visible("rrLinkPanel"));
        QVERIFY(capture(fixture, name + "-footnotes-long-preview"));
        QVERIFY(fixture.press("rrLinkNext"));
        QVERIFY(capture(fixture, name + "-footnotes-page-2"));
        QVERIFY(fixture.press("rrLinkClose"));
        // A short screen can push the website link to the next page.
        auto website = fixture.linkPoint(path, "website");
        for (int turn = 0; turn < 3 && website.x() < 0; ++turn) {
            fixture.window.readerWindow()->pageButton(1);
            QTRY_VERIFY(!fixture.state().value("busy").toBool());
            website = fixture.linkPoint(path, "website");
        }
        QVERIFY(website.x() >= 0);
        QVERIFY(fixture.click(page, website));
        QTRY_VERIFY(fixture.visible("rrBrowserConfirmation"));
        QVERIFY(capture(fixture, name + "-links-website"));
    }
    void screens_data() { screenshotVariants(); }
    void screens() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        Fixture fixture(dark, false, false, true, true);
        QVERIFY(fixture.prepared);
        fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
        const auto device = screenshotDevice(preset);
        const auto name = device + ('-' + screenshotTheme(dark));
        // Onboarding is part of the Library and follows both themes.
        auto *start = fixture.window.findChild<android::StartScreen *>();
        QTRY_VERIFY(start && start->library()->size() == librarySize(preset));
        QVERIFY(capture(fixture, name + "-library-empty"));
        QSignalSpy opened(&fixture.window, &desktop::Window::bookOpened);
        fixture.window.openBook(book);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.state().value("busy").toBool(), 30000);
        auto *reader = fixture.window.readerWindow();
        QVERIFY(reader);
        QCOMPARE(reader->property("reader_dark").toBool(), dark);
        QVERIFY(hasText(reader->findChild<QWidget *>("rrPage")->grab().toImage()));
        QVERIFY(capture(fixture, name + "-cover"));
        openPrologueBody(fixture);
        QVERIFY(capture(fixture, name + "-reading"));
        tour(fixture, name, true);
        reader = fixture.window.readerWindow();
        QVERIFY(reader);
        QVERIFY(fixture.press("rrBookmarkListClose"));
        QTRY_VERIFY(!fixture.visible("rrBookmarkList"));
        QTRY_VERIFY(fixture.visible("rrSettings"));
        QVERIFY(fixture.press("rrSettings"));
        QVERIFY(fixture.press("rrSettingsKosync"));
        QVERIFY(fixture.press("rrKosyncPassword"));
        QTRY_VERIFY(fixture.visible("rrKosyncInput"));
        auto *input = reader->findChild<QLineEdit *>("rrKosyncInput");
        QVERIFY(input);
        input->setText("sample-password");
        QVERIFY(capture(fixture, name + "-kosync-password-masked"));
        QVERIFY(fixture.press("rrKosyncPasswordReveal"));
        QCOMPARE(input->echoMode(), QLineEdit::Normal);
        QVERIFY(capture(fixture, name + "-kosync-password-shown"));
        QVERIFY(fixture.press("rrKosyncEditCancel"));
        QVERIFY(fixture.press("rrSettingsClose"));
        QTRY_VERIFY(!fixture.visible("rrGeneralSettings"));
        captureReturnPressed(fixture, name);
    }
    void library_data() { screenshotVariants(); }
    void library() {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        const auto device = screenshotDevice(preset);
        const auto name = device + ('-' + screenshotTheme(dark)) + "-library";
        captureBook = {{"title", "Library fixtures"}, {"fixture", "invented books, authors and generated covers"}};
        const auto resetBook = qScopeGuard([] { captureBook = {}; });
        {
            Fixture fixture(dark);
            QVERIFY(fixture.prepared);
            fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
            auto *start = fixture.window.findChild<android::StartScreen *>();
            auto *view = start->library();
            auto *canvas = start->libraryCanvas();
            // The portrait phone's Library starts below its camera band.
            QTRY_VERIFY(view->isVisible() && view->geometry().bottomRight() + QPoint(1, 1) == canvas->sceneRect().bottomRight().toPoint() &&
                        view->width() > 600);
            view->findChild<QPushButton *>("libraryMore")->click();
            view->findChild<QPushButton *>("libraryMoreFolders")->click();
            QTRY_VERIFY(view->findChild<QWidget *>("libraryFolders")->isVisible());
            QVERIFY(capture(fixture, name + "-folders-empty"));
        }
        for (const bool reading : {true, false}) {
            Fixture fixture(dark, false, true, reading);
            QVERIFY(fixture.prepared);
            fixture.window.findChild<QComboBox *>("desktopScreenPicker")->setCurrentIndex(preset);
            auto *start = fixture.window.findChild<android::StartScreen *>();
            QVERIFY(start);
            start->showLibrary();
            auto *view = start->library();
            // The preset applies on a short timer; wait until the library has the reader's size.
            auto *canvas = start->libraryCanvas();
            // The portrait phone's Library starts below its camera band.
            QTRY_VERIFY(view->isVisible() && view->geometry().bottomRight() + QPoint(1, 1) == canvas->sceneRect().bottomRight().toPoint() &&
                        view->width() > 600);
            // The library opens as a cover grid.
            QVERIFY(capture(fixture, name + (reading ? "-grid" : "-grid-new")));
            const auto press = [view](const char *control) {
                auto *button = view->findChild<QAbstractButton *>(control);
                QTRY_VERIFY2(button && button->isVisible() && button->isEnabled(), control);
                button->click();
            };
            const auto bookMenu = [view] {
                auto *pages = view->findChild<QWidget *>("libraryPages");
                const QPoint point(pages->width() / 8, pages->height() / 8);
                QContextMenuEvent menu(QContextMenuEvent::Mouse, point, pages->mapToGlobal(point));
                QApplication::sendEvent(pages, &menu);
                QVERIFY(view->findChild<QWidget *>("libraryBookMenu"));
            };
            if (reading) {
                // A pressed cover gets its frame. Releasing on paper opens nothing.
                auto *pages = view->findChild<QWidget *>("libraryPages");
                QVERIFY(pages);
                const QPoint book(pages->width() / 8, pages->height() / 8);
                QTest::mousePress(pages, Qt::LeftButton, Qt::NoModifier, book);
                QVERIFY(capture(fixture, name + "-pressed"));
                QTest::mouseRelease(pages, Qt::LeftButton, Qt::NoModifier, QPoint(pages->width() - 2, pages->height() - 2));
                // The book's menu, opened at once with a secondary click rather than a timed hold.
                QContextMenuEvent menu(QContextMenuEvent::Mouse, book, pages->mapToGlobal(book));
                QApplication::sendEvent(pages, &menu);
                QVERIFY(view->findChild<QWidget *>("libraryBookMenu"));
                QVERIFY(capture(fixture, name + "-book-menu"));
                press("libraryBookMenuRemove");
                QTRY_VERIFY(view->findChild<QPushButton *>("libraryRemoveCancel")->isVisible());
                QVERIFY(capture(fixture, name + "-remove-confirmation"));
                press("libraryRemoveCancel");
            } else {
                bookMenu();
                QVERIFY(capture(fixture, name + "-new-book-menu"));
                QVERIFY(view->dismiss());
            }
            // Home shows only the books in progress, or a note when there are none.
            press("libraryViewHome");
            QVERIFY(capture(fixture, name + (reading ? "-home" : "-home-empty")));
            if (!reading)
                continue;
            bookMenu();
            QVERIFY(capture(fixture, name + "-reading-book-menu"));
            QVERIFY(view->dismiss());
            press("libraryViewList");
            QVERIFY(capture(fixture, name + "-list"));
            press("libraryNext");
            QVERIFY(capture(fixture, name + "-list-next"));
            press("libraryPrevious");
            press("librarySearch");
            auto *query = view->findChild<QLineEdit *>("librarySearchQuery");
            QVERIFY(query && query->isVisible());
            query->setText("Neralis");
            QVERIFY(capture(fixture, name + "-search-results"));
            query->setText("No matching invented book");
            QVERIFY(capture(fixture, name + "-search-empty"));
            press("libraryCloseSearch");
            press("librarySort");
            QVERIFY(capture(fixture, name + "-sort"));
            auto *bySeries = view->findChild<QAbstractButton *>("librarySortSeries");
            QVERIFY(bySeries);
            bySeries->click();
            QVERIFY(capture(fixture, name + "-list-series"));
            press("libraryViewGrid");
            press("librarySort");
            QVERIFY(capture(fixture, name + "-grid-sort"));
            press("libraryCoverSmall");
            QVERIFY(capture(fixture, name + "-grid-small"));
            press("librarySort");
            press("libraryCoverLarge");
            QVERIFY(capture(fixture, name + "-grid-large"));
            press("libraryPrevious"); // The pager wraps from the first page to the last.
            QVERIFY(capture(fixture, name + "-grid-last"));
            press("libraryMore");
            QVERIFY(capture(fixture, name + "-more"));
            press("libraryMoreFolders");
            QTRY_VERIFY(view->findChild<QWidget *>("libraryFolders")->isVisible());
            QVERIFY(capture(fixture, name + "-folders"));
            press("libraryFoldersBack");
            view->setScanning(true, 17);
            auto *progress = dynamic_cast<ui::LoadingIndicator *>(view->findChild<QWidget *>("libraryScanProgress"));
            QVERIFY(progress);
            // At 150% on the six-inch, the caption takes the squares' place.
            QTRY_VERIFY(progress->isRunning());
            QCOMPARE(progress->isVisible(), !(preset == 3 || (largeTextVariant() && preset == 1)));
            // Capture the same revealed animation frame on every run.
            auto *timer = progress->findChild<QTimer *>();
            QVERIFY(timer);
            progress->start(0);
            timer->stop();
            QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
            timer->stop();
            QVERIFY(capture(fixture, name + "-scanning"));
            view->setScanning(false, 17);
            start->showLibraryError("This EPUB could not be opened. Choose another book or try again.");
            QTRY_VERIFY(view->findChild<QPushButton *>("libraryNoticeOk")->isVisible());
            QVERIFY(capture(fixture, name + "-opening-error"));
        }
    }
};

#include "desktop_perf.h"

// The test inspects saved data between events; the reader's own work runs
// inside event delivery, where the main-thread guard checks it. The test
// runner calls checkEventsOnly(true) where the test body starts.
class GuardedApplication final : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject *object, QEvent *event) override {
        mainthread::EventDelivery delivery;
        return QApplication::notify(object, event);
    }
};

int main(int argc, char **argv) {
    // --watch is the one visible mode, for a person who asked to watch the
    // performance scenarios. Every other mode stays headless.
    const bool watching = argc == 2 && qstrcmp(argv[1], "--watch") == 0;
    if (!watching)
        qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts:freetype");
    FontCopies fontCopies;
    copies = &fontCopies;
    GuardedApplication app(argc, argv);
    if (QGuiApplication::platformName() != "minimal" && !watching) {
        qCritical() << "This executable only runs headlessly.";
        return 2;
    }
    app.setApplicationName("Kobalt headless tests");
    app.setStyle("Fusion");
    app.setQuitOnLastWindowClosed(false);
    if (app.arguments().value(1) == "--gallery") {
        if (app.arguments().size() != 3 || !QDir::isAbsolutePath(app.arguments().at(2))) {
            qCritical() << "Usage: kobalt-desktop-tests --gallery ABSOLUTE_DIRECTORY";
            return 2;
        }
        const auto directory = QDir::cleanPath(app.arguments().at(2));
        QFile record(directory + "/manifest.json");
        if (!record.open(QIODevice::ReadOnly)) return 2;
        const auto manifest = QJsonDocument::fromJson(record.readAll()).object();
        if (manifest["generator"] != "kobalt-desktop" || manifest["images"].toArray().isEmpty()) return 2;
        const auto html = test::screenshotGallery(directory, manifest["images"].toArray(), manifest);
        QSaveFile page(directory + "/index.html");
        if (html.isEmpty() || !page.open(QIODevice::WriteOnly) || page.write(html) != html.size() || !page.commit()) return 2;
        qInfo().noquote() << "Updated gallery from existing captures:" << directory;
        return 0;
    }
    QTemporaryDir fixtureDirectory(QDir::tempPath() + "/kobalt-book-XXXXXX");
    AssetBundle bundle(QStringLiteral(KOBALT_TEST_ASSETS), QStringLiteral(KOBALT_VERSION));
    assets = &bundle;
    if (!fixtureDirectory.isValid() || !bundle.available() ||
        !ui::initializeInterfaceFonts(bundle)) {
        qCritical() << "Cannot prepare the test book or runtime assets.";
        return 2;
    }
    for (const auto &path : bundledFontFiles(bundle)) {
        if (QFontDatabase::addApplicationFont(path) < 0) {
            qCritical() << "Cannot register bundled font:" << path;
            return 2;
        }
    }
    readingPpi.store(300);
    ui::setDensity(300);
    app.setFont(ui::interfaceFont());
    auto arguments = app.arguments();
    if (watching) {
        QTimer::singleShot(0, &app, [&] { app.exit(perf::watch()); });
        return app.exec();
    }
    if (arguments.value(1) == "--perf") {
        if (arguments.size() != 3 || !QDir::isAbsolutePath(arguments.at(2))) {
            qCritical() << "Usage: kobalt-desktop-tests --perf ABSOLUTE_FILE";
            return 2;
        }
        // The event loop's end stops the engine worker before the process exits.
        QTimer::singleShot(0, &app, [&] { app.exit(perf::run(arguments.at(2))); });
        return app.exec();
    }
    if (arguments.size() == 3 && arguments.at(1) == "--regression") {
        // Share the offline EPUB fixture with the restored standalone checks.
        const auto name = QFileInfo(arguments.at(2)).fileName();
        const bool lookup = name == "kobalt-search-tests" || name == "kobalt-word-selection-tests";
        const bool links = name == "kobalt-links-tests";
        QMap<QString, test::BookFiles> engineBooks;
        QStringList inputs{fixtureDirectory.path() + "/text.epub"};
        if (name == "kobalt-document-cache-tests") {
            engineBooks = {{"text.epub", test::engineBook()}};
        } else if (name == "kobalt-reader-tests") {
            engineBooks = {{"text.epub", test::engineBook()}, {"color.epub", test::engineBook(true)}};
            inputs.append(fixtureDirectory.path() + "/color.epub");
        } else if (name == "kobalt-local-reader-tests") {
            engineBooks = {{"text.epub", test::engineBook(false, 2, 300)}, {"color.epub", test::engineBook(true)}};
            inputs.append(fixtureDirectory.path() + "/color.epub");
        } else if (name == "kobalt-document-word-count-tests") {
            engineBooks = {{"text.epub", test::wordCountBook()}, {"word-count-many.epub", test::wordCountBook(1500)}};
            inputs.append(fixtureDirectory.path() + "/word-count-many.epub");
        } else if (name == "kobalt-publisher-markup-tests") {
            QFile font(bundle.file("fonts/IBMPlexSans-Regular.ttf"));
            if (!font.open(QIODevice::ReadOnly))
                return 2;
            engineBooks = test::publisherBooks(font.readAll());
            inputs = {fixtureDirectory.path() + "/publisher.epub", fixtureDirectory.path() + "/publisher-de.epub",
                      fixtureDirectory.path() + "/publisher-ru.epub"};
        } else if (!(lookup || links ? writeBook(fixtureDirectory.path(), "text.epub", links ? test::linkBook() : test::lookupBook())
                                     : createBook(fixtureDirectory.path()))) {
            return 2;
        }
        for (auto it = engineBooks.cbegin(); it != engineBooks.cend(); ++it)
            if (it.value().isEmpty() || !writeBook(fixtureDirectory.path(), it.key(), it.value()))
                return 2;
        QProcess child;
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        if (QFileInfo(arguments.at(2)).fileName() == "kobalt-reader-session-tests") {
            for (const auto *name : {"formatting-unsupported-writing.epub", "later-images.epub", "publisher-non-linear.epub"})
                if (!createBook(fixtureDirectory.path(), name))
                    return 2;
            inputs.append(fixtureDirectory.path() + "/publisher-non-linear.epub");
        }
        child.start(arguments.at(2), inputs);
        if (!child.waitForFinished(90000)) {
            const auto error = child.errorString();
            child.kill();
            child.waitForFinished();
            qCritical() << "Regression process did not finish:" << error;
            return 2;
        }
        return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 2;
    }
    const bool worker = arguments.size() >= 4 && arguments.at(1) == "--screenshot-worker";
    const bool lightTests = arguments.size() == 2 && arguments.at(1) == "--light-tests";
    const bool lightGallery = arguments.size() == 4 && arguments.at(3) == "--light";
    const bool screenshots = arguments.size() >= 2 && arguments.at(1) == "--screenshots";
    QString output;
    QElapsedTimer galleryTimer;
    galleryTimer.start();
    std::unique_ptr<QTemporaryDir> staging;
    // KOBALT_GALLERY_SCENE names gallery cards, separated by commas. The last
    // full gallery's manifest tells which cases capture them.
    const bool caseGallery = !qEnvironmentVariableIsEmpty("KOBALT_GALLERY_CASE");
    const auto selectedScenes = qEnvironmentVariable("KOBALT_GALLERY_SCENE").split(',', Qt::SkipEmptyParts);
    QMap<QString, QStringList> selection;
    QSet<QString> selectedVariants;
    int expectedScreenshots = lightGallery ? 182 : 3455;
    if (screenshots) {
        if ((arguments.size() != 3 && !lightGallery) || !QDir::isAbsolutePath(arguments.at(2))) {
            qCritical() << "Usage: kobalt-desktop-tests --screenshots ABSOLUTE_DIRECTORY [--light]";
            return 2;
        }
        output = QDir::cleanPath(arguments.at(2));
        if (!selectedScenes.isEmpty()) {
            if (lightGallery) {
                qCritical() << "KOBALT_GALLERY_SCENE selects scenes from the full gallery";
                return 2;
            }
            QFile index(output + "/manifest.json");
            const auto images = index.open(QIODevice::ReadOnly)
                ? QJsonDocument::fromJson(index.readAll()).object().value("images").toArray() : QJsonArray{};
            QSet<QString> found;
            expectedScreenshots = 0;
            for (const auto &value : images) {
                const auto image = value.toObject();
                if (!selectedScenes.contains(image["screen"].toString())) continue;
                if (image["case"].toString().isEmpty()) {
                    qCritical().noquote() << "The full gallery in" << output << "has no case index. Run it once without KOBALT_GALLERY_SCENE.";
                    return 2;
                }
                auto &screens = selection[image["case"].toString()];
                const auto screen = "@" + image["screen"].toString();
                if (!screens.contains(screen)) screens.append(screen);
                selectedVariants.insert(image["device"].toString() + "/" + image["theme"].toString() + "/" + image["screen"].toString());
                found.insert(image["screen"].toString());
                ++expectedScreenshots;
            }
            for (const auto &scene : selectedScenes) {
                if (!found.contains(scene)) {
                    qCritical().noquote() << "No gallery scene" << scene << "in" << output + "/manifest.json";
                    return 2;
                }
            }
            output += "-scenes";
        }
        if (caseGallery) output += "-cases";
        if (QFileInfo::exists(output)) {
            QFile previous(output + "/manifest.json");
            if (!previous.open(QIODevice::ReadOnly) ||
                QJsonDocument::fromJson(previous.readAll()).object().value("generator") != "kobalt-desktop") {
                qCritical() << "Refusing to replace a directory not created by this screenshot generator:" << output;
                return 2;
            }
        }
        const auto parent = QFileInfo(output).absolutePath();
        if (!QDir().mkpath(parent)) {
            return 2;
        }
        // The CMake parent owns TMPDIR and can clean captures after a timeout.
        staging = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/kobalt-captures-XXXXXX");
        captureDirectory = staging->path() + "/images";
        if (!staging->isValid() || !QDir().mkpath(captureDirectory)) {
            return 2;
        }
        arguments = {arguments.first()};
    }
    DesktopTests tests;
    ScreenshotTests screens;
    if (worker) {
        const auto selectors = arguments.mid(3);
        captureDirectory = arguments.at(2);
        captureOnly = qEnvironmentVariable("KOBALT_GALLERY_CAPTURE").split(',', Qt::SkipEmptyParts);
        if (!QDir::isAbsolutePath(captureDirectory) || !QDir().mkpath(captureDirectory) ||
            QFileInfo::exists(captureDirectory + "/part.json")) return 2;
        QTimer::singleShot(0, &app, [&] {
            mainthread::checkEventsOnly(true);
            app.exit(QTest::qExec(&screens, QStringList{arguments.first()} + selectors));
        });
        const int result = app.exec();
        if (result || selectors == QStringList{"-datatags"}) return result;
        return writeCaptureRecord() ? 0 : 2;
    }
    if (screenshots) {
        if (!test::screenshotJobs(QCoreApplication::applicationFilePath(), staging->path(), captureDirectory, lightGallery,
                                  captures, selection)) return 2;
    } else {
        if (lightTests) arguments = {arguments.first(), "primaryViews"};
        QTimer::singleShot(0, &app, [&] {
            mainthread::checkEventsOnly(true);
            app.exit(QTest::qExec(lightTests ? static_cast<QObject *>(&screens) : &tests, arguments));
        });
        return app.exec();
    }
    QFile fixture(book);
    if (!selection.isEmpty()) {
        QSet<QString> capturedVariants;
        for (const auto &value : captures) {
            const auto image = value.toObject();
            if (!selectedScenes.contains(image["screen"].toString())) return 2;
            if (image["capture_page"].toInt() == 1)
                capturedVariants.insert(image["device"].toString() + "/" + image["theme"].toString() + "/" + image["screen"].toString());
        }
        if (capturedVariants != selectedVariants) {
            qCritical() << "Scene capture lost a selected screen or viewport variant";
            return 2;
        }
    } else if (!caseGallery && captures.size() != expectedScreenshots) {
        qCritical() << "Expected" << expectedScreenshots << "screenshots, captured" << captures.size();
        return 2;
    }
    if (!fixture.open(QIODevice::ReadOnly)) return 2;
    const QJsonObject manifest{{"schema", 1}, {"generator", "kobalt-desktop"}, {"qt", qVersion()}, {"version", KOBALT_VERSION},
                               {"commit", kobaltCommit}, {"modified", bool(KOBALT_COMMIT_MODIFIED)},
                               {"book", "Trevelyan's Type Tester v1.2 with chapter-heading CSS fix"}, {"book_file", QFileInfo(book).fileName()},
                               {"book_sha256", QString::fromLatin1(QCryptographicHash::hash(fixture.readAll(), QCryptographicHash::Sha256).toHex())},
                               {"suite", caseGallery ? "cases" : !selection.isEmpty() ? "scenes" : lightGallery ? "light" : "full"}, {"images", captures}};
    QSaveFile record(captureDirectory + "/manifest.json");
    const auto json = QJsonDocument(manifest).toJson();
    if (!record.open(QIODevice::WriteOnly) || record.write(json) != json.size() || !record.commit()) {
        return 2;
    }
    const auto html = test::screenshotGallery(captureDirectory, captures, manifest);
    if (html.isEmpty()) return 2;
    QSaveFile index(captureDirectory + "/index.html");
    if (!index.open(QIODevice::WriteOnly) || index.write(html) != html.size() || !index.commit()) {
        return 2;
    }
    // Keep the last complete set outside TMPDIR until replacement succeeds.
    const auto previous = QFileInfo(output).absolutePath() + '/' + QFileInfo(staging->path()).fileName() + "-previous";
    const bool existing = QFileInfo::exists(output);
    if (existing && !QDir().rename(output, previous)) {
        return 2;
    }
    if (!QDir().rename(captureDirectory, output)) {
        if (existing && !QDir().rename(previous, output)) {
            qCritical() << "Previous screenshots retained at" << previous;
        }
        return 2;
    }
    if (existing && !QDir(previous).removeRecursively())
        qWarning() << "Cannot remove superseded screenshots:" << previous;
    qInfo().noquote() << "Saved" << captures.size() << "screenshots and manifest.json to" << output << "in" << galleryTimer.elapsed() / 1000.0 << "seconds";
    return 0;
}

#include "desktop.moc"
