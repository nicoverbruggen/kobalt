#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/paged_text.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "reader_host.h"
#include "host_presentation.h"
#include "interfaces/reader/views/opening.h"
#include "interfaces/reader/panels/chapter_graph.h"
#include "interfaces/reader/panels/bookmark_list.h"
#include "interfaces/reader/panels/choice_screen.h"
#include "interfaces/reader/settings/acknowledgments.h"
#include "reader_inputs.h"
#include "host_fixture.h"
#include "host_style.h"
#include "host_platform.h"
#include "interfaces/reader/settings/settings_panel.h"
#include "interfaces/reader/panels/light_panel.h"
#include "platform/api/light_controls.h"
#include "interfaces/reader/panels/search_panel.h"
#include "interfaces/reader/settings/appearance_panel.h"
#include "interfaces/reader/views/navigation.h"
#include "interfaces/reader/panels/link_panel.h"
#include "interfaces/reader/views/reading_page.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "support/strings.h"
#include "host_services.h"
#include "host_progress.h"
#include "qt_test_shards.h"
#include "platform/nickel/common/engine_positions.h"
#include "reader_canvas.h"
#include "platform/android/canvas.h"
#include <QStackedLayout>
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "storage/reader_storage.h"
#include "storage/bookmark_store.h"
#include "reading/reading_density.h"
#include "engine/crengine/engine_activity.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/private_text.h"
#include <QApplication>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QFontInfo>
#include <QPushButton>
#include <QProgressBar>
#include <QSignalSpy>
#include <QTest>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QDir>
#include <QLineEdit>
#include <QWindow>
#include <QScreen>
#include <QScrollBar>
#include <QEventLoop>
#include <QJsonArray>
#include <QLabel>
#include <QAbstractSlider>
#include <QSlider>
#include <QStyle>
#include <QCryptographicHash>
#include <QContextMenuEvent>
#include <QTextDocument>
#include <QTemporaryDir>
#include <QPaintEvent>
#include <QThread>
#include <atomic>

using namespace kobalt;
namespace {
const QSize nativeSize(1264, 1680);
std::atomic_bool delayUsbEncoding{false}, usbEncodingReturned{false};
QString usbPosition(const ReaderModel &model, QString *error) {
    if (delayUsbEncoding) {
        QThread::msleep(2500);
        usbEncodingReturned = true;
    }
    return nickel::positionCodec().encode(model, error);
}
AssetBundle *testAssets = nullptr;
FontCopies *testFontCopies = nullptr;
EngineFontInputs testFonts;
QString testPlaceholder;
storage::Paths testPaths() {
    storage::Paths paths{qEnvironmentVariable("KOBALT_TEST_STATE", QStringLiteral(KOBALT_STORAGE_ROOT))};
    paths.bookPrefix = "book-";
    paths.startup = paths.directory + "/startup-session";
    paths.active = paths.directory + "/active-session.json";
    paths.stateReport = paths.directory + "/state.json";
    paths.sessionReport = paths.directory + "/session.json";
    return paths;
}
QString fixture() {
    return qEnvironmentVariable("KOBALT_TEST_EPUB", qEnvironmentVariable("KOBALT_FIXTURES") + "/text.epub");
}
QPushButton *sizeStep(test::ReaderHost &host) {
    auto *slider = host.window()->findChild<QSlider *>("rrTextSize");
    if (!slider)
        return nullptr;
    return host.window()->findChild<QPushButton *>(slider->value() == slider->maximum() ? "rrSmaller" : "rrLarger");
}
QJsonObject inspectReader(test::ReaderHost &host) {
    host.window()->checkpoint();
    return storage::readJson(testPaths().stateReport);
}
QJsonObject rectangle(const QRect &rect) {
    return {{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
}
bool capture(test::ReaderCanvas &canvas, test::ReaderHost &host, const QString &name) {
    const QImage rendered = canvas.capture();
    if (rendered.isNull())
        return false;
    QJsonArray widgets;
    auto all = canvas.surface()->findChildren<QWidget *>();
    all.prepend(canvas.surface());
    for (auto *widget : all) {
        if (widget->objectName().isEmpty())
            continue;
        const auto point = widget->mapTo(canvas.surface(), QPoint());
        QJsonObject data{{"name", widget->objectName()},
                         {"class", widget->metaObject()->className()},
                         {"visible", widget->isVisible()},
                         {"enabled", widget->isEnabled()},
                         {"x", point.x()},
                         {"y", point.y()},
                         {"width", widget->width()},
                         {"height", widget->height()},
                         {"font", widget->font().family()},
                         {"resolved_font", QFontInfo(widget->font()).family()}};
        data["local_rect"] = rectangle(widget->rect());
        data["surface_rect"] = rectangle(QRect(point, widget->size()));
        const QRegion region = widget->visibleRegion().translated(point);
        QJsonArray visibleRects;
        for (const QRect &rect : region)
            visibleRects.append(rectangle(rect));
        data["visible_region_rects"] = visibleRects;
        data["visible_region_bounds"] = rectangle(region.boundingRect());
        if (auto *button = qobject_cast<QAbstractButton *>(widget))
            data["text"] = button->text();
        if (auto *label = qobject_cast<QLabel *>(widget)) {
            data["text"] = label->text();
        }
        if (auto *slider = qobject_cast<QAbstractSlider *>(widget))
            data["value"] = slider->value();
        widgets.append(data);
    }
    QJsonObject metadata{
        {"width", nativeSize.width()},
        {"height", nativeSize.height()},
        {"capture_dpr", 1},
        {"desktop_dpr", canvas.devicePixelRatioF()},
        {"reader_ppi", readingPpi.load()},
        {"test_device_fixture", test::fixtureProvenance()},
        {"host_style_profile", test::hostStyleProfile},
        {"host_font_profile", test::hostFontProvenance()},
        {"application_style_sheet", qApp->styleSheet()},
        {"visible_region_coordinates", "reader surface, clipped to current host viewport and opaque siblings"},
        {"qt", qVersion()},
        {"platform_plugin", QGuiApplication::platformName()},
        {"state", host.session()->state()},
        {"widgets", widgets},
        {"source_fingerprint", QString::fromLatin1(host.session()->sourceHash())},
        {"font_files", QJsonArray::fromStringList(bundledFontFiles(*testAssets))},
        {"asset_root", testAssets->root()}};
    QFile source(fixture());
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!source.open(QIODevice::ReadOnly) || !digest.addData(&source))
        return false;
    metadata["book_sha256"] = QString::fromLatin1(digest.result().toHex());
    return rendered.save(name + ".png") && storage::writeJson(name + ".json", metadata);
}
struct CloseHost {
    test::ReaderHost &host;
    ~CloseHost() {
        if (host.idle())
            return;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&host, &test::ReaderHost::closed, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(10000);
        host.shutdown();
        if (!host.idle())
            loop.exec();
        if (!host.idle())
            qFatal("Native test failed to drain its reader");
    }
};
class PaintRegions final : public QObject {
public:
    QRegion painted;
    int count = 0;
    QStringList widgets;
    QWidget *coordinateRoot = nullptr;

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Paint) {
            ++count;
            const auto region = static_cast<QPaintEvent *>(event)->region();
            painted += coordinateRoot
                           ? region.translated(static_cast<QWidget *>(object)->mapTo(coordinateRoot, QPoint()))
                           : region;
            widgets.append(object->objectName());
        }
        return false;
    }
};
class PanelPlacement final : public QObject {
public:
    QRect shown;
    int visibleMoves = 0;

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        auto *widget = static_cast<QWidget *>(object);
        if (event->type() == QEvent::Show)
            shown = widget->geometry();
        if (event->type() == QEvent::Move && widget->isVisible())
            ++visibleMoves;
        return false;
    }
};
// Replaces a file with an empty directory of the same name until restored.
// Later tests share the state directory, so a failed restore stops the run.
class BlockedFile {
public:
    explicit BlockedFile(QString path) : path(std::move(path)), saved(this->path + ".blocked") {
        moved = QFile::rename(this->path, saved);
        created = moved && QDir().mkdir(this->path);
    }
    ~BlockedFile() {
        if (!restore())
            qFatal("Could not restore %s", qPrintable(path));
    }
    BlockedFile(const BlockedFile &) = delete;
    BlockedFile &operator=(const BlockedFile &) = delete;
    bool active() const {
        return created;
    }
    // Removes only the empty directory this object made, then puts the file back.
    bool restore() {
        if (created && !QDir().rmdir(path))
            return false;
        created = false;
        if (moved && !QFile::rename(saved, path))
            return false;
        moved = false;
        return true;
    }

private:
    QString path, saved;
    bool moved = false, created = false;
};
bool clickAt(test::ReaderCanvas &canvas, QPoint point) {
    canvas.ensureVisible(point.x(), point.y(), 30, 30);
    QCoreApplication::processEvents();
    const QPoint local = canvas.surface()->mapTo(canvas.viewport(), point);
    if (!canvas.viewport()->rect().contains(local))
        return false;
    QTest::mouseClick(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, canvas.surface()->mapTo(&canvas, point));
    return true;
}
bool click(test::ReaderCanvas &canvas, QWidget *widget) {
    if (!widget || !widget->isVisible() || !widget->isEnabled())
        return false;
    const QPoint point = widget->mapTo(canvas.surface(), widget->rect().center());
    canvas.ensureVisible(point.x(), point.y(), 30, 30);
    QCoreApplication::processEvents();
    auto *receiver = canvas.surface()->childAt(point);
    // A label inside the control is valid; an overlay or another control is not.
    if (receiver != widget && (!receiver || !widget->isAncestorOf(receiver)))
        return false;
    return clickAt(canvas, point);
}
int sliderX(const QSlider *slider, int value) {
    const int inset = ui::metrics::control::sliderInset;
    return inset + QStyle::sliderPositionFromValue(slider->minimum(), slider->maximum(), value,
                                                   qMax(1, slider->width() - 2 * inset));
}
// One continuous press, move and release through the window, as a finger
// drag arrives on the device. Events go through Qt's grab and enabled checks.
void drag(test::ReaderCanvas &canvas, QSlider *slider, int from, int to, const std::function<void()> &moved = {}) {
    const QPoint start = slider->mapTo(canvas.surface(), QPoint(sliderX(slider, from), slider->height() / 2));
    canvas.ensureVisible(start.x(), start.y(), 30, 30);
    QCoreApplication::processEvents();
    const auto at = [&](int value) {
        return canvas.surface()->mapTo(
            &canvas, slider->mapTo(canvas.surface(), QPoint(sliderX(slider, value), slider->height() / 2)));
    };
    QTest::mousePress(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at(from));
    QCoreApplication::processEvents();
    const int direction = to >= from ? 1 : -1;
    for (int value = from; value != to + direction; value += direction) {
        QTest::mouseMove(canvas.windowHandle(), at(value));
        QCoreApplication::processEvents();
        if (moved)
            moved();
    }
    QTest::mouseRelease(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at(to));
}
// The same gesture by track position, for sliders whose stops are evenly
// spaced rather than proportional to their values.
void dragAcross(test::ReaderCanvas &canvas, QSlider *slider, qreal from, qreal to,
                const std::function<void()> &moved = {}) {
    const int inset = ui::metrics::control::sliderInset;
    const auto at = [&](qreal fraction) {
        const QPoint local(inset + qRound(fraction * (slider->width() - 2 * inset)), slider->height() / 2);
        return canvas.surface()->mapTo(&canvas, slider->mapTo(canvas.surface(), local));
    };
    const QPoint start = slider->mapTo(canvas.surface(), QPoint(slider->width() / 2, slider->height() / 2));
    canvas.ensureVisible(start.x(), start.y(), slider->width() / 2 + 30, 30);
    QCoreApplication::processEvents();
    QTest::mousePress(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at(from));
    QCoreApplication::processEvents();
    for (int step = 1; step <= 10; ++step) {
        QTest::mouseMove(canvas.windowHandle(), at(from + (to - from) * step / 10));
        QCoreApplication::processEvents();
        if (moved)
            moved();
    }
    QTest::mouseRelease(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at(to));
}
bool captureSearchMetrics(QWidget *result, const QString &query) {
    for (auto *label : result->findChildren<QLabel *>()) {
        if (label->textFormat() != Qt::RichText)
            continue;
        const QFont font = label->font();
        const QFontMetrics metrics(font);
        QJsonArray documents;
        for (int percent : {100, 120, 130}) {
            QTextDocument document;
            document.setDocumentMargin(0);
            document.setDefaultFont(font);
            document.setTextWidth(label->width());
            const QString block = QString("<div style=\"line-height:%1%\">%2</div>").arg(percent);
            document.setHtml(block.arg("M<br>M"));
            const auto sampleHeight = document.size().height();
            document.setHtml(block.arg("<b><u>" + query.toHtmlEscaped() + "</u></b>"));
            documents.append(QJsonObject{{"line_height_percent", percent},
                                         {"two_line_height", sampleHeight},
                                         {"query_height", document.size().height()}});
        }
        return storage::writeJson("native-reader-search-metrics.json",
                                  {{"qt", qVersion()},
                                   {"platform_plugin", QGuiApplication::platformName()},
                                   {"query", query},
                                   {"row_height", result->height()},
                                   {"excerpt_width", label->width()},
                                   {"family", font.family()},
                                   {"resolved_family", QFontInfo(font).family()},
                                   {"pixel_size", font.pixelSize()},
                                   {"ascent", metrics.ascent()},
                                   {"descent", metrics.descent()},
                                   {"height", metrics.height()},
                                   {"leading", metrics.leading()},
                                   {"documents", documents}});
    }
    return false;
}
}
class NativeReaderTest : public QObject {
    Q_OBJECT
private slots:
    void rotationOwnership() {
        QList<int> calls;
        {
            ReaderOrientation orientation(
                nullptr,
                [&](Qt::ScreenOrientation value) {
                    calls.append(int(value));
                },
                [&](bool owned) {
                    calls.append(owned ? -1 : -2);
                });
            orientation.configure(QJsonObject{{"orientation", int(Qt::LandscapeOrientation)}});
            QVERIFY(calls.isEmpty());
            orientation.start(Qt::PortraitOrientation);
            QCOMPARE(calls, QList<int>({-1, int(Qt::LandscapeOrientation)}));
            orientation.start(Qt::PortraitOrientation);
            QCOMPARE(calls.size(), 2);
            orientation.stop();
            QCOMPARE(calls, QList<int>({-1, int(Qt::LandscapeOrientation), int(Qt::PortraitOrientation), -2}));
            orientation.stop();
            QCOMPARE(calls.size(), 4);
            orientation.start(Qt::PortraitOrientation);
        }
        QCOMPARE(calls.mid(4), QList<int>({-1, int(Qt::LandscapeOrientation), int(Qt::PortraitOrientation), -2}));
    }
    void initTestCase() {
        test::applyHostStyle();
        QVERIFY2(test::deviceFixture().isEmpty() || test::deviceFixture() == "libra-colour",
                 "Unknown test device fixture");
        const QString state = testPaths().directory;
        QVERIFY(state.endsWith("/test-state"));
        // This separately compiled runtime never shares the interactive root.
        QVERIFY(!QDir(state).exists() || QDir(state).removeRecursively());
        QVERIFY(QDir().mkpath(state));
        if (qEnvironmentVariableIsSet("KOBALT_TEST_SETTINGS")) {
            const auto preferences = storage::readJson(qEnvironmentVariable("KOBALT_TEST_SETTINGS"));
            QVERIFY(!preferences.isEmpty());
            QVERIFY(storage::writeJson(state + "/reader-settings.json", preferences));
        }
        QVERIFY(testAssets->available());
        QVERIFY(ui::initializeInterfaceFonts(*testAssets));
        testFonts = engineFontInputs(*testAssets, {}, {}, {});
        testPlaceholder = placeholderFontFamily(*testAssets, *testFontCopies);
        QVERIFY(!testPlaceholder.isEmpty());
        QVERIFY(!bundledFontFiles(*testAssets).isEmpty());
        for (const auto &file : bundledFontFiles(*testAssets))
            QVERIFY(QFontDatabase::addApplicationFont(file) >= 0);
        ui::setDensity(300);
        readingPpi.store(300);
        QApplication::setFont(ui::interfaceFont());
        QVERIFY(QFontInfo(ui::interfaceFont()).family().startsWith("Kobalt UI Sans"));
        QVERIFY(QFontInfo(ui::captionFont()).family().startsWith("Kobalt UI Mono"));
        QVERIFY(QFontInfo(ui::brandFont(30)).family().startsWith("Kobalt UI Serif"));
        QCOMPARE(test::selectedHostPlatform("offscreen", true), QByteArray("offscreen"));
#ifdef Q_OS_MACOS
        QCOMPARE(test::selectedHostPlatform({}, true), QByteArray("minimal:enable_fonts:freetype"));
        QCOMPARE(test::selectedHostPlatform({}, false), QByteArray("cocoa:fontengine=freetype"));
        if (test::platformRequest.contains("freetype")) {
            const auto profile = test::hostFontProvenance();
            QCOMPARE(profile["probe_family"].toString(), QString("Libron"));
            QCOMPARE(profile["probe_ascent"].toInt(), 26);
            QCOMPARE(profile["probe_descent"].toInt(), 6);
            QCOMPARE(profile["probe_height"].toInt(), 32);
        }
#endif
    }
    void sharedWordCountsReachStats_data() {
        QTest::addColumn<bool>("platformProgress");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("ppi");
        for (bool platform : {false, true}) {
            for (bool dark : {false, true}) {
                for (int ppi : {212, 300}) {
                    const auto name =
                        QString("%1-%2-%3").arg(platform ? "bridge" : "local").arg(dark ? "dark" : "light").arg(ppi);
                    QTest::newRow(qPrintable(name)) << platform << dark << ppi;
                }
            }
        }
    }
    void sharedWordCountsReachStats() {
        QFETCH(bool, platformProgress);
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const auto previousDensity = ui::density();
        const auto previousPpi = readingPpi.load();
        struct Restore {
            int density, ppi;
            ~Restore() {
                ui::setDensity(density);
                readingPpi.store(ppi);
            }
        } restore{previousDensity, previousPpi};
        ui::setDensity(ppi);
        readingPpi.store(ppi);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        QVERIFY(storage::writeJson(paths.preferences(), {{"dark_mode", dark}}));
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        if (platformProgress) {
            host.progress = std::make_shared<test::HostProgress>();
        }
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(qEnvironmentVariable("KOBALT_FIXTURES") + "/word-counts.epub", nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->totalWords(), qint64(40));
        const auto chapters = host.session()->chapterGraph();
        QCOMPARE(chapters.size(), 5);
        const qint64 expected[]{23, 0, 6, 0, 9};
        for (int i = 0; i < chapters.size(); ++i) {
            QCOMPARE(chapters[i].words, expected[i]);
        }
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavStats")));
        auto *detail = host.window()->findChild<QLabel *>("rrGraphBookDetail");
        QVERIFY(detail);
        QTRY_VERIFY(detail->isVisible());
        QCOMPARE(detail->text(), strings::get("stats.words").arg("40"));
        QVERIFY(capture(canvas, host, QString("shared-words-%1").arg(QTest::currentDataTag())));
        const auto anchor = host.session()->state().value("position");
        auto settings = host.session()->state();
        settings["font_pixels"] = 60;
        host.session()->apply(settings);
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy(), 30000);
        QCOMPARE(host.session()->totalWords(), qint64(40));
        QCOMPARE(host.session()->state().value("position"), anchor);
        QCOMPARE(host.session()->chapterGraph().last().words, qint64(9));
        QCOMPARE(detail->text(), strings::get("stats.words").arg("40"));
    }
    void navigationCompositionAndRelease() {
        QWidget owner;
        auto *toolbar = new ui::Panel(&owner, Qt::BottomEdge);
        auto *rows = new QVBoxLayout(toolbar);
        auto *exit = new QPushButton(toolbar);
        auto *settings = new ui::Button(toolbar), *bookmarks = new ui::Button(toolbar);
        settings->setPaintMode(ui::ButtonPaintMode::ToolbarSquare);
        bookmarks->setPaintMode(ui::ButtonPaintMode::OutlinedAction);
        auto *clock = new QLabel(toolbar), *session = new QLabel(toolbar), *battery = new QLabel(toolbar);
        rows->addWidget(exit);
        rows->addWidget(settings);
        rows->addWidget(bookmarks);
        settings->setStyleSheet("color:red;");
        bookmarks->setStyleSheet("color:blue;");
        session->setStyleSheet("color:green;");
        const QString settingsStyle = settings->styleSheet(), bookmarksStyle = bookmarks->styleSheet(),
                      sessionStyle = session->styleSheet();
        ToolbarComposition controls{toolbar, rows, exit, settings, bookmarks, clock, session, battery};
        QPointer<QWidget> retainedSettings(settings);
        {
            ::kobalt::reader::Navigation navigation(&owner, controls);
            QVERIFY(!rows->isEnabled());
            QCOMPARE(settings->paintMode(), ui::ButtonPaintMode::ToolbarCell);
            QCOMPARE(bookmarks->paintMode(), ui::ButtonPaintMode::ToolbarCell);
        }
        QVERIFY(retainedSettings);
        QVERIFY(rows->isEnabled());
        QCOMPARE(settings->paintMode(), ui::ButtonPaintMode::ToolbarSquare);
        QCOMPARE(bookmarks->paintMode(), ui::ButtonPaintMode::OutlinedAction);
        QCOMPARE(settings->styleSheet(), settingsStyle);
        QCOMPARE(bookmarks->styleSheet(), bookmarksStyle);
        QCOMPARE(session->styleSheet(), sessionStyle);
        QCOMPARE(settings->size(),
                 QSize(ui::metrics::control::toolbarButtonSize, ui::metrics::control::toolbarButtonSize));
    }
    void delayedProgressPaintsAfterTheEventLoop() {
        QWidget owner;
        owner.resize(600, 800);
        owner.show();
        ui::ModalOverlay loading(&owner);
        loading.card()->message("Applying appearance changes", "rrUpdatingText");
        loading.fitCard();
        QVERIFY(loading.progressUpdateRect(35, "Applying appearance changes").isEmpty());
        loading.setDelayedProgress(true, 1100, 20, "Applying appearance changes");
        QCoreApplication::processEvents();
        auto *bar = loading.findChild<QProgressBar *>("rrApplyingProgress");
        QVERIFY(bar);
        QVERIFY(bar->isVisible());
        QCOMPARE(loading.progressUpdateRect(35, "Applying appearance changes"),
                 QRect(bar->mapTo(loading.window(), QPoint()), bar->size()));
        QVERIFY(loading.progressUpdateRect(20, "Applying appearance changes").isEmpty());
        QVERIFY(loading.progressUpdateRect(35, "Updating page layout").isEmpty());
        PaintRegions paints;
        bar->installEventFilter(&paints);
        PaintRegions ownerPaints;
        owner.installEventFilter(&ownerPaints);

        loading.setDelayedProgress(true, 1100, 35, "Applying appearance changes");
        QCOMPARE(bar->value(), 35);
        QCOMPARE(paints.count, 0);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);
        QVERIFY((ownerPaints.painted - QRegion(QRect(bar->mapTo(&owner, QPoint()), bar->size()))).isEmpty());

        paints.count = 0;
        paints.painted = {};
        loading.setDelayedProgress(true, 1100, 35, "Applying appearance changes");
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);

        loading.setDelayedProgress(true, 1100, 50, "Applying appearance changes");
        loading.setDelayedProgress(true, 1100, 65, "Applying appearance changes");
        QCOMPARE(bar->value(), 65);
        QCOMPARE(paints.count, 0);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);

        owner.setUpdatesEnabled(false);
        loading.setDelayedProgress(true, 1100, 80, "Applying appearance changes");
        owner.setUpdatesEnabled(true);
        QVERIFY(bar->updatesEnabled());
        QCoreApplication::processEvents();

        paints.count = 0;
        loading.setDelayedProgress(false, 1100, 100);
        QCOMPARE(bar->value(), 0);
        QVERIFY(!bar->isVisible());
        QCOMPARE(paints.count, 0);
        QVERIFY(loading.progressUpdateRect(35, "Applying appearance changes").isEmpty());
    }
    void unchangedLightNoticeDoesNotRepaint() {
        QWidget owner;
        owner.resize(600, 800);
        ui::LightSwipeNotice notice(&owner);
        notice.centerIn(owner.size());
        notice.setLevel(false, 100);
        owner.show();
        notice.show();
        QCoreApplication::processEvents();
        PaintRegions paints;
        notice.installEventFilter(&paints);
        for (auto *widget : notice.findChildren<QWidget *>())
            widget->installEventFilter(&paints);
        notice.setLevel(false, 101);
        notice.setLevel(false, 100);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        notice.setLevel(true, 100);
        QCoreApplication::processEvents();
        QVERIFY(paints.count > 0);
        paints.count = 0;
        notice.setLevel(true, -1);
        QCoreApplication::processEvents();
        QVERIFY(paints.count > 0);
        paints.count = 0;
        notice.setLevel(true, 0);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        owner.setProperty("reader_dark", true);
        notice.setLevel(true, 0);
        auto *caption = notice.findChild<QLabel *>("rrLightSwipeCaption");
        QVERIFY(caption);
        QVERIFY(caption->property("reader_style").toString().contains("#aaaaaa"));
    }
    void appearanceOpeningDoesNotRepaintToolbar_data() {
        QTest::addColumn<QString>("buttonName");
        QTest::newRow("text") << "rrNavText";
        QTest::newRow("layout") << "rrNavLayout";
        QTest::newRow("stats") << "rrNavStats";
        QTest::newRow("font-picker-back") << "rrFontClose";
    }
    void appearanceOpeningDoesNotRepaintToolbar() {
        QFETCH(QString, buttonName);
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(nativeSize + QSize(40, 40));
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        if (buttonName == "rrFontClose") {
            host.window()->findChild<QPushButton *>("rrNavText")->click();
            host.window()->findChild<QPushButton *>("rrFont")->click();
        }
        auto *toolbar = host.window()->findChild<QWidget *>("rrControlsTop");
        auto *button = host.window()->findChild<QPushButton *>(buttonName);
        QVERIFY(toolbar);
        QVERIFY(button);
        QCoreApplication::processEvents();
        PaintRegions paints;
        toolbar->installEventFilter(&paints);
        for (auto *widget : toolbar->findChildren<QWidget *>())
            widget->installEventFilter(&paints);
        // Measure the view transition without button press/release feedback.
        button->click();
        QVERIFY(!host.session()->busy());
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
    }
    void openingProgressPaintsAfterTheEventLoop() {
        QWidget owner;
        owner.resize(600, 800);
        auto opening = createEmbeddedOpening(&owner, test::makeHostPresentation);
        opening->window()->setGeometry(owner.rect());
        owner.show();
        opening->window()->show();
        QCoreApplication::processEvents();
        auto *bar = opening->window()->findChild<QProgressBar *>("rrOpeningProgress");
        QVERIFY(bar);
        QVERIFY(bar->isVisible());
        PaintRegions paints;
        bar->installEventFilter(&paints);
        opening->setProgress(20);
        opening->setProgress(40);
        QCOMPARE(bar->value(), 40);
        QCOMPARE(paints.count, 0);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 1);
        paints.count = 0;
        opening->setProgress(40);
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        opening->setProgress(60);
        opening->setProgress(100);
        opening->window()->hide();
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
    }
    void openingFirstPaintCallback() {
        QWidget owner;
        owner.resize(600, 800);
        auto opening = createEmbeddedOpening(&owner, test::makeHostPresentation);
        opening->window()->setGeometry(owner.rect());
        int calls = 0;
        opening->afterFirstPaint([&] {
            ++calls;
        });
        QCoreApplication::processEvents();
        QCOMPARE(calls, 0);
        owner.show();
        opening->window()->show();
        QTRY_COMPARE(calls, 1);
        opening->window()->update();
        QCoreApplication::processEvents();
        QCOMPARE(calls, 1);

        QWidget otherOwner;
        otherOwner.resize(600, 800);
        auto discarded = createEmbeddedOpening(&otherOwner, test::makeHostPresentation);
        discarded->afterFirstPaint([&] {
            ++calls;
        });
        discarded.reset();
        otherOwner.show();
        QCoreApplication::processEvents();
        QCOMPARE(calls, 1);
    }
    void panelPaginationKeepsStaticLabelsUnpainted_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("stats") << "stats";
        QTest::newRow("chapters") << "chapters";
        QTest::newRow("bookmarks") << "bookmarks";
        QTest::newRow("acknowledgments") << "acknowledgments";
        QTest::newRow("license") << "license";
    }
    void panelPaginationKeepsStaticLabelsUnpainted() {
        QFETCH(QString, kind);
        QWidget owner;
        owner.resize(nativeSize);
        QWidget *panel = nullptr;
        QString prefix, titleName;
        if (kind == "stats") {
            auto *graph = new ChapterGraph(&owner);
            graph->resize(1072, ui::metrics::chapters::graphHeight);
            graph->setDense(false);
            QVector<ChapterLink> chapters;
            for (int i = 0; i < 25; ++i)
                chapters.append({QString::number(i), QString::number(i), i, 0, 100 * (i + 1)});
            graph->setData(chapters, {}, true, {});
            graph->setProgress("A chapter", 1, 10, 20, 0, 100);
            graph->open();
            panel = graph;
            prefix = "rrGraph";
            titleName = "rrGraphChapterTitle";
        } else if (kind == "chapters") {
            auto *contents = new ChapterContents(&owner);
            contents->resize(1072, 900);
            QVector<Choice> choices;
            for (int i = 0; i < 25; ++i)
                choices.append({QString::number(i), QString::number(i), QString("choice%1").arg(i)});
            contents->showChapters("Book", choices, {});
            panel = contents;
            prefix = "rrContents";
            titleName = "rrContentsTitle";
        } else if (kind == "acknowledgments" || kind == "license") {
            auto *acknowledgments = new Acknowledgments(&owner);
            acknowledgments->resize(1072, 900);
            acknowledgments->open();
            if (kind == "license") {
                auto *entry = acknowledgments->findChild<QPushButton *>("rrAcknowledgment0");
                QVERIFY(entry);
                entry->click();
            }
            panel = acknowledgments;
            prefix = "rrAcknowledgments";
            titleName = "rrAcknowledgmentsTitle";
        } else {
            auto *bookmarks = new BookmarkList(&owner);
            bookmarks->resize(1072, 900);
            QVector<BookmarkRow> rows;
            for (int i = 0; i < 25; ++i)
                rows.append({QString::number(i), QString("Chapter %1").arg(i), "An excerpt", {}, i, false});
            bookmarks->showBookmarks(rows, {});
            panel = bookmarks;
            prefix = "rrBookmarkList";
            titleName = "rrBookmarkListTitle";
        }
        owner.show();
        QCoreApplication::processEvents();
        auto *title = panel->findChild<QWidget *>(titleName);
        auto *next = panel->findChild<QPushButton *>(prefix + "Next");
        QVERIFY(title);
        QVERIFY(next);
        QVERIFY(next->isEnabled());
        PaintRegions paints;
        title->installEventFilter(&paints);
        if (kind == "stats")
            for (const auto *name : {"rrGraphChapterCell", "rrGraphBookCell", "rrGraphSessionCell"}) {
                auto *cell = panel->findChild<QWidget *>(name);
                QVERIFY(cell);
                cell->installEventFilter(&paints);
            }
        PaintRegions damage;
        damage.coordinateRoot = panel;
        panel->installEventFilter(&damage);
        for (auto *widget : panel->findChildren<QWidget *>())
            widget->installEventFilter(&damage);
        auto *counter = panel->findChild<QLabel *>(prefix + "Page");
        QVERIFY(counter);
        const auto firstPage = counter->text();
        auto before = panel->grab().toImage();
        int turns = 0;
        do {
            QVERIFY(++turns < 128);
            paints.count = 0;
            damage.painted = {};
            next->click();
            QCoreApplication::processEvents();
            QCOMPARE(paints.count, 0);
            const auto changed = damage.painted;
            const auto after = panel->grab().toImage();
            QVERIFY(before != after);
            for (int y = 0; y < after.height(); ++y)
                for (int x = 0; x < after.width(); ++x)
                    if (before.pixel(x, y) != after.pixel(x, y))
                        QVERIFY(changed.contains(QPoint(x, y)));
            before = after;
        } while (next->isEnabled() && counter->text().split(" of ").first() != counter->text().split(" of ").last());
        if (kind != "stats" && kind != "license") {
            auto *previous = panel->findChild<QPushButton *>(prefix + "Previous");
            QVERIFY(previous);
            const auto lastPage = counter->text();
            QVERIFY(next->isEnabled());
            next->click();
            QCOMPARE(counter->text(), firstPage);
            previous->click();
            QCOMPARE(counter->text(), lastPage);
            QCOMPARE(panel->grab().toImage(), before);
        }
    }
    void navigationReplayDoesNotInvalidateRules_data() {
        QTest::addColumn<bool>("bluetoothActive");
        QTest::newRow("plain") << false;
        QTest::newRow("bluetooth") << true;
    }
    void navigationReplayDoesNotInvalidateRules() {
        QFETCH(bool, bluetoothActive);
        QWidget owner;
        owner.resize(600, 900);
        auto *toolbar = new ui::Panel(&owner);
        auto *rows = new QVBoxLayout(toolbar);
        auto *exit = new QPushButton(toolbar);
        auto *settings = new ui::Button(toolbar), *bookmarks = new ui::Button(toolbar);
        auto *clock = new QLabel(toolbar), *session = new QLabel(toolbar);
        auto *battery = new ui::BatteryIndicator(toolbar);
        auto *bluetooth = new ui::BluetoothIndicator(toolbar);
        bluetooth->setActive(bluetoothActive);
        session->setText("Reading for 10 minutes");
        reader::Navigation navigation(&owner,
                                      {toolbar, rows, exit, settings, bookmarks, clock, session, battery, bluetooth});
        NavigationState state{};
        state.footer = new QWidget(&owner);
        state.appearance = new QWidget(&owner);
        state.stats = new QWidget(&owner);
        state.chapters = new QWidget(&owner);
        state.settings = new QWidget(&owner);
        state.light = new QWidget(&owner);
        for (auto *widget : {state.footer, state.appearance, state.stats, state.chapters, state.settings, state.light})
            widget->hide();
        state.visible = true;
        state.enabled = true;
        state.lightAvailable = true;
        state.active = -1;
        state.pageCount = "1";
        owner.show();
        navigation.update(owner.size(), state);
        QCoreApplication::processEvents();
        PaintRegions upper, lower, bottom, sessionPaints;
        session->installEventFilter(&sessionPaints);
        auto *upperRule = owner.findChild<QWidget *>("rrNavigationUpperEdge");
        auto *lowerRule = owner.findChild<QWidget *>("rrNavigationLowerEdge");
        auto *bar = owner.findChild<QWidget *>("rrBottomNavigation");
        QVERIFY(upperRule);
        QVERIFY(lowerRule);
        QVERIFY(bar);
        upperRule->installEventFilter(&upper);
        lowerRule->installEventFilter(&lower);
        bar->installEventFilter(&bottom);
        QCoreApplication::processEvents();
        upper.painted = {};
        lower.painted = {};
        bottom.painted = {};
        sessionPaints.painted = {};
        navigation.update(owner.size(), state);
        QCoreApplication::processEvents();
        QVERIFY(sessionPaints.painted.isEmpty());
        QVERIFY(upper.painted.isEmpty());
        QVERIFY(lower.painted.isEmpty());
        QVERIFY(bottom.painted.isEmpty());
        auto *bottomNavigation = dynamic_cast<reader::BottomNavigation *>(bar);
        QVERIFY(bottomNavigation);
        bottomNavigation->setPanelOpen(true);
        QCoreApplication::processEvents();
        QVERIFY(!bottom.painted.isEmpty());
        QVERIFY(QRegion(QRect(0, 0, bar->width(), int(ui::metrics::chrome::heavyRule)))
                    .contains(bottom.painted.boundingRect()));
        bottom.painted = {};
        bottomNavigation->setPanelOpen(true);
        QCoreApplication::processEvents();
        QVERIFY(bottom.painted.isEmpty());
    }
    void appearancePageComposition() {
        QWidget owner;
        auto *typography = new QWidget(&owner), *layout = new QWidget(&owner);
        auto *typeRows = new QVBoxLayout(typography), *pageRows = new QVBoxLayout(layout);
        QPointer<ui::FontButton> retainedFont;
        {
            reader::AppearancePanelInputs inputs;
            inputs.headerSize = "small";
            inputs.footerSize = "off";
            inputs.sideButtons = true;
            inputs.reverseLandscapeButtons = true;
            inputs.automaticRotationAvailable = true;
            inputs.showClock = false;
            reader::AppearancePanel panel({typography, layout, typeRows, pageRows}, inputs);
            retainedFont = panel.fontButton;
            QCOMPARE(panel.fontButton->parentWidget(), typography);
            QCOMPARE(panel.marginSlider->parentWidget()->parentWidget(), layout);
            QCOMPARE(panel.sizeSlider->minimum(), 14);
            QCOMPARE(panel.sizeSlider->maximum(), 96);
            QCOMPARE(panel.sizeSlider->singleStep(), 2);
            QCOMPARE(panel.weightSlider->minimum(), 0);
            QCOMPARE(panel.weightSlider->maximum(), 12);
            QCOMPARE(panel.weightSlider->singleStep(), 1);
            QCOMPARE(panel.spacingSlider->minimum(), 100);
            QCOMPARE(panel.spacingSlider->maximum(), 300);
            QCOMPARE(panel.spacingSlider->singleStep(), 1);
            QCOMPARE(panel.marginSlider->singleStep(), 3);
            QCOMPARE(panel.verticalMarginSlider->singleStep(), 3);
            QCOMPARE(panel.verticalMarginSlider->parentWidget()->parentWidget(), layout);
            QCOMPARE(panel.headerSizeButtons.size(), 4);
            QVERIFY(panel.headerSizeButtons[1]->isChecked());
            QCOMPARE(panel.footerSizeButtons.size(), 4);
            QVERIFY(panel.footerSizeButtons[0]->isChecked());
            QCOMPARE(panel.rotationButtons.size(), 2);
            QCOMPARE(panel.rotationButtons[0]->text(), strings::get("reader.portrait"));
            QCOMPARE(panel.rotationButtons[1]->text(), strings::get("reader.landscape"));
            QCOMPARE(panel.autoRotateOption->text(), strings::get("reader.both"));
            QVERIFY(!panel.autoRotateOption->icon().isNull());
            QCOMPARE(panel.rotationButtons[0]->accessibleName(), strings::get("reader.lock_portrait"));
            QCOMPARE(panel.rotationButtons[1]->accessibleName(), strings::get("reader.lock_landscape"));
            const QList<int> orientations = {1, 2};
            for (int i = 0; i < orientations.size(); ++i) {
                QCOMPARE(panel.rotationButtons[i]->property("orientation").toInt(), orientations[i]);
                const auto *button = dynamic_cast<ui::Button *>(panel.rotationButtons[i]);
                QVERIFY(button);
                QVERIFY(button->iconConfiguration() != nullptr);
                QVERIFY(button->iconConfiguration()->rotationSideButtons);
            }
            QVERIFY(!panel.autoRotateOption->isHidden());
            QVERIFY(panel.clockHideOption->isChecked());
            QVERIFY(!panel.clockOption->isChecked());
            QCOMPARE(panel.alignmentButtons.size(), 3);
            QCOMPARE(panel.hyphenButtons.size(), 3);
            // Construction installs no session policy. The reader owns each callback.
            QVERIFY(!panel.sizeSlider->committed);
            QVERIFY(!panel.marginSlider->committed);
        }
        QVERIFY(retainedFont); // The pages retain their controls after composition returns.
        delete typography;
        QVERIFY(retainedFont.isNull());
    }
    void appearanceContentMeasurement_data() {
        QTest::addColumn<int>("ppi");
        for (const int ppi : {212, 300}) {
            QTest::newRow(qPrintable(QString::number(ppi))) << ppi;
        }
    }
    void appearanceContentMeasurement() {
        QFETCH(int, ppi);
        struct RestoreDensity {
            int value = ui::density();
            ~RestoreDensity() {
                ui::setDensity(value);
            }
        } restore;
        ui::setDensity(ppi);
        QWidget owner;
        owner.resize(nativeSize);
        owner.setFont(ui::interfaceFont());
        reader::AppearanceSheet sheet(&owner), androidSheet(&owner);
        reader::AppearancePanel panel(sheet.controls(), {});
        reader::AppearancePanelInputs inputs;
        inputs.systemRotation = true;
        reader::AppearancePanel androidPanel(androidSheet.controls(), inputs);
        sheet.pages->setCurrentIndex(1);
        androidSheet.pages->setCurrentIndex(1);
        const int layoutHeight = sheet.fitHeight(nativeSize.width(), 1400);
        const int androidHeight = androidSheet.fitHeight(nativeSize.width(), 1400);
        QVERIFY(androidHeight < layoutHeight);
        // The inactive Text page must not reserve space in Layout.
        sheet.controls().typeRows->addSpacing(2000);
        QCOMPARE(sheet.fitHeight(nativeSize.width(), 1400), layoutHeight);
        androidSheet.pages->setCurrentIndex(0);
        const int textHeight = androidSheet.fitHeight(nativeSize.width(), 1400);
        androidPanel.opticalSizeSlider->parentWidget()->show();
        const int opticalHeight = androidSheet.fitHeight(nativeSize.width(), 1400);
        QVERIFY(opticalHeight > textHeight);
        androidPanel.variableWeightSlider->parentWidget()->show();
        QVERIFY(androidSheet.fitHeight(nativeSize.width(), 1400) > opticalHeight);
        androidPanel.opticalSizeSlider->parentWidget()->hide();
        androidPanel.variableWeightSlider->parentWidget()->hide();
        QCOMPARE(androidSheet.fitHeight(nativeSize.width(), 1400), textHeight);
        androidSheet.message->setText(QStringLiteral("Could not apply settings. ").repeated(8));
        androidSheet.message->show();
        QVERIFY(androidSheet.fitHeight(nativeSize.width(), 1400) > textHeight);
        androidSheet.message->hide();
        QCOMPARE(androidSheet.fitHeight(nativeSize.width(), 1400), textHeight);
        androidSheet.pages->setCurrentIndex(1);
        owner.show();
        androidSheet.show();
        const int limitedHeight = qMax(1, androidHeight / 2);
        QCOMPARE(androidSheet.fitHeight(nativeSize.width(), limitedHeight), limitedHeight);
        QCoreApplication::processEvents();
        auto *scroll = androidSheet.findChild<QScrollArea *>("rrAppearanceScroll");
        QVERIFY(scroll);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        scroll->ensureWidgetVisible(androidPanel.clockOption);
        QCoreApplication::processEvents();
        QVERIFY(scroll->viewport()->rect().contains(
            androidPanel.clockOption->mapTo(scroll->viewport(), androidPanel.clockOption->rect().center())));
        QCOMPARE(androidSheet.fitHeight(nativeSize.width(), 1400), androidHeight);
    }
    void marginNotchesMatchStepPositions() {
        const int originalDensity = ui::density();
        struct RestoreDensity {
            int ppi;
            ~RestoreDensity() {
                ui::setDensity(ppi);
            }
        } restore{originalDensity};
        for (const int ppi : {212, 300}) {
            ui::setDensity(ppi);
            QWidget owner;
            owner.setFont(ui::interfaceFont());
            QWidget typography(&owner), layout(&owner);
            auto *typeRows = new QVBoxLayout(&typography), *pageRows = new QVBoxLayout(&layout);
            reader::AppearancePanel panel({&typography, &layout, typeRows, pageRows}, {});
            layout.resize(ppi == 212 ? 758 : 1264, 800);
            owner.show();
            layout.show();
            QCoreApplication::processEvents();
            for (auto *slider : {panel.marginSlider, panel.verticalMarginSlider}) {
                const auto image = slider->grab().toImage();
                const int inset = ui::metrics::control::sliderInset;
                const int span = slider->width() - 2 * inset;
                const int y = ui::metrics::control::sliderMarkerTop + 1;
                const auto paper = slider->palette().color(QPalette::Window);
                for (int value = 0; value <= 120; value += 3) {
                    const int x = inset + QStyle::sliderPositionFromValue(0, 120, value, span);
                    QVERIFY(image.pixelColor(x, y) != paper);
                    if (value < 120) {
                        const int next = inset + QStyle::sliderPositionFromValue(0, 120, value + 3, span);
                        QCOMPARE(image.pixelColor((x + next) / 2, y), paper);
                    }
                }
                const int x = inset + QStyle::sliderPositionFromValue(0, 120, 62, span);
                QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, QPoint(x, slider->height() / 2));
                QCOMPARE(slider->value(), 63);
                slider->nudge(1);
                QCOMPARE(slider->value(), 66);
            }
        }
    }
    void dictionaryManagementView() {
        QWidget owner;
        reader::DictionariesPage page(&owner);
        owner.resize(1264, 1400);
        page.resize(owner.size());
        owner.show();
        page.show();
        const auto catalog = DictionaryManager::catalog();
        page.setLanguage("en-US");
        page.setCatalog(catalog);
        page.setState({"en-en", "ar-ar"}, false, {});
        page.setStorage({{"en-en", 92000000}, {"ar-ar", 3500000}}, 1000000000);
        QCoreApplication::processEvents();
        auto *first = page.findChild<QWidget *>("rrDictionaryRow0");
        QVERIFY(first);
        QCOMPARE(first->property("dictionary_id").toString(), QString("en-en"));
        QVERIFY(!page.findChild<QLineEdit *>("rrDictionariesFilter"));
        QString removed;
        page.removeRequested = [&](const QString &id) {
            removed = id;
        };
        page.findChild<QPushButton *>("rrDictionaryAction0")->click();
        QVERIFY(removed.isEmpty());
        auto *confirm = owner.findChild<QPushButton *>("rrDictionaryRemoveConfirm");
        QVERIFY(confirm);
        confirm->click();
        QCOMPARE(removed, QString("en-en"));
        int actionWidth = -1;
        for (auto *button : page.findChildren<QPushButton *>())
            if (button->objectName().startsWith("rrDictionaryAction")) {
                if (actionWidth < 0)
                    actionWidth = button->width();
                QCOMPARE(button->width(), actionWidth);
            }
    }
    void dictionaryTransferSurvivesNavigationAndResize() {
        QWidget owner;
        reader::DictionariesPage page(&owner);
        owner.resize(1264, 1400);
        page.resize(owner.size());
        owner.show();
        page.show();
        page.setLanguage("en");
        page.setCatalog(DictionaryManager::catalog());
        page.setState({"en-en"}, true, {});
        page.setTransfer("en-fr", 3100000, 6900000, false);
        QCoreApplication::processEvents();
        int stopped = 0;
        page.cancelRequested = [&] {
            ++stopped;
        };
        page.hide();
        page.setTransfer("en-fr", 4000000, 6900000, false);
        page.show();
        QCoreApplication::processEvents();
        QVERIFY(page.findChild<QProgressBar *>());
        QCOMPARE(stopped, 0);
        for (auto *button : page.findChildren<QPushButton *>())
            if (button->objectName().startsWith("rrDictionaryAction")) {
                if (button->text() == strings::get("dictionaries.stop")) {
                    button->click();
                    QCOMPARE(stopped, 1);
                } else if (button->text() == strings::get("dictionaries.remove"))
                    QVERIFY(button->isEnabled());
                else
                    QVERIFY(!button->isEnabled());
            }
    }
    void dictionaryFailedDownloadReturnsToGet() {
        QWidget owner;
        reader::DictionariesPage page(&owner);
        owner.resize(1264, 1400);
        page.resize(owner.size());
        owner.show();
        page.show();
        page.setLanguage("en");
        page.setCatalog(DictionaryManager::catalog());
        page.setState({}, true, {});
        page.setTransfer("en-en", 100, 1000, false);
        page.setTransfer({}, 0, 0, false);
        page.setState({}, false, "Download failed");
        QCoreApplication::processEvents();
        auto *action = page.findChild<QPushButton *>("rrDictionaryAction0");
        QVERIFY(action);
        QCOMPARE(action->text(), strings::get("dictionaries.get"));
        QVERIFY(action->isEnabled());
        bool failed = false;
        for (auto *label : page.findChildren<QLabel *>())
            failed |= label->text() == strings::get("dictionaries.download_failed");
        QVERIFY(failed);
    }
    void searchClearConstructionAndReplay() {
        const int savedDensity = ui::density();
        struct RestoreDensity {
            int value;
            ~RestoreDensity() {
                ui::setDensity(value);
            }
        } restore{savedDensity};
        for (int ppi : {167, 212, 300})
            for (bool initiallyDark : {false, true}) {
                ui::setDensity(ppi);
                QWidget owner;
                owner.resize(1264, 1680);
                const auto theme = [&](bool dark) {
                    owner.setProperty("reader_dark", dark);
                    auto palette = owner.palette();
                    palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
                    palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
                    palette.setColor(QPalette::Button, dark ? Qt::black : Qt::white);
                    palette.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
                    palette.setColor(QPalette::Base, dark ? Qt::black : Qt::white);
                    palette.setColor(QPalette::Text, dark ? Qt::white : Qt::black);
                    owner.setPalette(palette);
                };
                theme(initiallyDark);
                SearchPanel panel(&owner, test::hostReaderServices().keyboard);
                panel.setGeometry(owner.rect());
                owner.show();
                panel.open(false, 0);
                auto *query = panel.findChild<QLineEdit *>("rrSearchQuery");
                auto *clear = panel.findChild<QPushButton *>("rrSearchClear");
                QVERIFY(query && clear);
                const auto *button = dynamic_cast<ui::Button *>(clear);
                QVERIFY(button);
                QCOMPARE(button->paintMode(), ui::ButtonPaintMode::Native);
                QVERIFY(button->iconConfiguration() != nullptr);
                QCOMPARE(button->iconConfiguration()->glyph, ui::ControlIcon::Close);
                QCOMPARE(button->iconConfiguration()->rendition, ui::IconRendition::OriginalAsset);
                query->setText("Alice");
                QCoreApplication::processEvents();
                QSignalSpy clicked(clear, &QPushButton::clicked);
                const auto constructed = clear->grab().toImage();
                ui::refreshButtonIcon(clear);
                QCOMPARE(clear->grab().toImage(), constructed);
                for (bool dark : {!initiallyDark, initiallyDark}) {
                    theme(dark);
                    ui::refreshButtonIcon(clear);
                    const auto replayed = clear->grab().toImage();
                    ui::refreshButtonIcon(clear);
                    QCOMPARE(clear->grab().toImage(), replayed);
                }
                QCOMPARE(clear->grab().toImage(), constructed);
                QCOMPARE(clicked.count(), 0);
                QTest::mouseClick(clear, Qt::LeftButton);
                QCOMPARE(query->text(), QString());
                QCOMPARE(clicked.count(), 1);
            }
    }
    void settingsPageComposition() {
        QWidget owner;
        reader::SettingsPanelInputs inputs;
        inputs.release = "0.4";
        inputs.commit = "test*";
        inputs.commitVisible = true;
        inputs.qtMajor = 6;
        auto *panel = new reader::SettingsPanel(&owner, inputs);
        QCOMPARE(panel->parentWidget(), &owner);
        QCOMPARE(panel->objectName(), QString("rrGeneralSettings"));
        QVERIFY(panel->isHidden());
        QVERIFY(panel->settingsError->isHidden());
        QCOMPARE(panel->settingsPages->count(), 5);
        const QStringList pages = {"rrDisplayOptions", "rrAdvancedOptions", "rrDictionaries", "rrStatsOptions",
                                   "rrAbout"};
        for (int index = 0; index < pages.size(); ++index)
            QCOMPARE(panel->settingsPages->widget(index)->objectName(), pages[index]);
        QCOMPARE(panel->settingsTabButtons.size(), 5);
        auto *sections = panel->findChild<QBoxLayout *>("settingsSectionLayout");
        QVERIFY(sections);
        QCOMPARE(sections->direction(), QBoxLayout::LeftToRight);
        auto *rail = panel->findChild<QWidget *>("rrSettingsRail");
        QVERIFY(rail);
        QCOMPARE(static_cast<QBoxLayout *>(rail->layout())->direction(), QBoxLayout::TopToBottom);
        QCOMPARE(rail->width(), int(ui::metrics::chrome::railWidth));
        const QStringList tabNames = {"rrSettingsDisplay", "rrSettingsAdvanced", "rrSettingsDictionaries",
                                      "rrSettingsStats", "rrSettingsAbout"};
        for (int index = 0; index < tabNames.size(); ++index)
            QCOMPARE(panel->settingsTabButtons[index]->objectName(), tabNames[index]);
        for (auto *option : panel->findChildren<ui::DescribedOption *>())
            QVERIFY(option->presentation().indicator == ui::DescribedOption::Indicator::Square);
        QCOMPARE(panel->smoothOption->presentation().minimumRowHeight, int(ui::metrics::chrome::listActionHeight));
        QVERIFY(!panel->findChild<QPushButton *>("rrManageDictionaries"));
        QCOMPARE(panel->findChildren<ui::DescribedOption *>().size(), 11);
        QCOMPARE(panel->bionicOption->parentWidget(), panel->settingsPages->widget(reader::DisplaySettings));
        QVERIFY(!panel->animationOption->isEnabled());
        QVERIFY(!panel->edgeLightOption->isEnabled());
        QVERIFY(!panel->debugProgress);
        QVERIFY(!panel->findChild<QLabel *>("rrAboutCommit")->isHidden());
        for (int index = 0; index < panel->settingsTabButtons.size(); ++index) {
            panel->settingsTabButtons[index]->click();
            QCOMPARE(panel->settingsPages->currentIndex(), index);
            QVERIFY(panel->settingsTabButtons[index]->isChecked());
            QVERIFY(panel->settingsTabButtons[index]->property("kobalt_selected").toBool());
        }
        panel->resetDefaults->click();
        QCOMPARE(panel->settingsPages->currentIndex(), int(reader::AdvancedSettings));
        QVERIFY(panel->settingsTabButtons[reader::AdvancedSettings]->isChecked());
        panel->keepDefaults->click();
        QCOMPARE(panel->settingsPages->currentIndex(), int(reader::AdvancedSettings));
        QVERIFY(panel->resetDefaults->isEnabled());
        inputs.animationsSupported = true;
        inputs.lightSupported = true;
        inputs.warmthSupported = true;
        auto *supported = new reader::SettingsPanel(&owner, inputs);
        QVERIFY(supported->animationOption->isEnabled());
        QVERIFY(supported->edgeLightOption->isEnabled());
    }
    void settingsContentMeasurement() {
        QWidget owner;
        owner.resize(nativeSize);
        reader::SettingsPanelInputs inputs;
        inputs.desktopUi = true;
        reader::SettingsPanel panel(&owner, inputs);
        const QSize available(nativeSize.width(), 1200);
        const int displayHeight = panel.fitContent(available);
        QCOMPARE(panel.settingsPages->height(), displayHeight);
        QVERIFY(displayHeight >= panel.findChild<QWidget *>("rrSettingsRail")->sizeHint().height());
        panel.settingsPages->setCurrentIndex(reader::DictionariesSettings);
        QCOMPARE(panel.fitContent(available), available.height());
        panel.settingsPages->setCurrentIndex(reader::AboutSettings);
        const int aboutHeight = panel.fitContent(available);
        QVERIFY(aboutHeight >= panel.findChild<QWidget *>("rrAboutContent")->sizeHint().height());
        auto *description = panel.findChild<QLabel *>("rrAboutDescription");
        QVERIFY(description);
        const int wideHeight = description->height();
        const auto originalDescription = description->text();
        description->setText(originalDescription.repeated(4));
        const QSize narrow(600, 1000);
        panel.fitContent(narrow);
        QCOMPARE(description->width(), narrow.width() - int(ui::metrics::chrome::railWidth) - ui::px(32));
        QCOMPARE(description->height(), description->heightForWidth(description->width()));
        QVERIFY(description->height() > wideHeight);
        QVERIFY(panel.settingsPages->height() > aboutHeight);
        description->setText(originalDescription);
        QCOMPARE(panel.fitContent(available), aboutHeight);
        QCOMPARE(description->height(), wideHeight);
        auto *nativeTab = panel.findChild<QPushButton *>("rrSettingsUI");
        QVERIFY(nativeTab);
        nativeTab->click();
        QCOMPARE(panel.settingsPages->currentIndex(), int(reader::NativeUiSettings));
        QVERIFY(nativeTab->isChecked());
        QVERIFY(nativeTab->property("kobalt_selected").toBool());
        panel.hideDeviceOptionsOption->setChecked(true);
        QVERIFY(panel.animationOption->isHidden());
        QVERIFY(panel.refreshButton->isHidden());
        panel.hideDeviceOptionsOption->setChecked(false);
        QVERIFY(!panel.animationOption->isHidden());
        QVERIFY(!panel.refreshButton->isHidden());
    }
    void settingsWithoutDictionaryDownloads() {
        QWidget owner;
        reader::SettingsPanelInputs inputs;
        inputs.downloadableDictionaries = false;
        reader::SettingsPanel panel(&owner, inputs);
        panel.resize(1072, 1000);
        owner.show();
        panel.show();
        const auto *tab = panel.settingsTabButtons[reader::DictionariesSettings];
        QVERIFY(tab->isHidden());
        QVERIFY(!tab->isEnabled());
        QVERIFY(!panel.dictionaries->installRequested);
        for (const auto index :
             {reader::DisplaySettings, reader::AdvancedSettings, reader::StatsSettings, reader::AboutSettings}) {
            auto *button = panel.settingsTabButtons[index];
            QVERIFY(!button->isHidden());
            button->click();
            QCOMPARE(panel.settingsPages->currentIndex(), int(index));
            QVERIFY(button->isChecked());
        }
    }
    void canvasInputRespectsModalBarrier() {
        auto *surface = new QWidget;
        surface->resize(420, 600);
        auto *button = new ui::Button("Underlying action", surface);
        button->setGeometry(20, 20, 180, 60);
        QSignalSpy activated(button, &QPushButton::clicked);
        auto *overlay = new ui::NoticeModal(surface, {"Modal input barrier", "Notice", {}}, {"OK", "testModalConfirm"});
        int dismissals = 0;
        overlay->dismissed = [&] {
            ++dismissals;
        };
        test::ReaderCanvas canvas(surface, QSize(420, 600));
        canvas.resize(420, 600);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(click(canvas, button));
        QCOMPARE(activated.count(), 1);
        button->setEnabled(false);
        QVERIFY(!click(canvas, button));
        QVERIFY(clickAt(canvas, button->geometry().center()));
        QCOMPARE(activated.count(), 1);
        button->setEnabled(true);
        button->hide();
        QVERIFY(!click(canvas, button));
        button->show();
        for (bool dark : {false, true}) {
            surface->setProperty("reader_dark", dark);
            auto colors = surface->palette();
            colors.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
            colors.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
            surface->setPalette(colors);
            overlay->fitCard();
            overlay->show();
            overlay->raise();
            QTRY_COMPARE(overlay->geometry(), surface->rect());
            QVERIFY(overlay->rect().contains(overlay->card()->geometry()));
            QVERIFY(!click(canvas, button));
            QCOMPARE(activated.count(), 1);
            const int before = dismissals;
            QVERIFY(clickAt(canvas, button->geometry().center()));
            QCOMPARE(dismissals, before + 1);
            QCOMPARE(activated.count(), 1);
            surface->setFixedSize(dark ? QSize(420, 600) : QSize(600, 420));
            QTRY_COMPARE(overlay->geometry(), surface->rect());
            QVERIFY(overlay->rect().contains(overlay->card()->geometry()));
            QVERIFY((overlay->card()->geometry().center() - overlay->rect().center()).manhattanLength() <= 2);
            QVERIFY(!click(canvas, button));
            QCOMPARE(activated.count(), 1);
            overlay->hide();
        }
        QVERIFY(click(canvas, button));
        QCOMPARE(activated.count(), 2);
    }
    void bluetoothStatusOnlyInReadingControls() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        std::function<void(bool)> changed;
        QPointer<QObject> context;
        host.configureServices = [&](ReaderHostServices &services) {
            services.observeBluetooth = [&](QObject *owner, std::function<void(bool)> callback) {
                context = owner;
                changed = std::move(callback);
            };
        };
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(context);
        QVERIFY(bool(changed));
        auto *indicator = host.window()->findChild<ui::BluetoothIndicator *>("rrMenuBluetooth");
        auto *battery = host.window()->findChild<QWidget *>("rrMenuBattery");
        QVERIFY(indicator);
        QVERIFY(battery);
        QVERIFY(!indicator->isVisible());
        changed(true);
        QCoreApplication::processEvents();
        QVERIFY(indicator->active());
        QVERIFY(!indicator->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(indicator->isVisible());
        QVERIFY(indicator->geometry().right() < battery->geometry().left());
        QCOMPARE(indicator->geometry().center().y(), battery->geometry().center().y());
        QCOMPARE(indicator->accessibleName(), strings::get("components.bluetooth_enabled"));
        PaintRegions paints;
        indicator->installEventFilter(&paints);
        QCoreApplication::processEvents();
        paints.painted = {};
        changed(true);
        QCoreApplication::processEvents();
        QVERIFY(paints.painted.isEmpty());
        changed(false);
        QTRY_VERIFY(!indicator->isVisible());
        changed(true);
        QTRY_VERIFY(indicator->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(!indicator->isVisible());
    }
    void pageTurnsResumeWhenReaderIsShownAfterSleep() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        // wakeReader() shows the reader full screen. Open it at the screen
        // size so page numbers stay comparable across wake. The offscreen
        // plugin resizes a full screen window to the screen; minimal keeps
        // the current size.
        const QSize screenSize = QGuiApplication::primaryScreen()->geometry().size();
        host.open(fixture(), screenSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->setWindowFlags(Qt::Window);
        host.window()->setWindowModality(Qt::ApplicationModal);
        host.window()->showFullScreen();
        QTRY_COMPARE(host.window()->size(), screenSize);
        QTRY_COMPARE(QApplication::activeModalWidget(), host.window());
        QTRY_VERIFY(!host.session()->busy());
        host.session()->go(0);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 0);
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
        host.window()->sleepReader();
        host.window()->pageButton(1);
        host.window()->pageButton(-1);
        QCOMPARE(host.session()->page(), 1);
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QVERIFY(host.window()->isVisible());
        QTRY_COMPARE(host.window()->size(), screenSize);
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 2);
        host.window()->pageButton(-1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
    }

    void remoteKeyActivityRequiresActiveReading() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        int reports = 0;
        bool suspended = false;
        host.configureServices = [&](ReaderHostServices &services) {
            services.pageButtonDirection = [](QKeyEvent *event) {
                return event->key() == Qt::Key_Right ? 1 : 0;
            };
            services.suspended = [&] {
                return suspended;
            };
            services.remoteKeyActivity = [&](QObject *owner, QKeyEvent *event) {
                QCOMPARE(owner, static_cast<QObject *>(host.window()));
                QCOMPARE(event->type(), QEvent::KeyPress);
                ++reports;
            };
        };
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.window()->setWindowFlags(Qt::Window);
        host.window()->setWindowModality(Qt::ApplicationModal);
        host.window()->show();
        QTRY_VERIFY(!host.session()->busy());
        QTRY_COMPARE(QApplication::activeModalWidget(), host.window());
        auto sendKey = [&](QEvent::Type type, int key, bool repeat = false) {
            QKeyEvent event(type, key, Qt::NoModifier, 106, 0, 0, {}, repeat);
            QCoreApplication::sendEvent(host.window(), &event);
        };
        const int initial = host.session()->page();
        sendKey(QEvent::KeyPress, Qt::Key_Right);
        QCOMPARE(reports, 1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == initial + 1);
        sendKey(QEvent::KeyRelease, Qt::Key_Right);
        QCOMPARE(reports, 1);
        sendKey(QEvent::KeyPress, Qt::Key_Right, true);
        QCOMPARE(reports, 2);
        QCOMPARE(host.session()->page(), initial + 1);
        sendKey(QEvent::KeyPress, Qt::Key_A);
        QCOMPARE(reports, 2);
        suspended = true;
        sendKey(QEvent::KeyPress, Qt::Key_Right);
        QCOMPARE(reports, 2);
        suspended = false;
        host.window()->hide();
        sendKey(QEvent::KeyPress, Qt::Key_Right);
        QCOMPARE(reports, 2);
    }
    void lightSwipeTipMatchesCapabilities() {
        class Light final : public LightControls {
        public:
            bool warm = true;
            bool initialize() override {
                return true;
            }
            bool hasWarmth() const override {
                return warm;
            }
            int warmth() const override {
                return 0;
            }
            int warmthMinimum() const override {
                return 0;
            }
            int warmthMaximum() const override {
                return 100;
            }
            int warmthStep() const override {
                return 1;
            }
            bool automaticWarmth() const override {
                return false;
            }
            int setWarmth(int value) override {
                return value;
            }
            bool saveWarmth() override {
                return true;
            }
            bool setAutomaticWarmth(bool) override {
                return true;
            }
            bool beginGesture() override {
                return true;
            }
            int setGestureLevel(int value) override {
                return value;
            }
            void finishGesture(bool) override {}
            int brightness() const override {
                return 50;
            }
            bool gesturing() const override {
                return false;
            }
        } light;
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        LightPanel panel(light, &parent);
        auto *tip = panel.findChild<QLabel *>("rrLightSwipeTip");
        auto *warmth = panel.findChild<QSlider *>("rrWarmth");
        QVERIFY(tip);
        QVERIFY(warmth);
        QVERIFY(panel.open());
        QVERIFY(tip->isVisible());
        QVERIFY(tip->wordWrap());
        QCOMPARE(tip->text(), strings::get("settings.edge_light_description"));
        QVERIFY(warmth->isVisible());
        panel.hide();
        light.warm = false;
        QVERIFY(panel.open());
        QCOMPARE(tip->text(), strings::get("settings.edge_brightness_description"));
        QVERIFY(!warmth->isVisible());
    }
    void smallSliderCaptionsWrap() {
        const int originalDensity = ui::density();
        struct RestoreDensity {
            int ppi;
            ~RestoreDensity() {
                ui::setDensity(ppi);
            }
        } restore{originalDensity};
        for (const int ppi : {212, 300}) {
            ui::setDensity(ppi);
            QWidget parent;
            parent.setFont(ui::interfaceFont());
            parent.resize(ppi == 212 ? 768 : 1072, 400);
            auto *rows = new QVBoxLayout(&parent);
            for (const auto *key : {"reader.margins", "reader.vertical_margins"}) {
                const auto name = QString::fromLatin1(key).toLatin1();
                ui::sliderRow(&parent, rows, name.constData(), "value", strings::get(key), "less", "more", 0, 120, 12,
                              {});
            }
            parent.show();
            QCoreApplication::processEvents();
            for (const auto *key : {"reader.margins", "reader.vertical_margins"}) {
                auto *caption = parent.findChild<QLabel *>(QString::fromLatin1(key) + "Caption");
                QVERIFY(caption);
                QVERIFY(caption->wordWrap());
                const auto text = caption->fontMetrics().boundingRect(QRect(0, 0, caption->width(), 1000),
                                                                      Qt::TextWordWrap, caption->text());
                QVERIFY2(text.height() <= caption->height(), qPrintable(caption->text()));
            }
        }
    }
    void settingsDefaultsDraft() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QTRY_VERIFY(settings->isVisible() && settings->isEnabled());
        QVERIFY(click(canvas, settings));
        auto *pages = host.window()->findChild<QStackedWidget *>("rrSettingsPages");
        auto *progress = host.window()->findChild<QCheckBox *>("rrShowProgress");
        QVERIFY(pages);
        QVERIFY(progress);
        QTRY_VERIFY(progress->isVisible());
        auto *dense = host.window()->findChild<QCheckBox *>("rrDenseGraph");
        QVERIFY(dense);
        QVERIFY(!dense->isChecked());
        // A settings draft must not reach storage when opening or keeping the restore dialog.
        const QString settingsPath = testPaths().preferences();
        const auto saved = storage::readJson(settingsPath);
        if (progress->isChecked())
            QVERIFY(click(canvas, progress));
        QVERIFY(!progress->isChecked());
        auto *advanced = host.window()->findChild<QPushButton *>("rrSettingsAdvanced");
        QVERIFY(advanced);
        QVERIFY(click(canvas, advanced));
        auto *restore = host.window()->findChild<QPushButton *>("rrRestoreDefaults");
        QVERIFY(restore);
        QTRY_VERIFY(restore->isVisible());
        QVERIFY(click(canvas, restore));
        QCOMPARE(pages->currentIndex(), int(reader::AdvancedSettings));
        QVERIFY(host.window()->findChild<QWidget *>("rrRestoreConfirmation")->isVisible());
        QVERIFY(!click(canvas, restore));
        QCOMPARE(pages->currentIndex(), int(reader::AdvancedSettings));
        QCOMPARE(storage::readJson(settingsPath), saved);
        auto *keep = host.window()->findChild<QPushButton *>("rrKeepDefaults");
        QVERIFY(keep);
        QVERIFY(click(canvas, keep));
        QCOMPARE(pages->currentIndex(), int(reader::AdvancedSettings));
        QVERIFY(!progress->isChecked());
        QVERIFY(restore->isEnabled());
        QCOMPARE(storage::readJson(settingsPath), saved);
        QVERIFY(click(canvas, restore));
        auto *confirm = host.window()->findChild<QPushButton *>("rrConfirmDefaults");
        QVERIFY(confirm);
        QVERIFY(click(canvas, confirm));
        QCOMPARE(pages->currentIndex(), int(reader::AdvancedSettings));
        QVERIFY(progress->isChecked());
        QVERIFY(!dense->isChecked());
        QVERIFY(!restore->isEnabled());
        QCOMPARE(storage::readJson(settingsPath), saved);
        QVERIFY(click(canvas, settings));
        auto *panel = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(panel);
        QTRY_VERIFY_WITH_TIMEOUT(!panel->isVisible() && !host.session()->busy(), 10000);
        QVERIFY(storage::readJson(settingsPath).value("show_progress_bar").toBool());
        host.shutdown();
        QTRY_VERIFY(host.idle());
    }
    void rotationLabelsFollowAvailableWidth() {
        QWidget owner;
        auto *typography = new QWidget(&owner), *layout = new QWidget(&owner);
        auto *typeRows = new QVBoxLayout(typography), *pageRows = new QVBoxLayout(layout);
        reader::AppearancePanelInputs inputs;
        inputs.automaticRotationAvailable = true;
        reader::AppearancePanel panel({typography, layout, typeRows, pageRows}, inputs);
        auto *frame = panel.autoRotateOption->parentWidget();
        auto buttons = panel.rotationButtons;
        buttons.append(panel.autoRotateOption);
        const QStringList labels = {strings::get("reader.portrait"), strings::get("reader.landscape"),
                                    strings::get("reader.both")};
        QEvent show(QEvent::Show);
        frame->resize(1000, 72);
        QApplication::sendEvent(frame, &show);
        for (int index = 0; index < buttons.size(); ++index) {
            QCOMPARE(buttons[index]->text(), labels[index]);
        }
        const auto iconSize = panel.autoRotateOption->iconSize();
        frame->resize(360, 72);
        QApplication::sendEvent(frame, &show);
        for (auto *button : buttons) {
            QVERIFY(button->text().isEmpty());
            QVERIFY(!button->icon().isNull());
            QVERIFY(!button->accessibleName().isEmpty());
            QCOMPARE(button->iconSize(), iconSize);
        }
        frame->resize(1000, 72);
        QApplication::sendEvent(frame, &show);
        for (int index = 0; index < buttons.size(); ++index) {
            QCOMPARE(buttons[index]->text(), labels[index]);
        }
    }

    void sensorlessRotationKeepsManualControls() {
        QWidget owner;
        auto *typography = new QWidget(&owner), *layout = new QWidget(&owner);
        auto *typeRows = new QVBoxLayout(typography), *pageRows = new QVBoxLayout(layout);
        reader::AppearancePanelInputs inputs;
        inputs.reverseLandscapeButtons = true;
        reader::AppearancePanel panel({typography, layout, typeRows, pageRows}, inputs);
        QCOMPARE(panel.rotationButtons.size(), 4);
        const QList<int> orientations = {1, 8, 4, 2};
        for (int i = 0; i < orientations.size(); ++i) {
            QCOMPARE(panel.rotationButtons[i]->property("orientation").toInt(), orientations[i]);
        }
        QVERIFY(panel.autoRotateOption->isHidden());
    }

    void missingAdobeTemplateKeepsBookReadable() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        const auto book =
            qEnvironmentVariable("KOBALT_TEST_EPUB", qEnvironmentVariable("KOBALT_FIXTURES") + "/adobe-template.epub");
        host.open(book, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QVERIFY(!host.session()->formattingWarning());
        QVERIFY(!host.session()->image().isNull());
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *warning = host.window()->findChild<QPushButton *>("rrFormattingWarning");
        QVERIFY(warning && warning->isHidden());
        const int page = host.session()->page();
        QVERIFY(host.session()->turn(1));
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() > page);
        QVERIFY(!host.session()->formattingWarning());
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(book, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QVERIFY(!host.session()->formattingWarning());
        host.shutdown();
        QTRY_VERIFY(host.idle());
    }
    void formattingInspectionKeepsBookReadable_data() {
        QTest::addColumn<QString>("bookName");
        QTest::addColumn<bool>("expectedWarning");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("ppi");
        for (const auto &bookName :
             {QStringLiteral("formatting-issues.epub"), QStringLiteral("formatting-entity.epub"),
              QStringLiteral("formatting-resource-budget.epub"), QStringLiteral("formatting-total-budget.epub"),
              QStringLiteral("formatting-unsupported-writing.epub")}) {
            for (bool dark : {false, true}) {
                for (int ppi : {212, 300}) {
                    const auto name = QString("%1-%2-%3").arg(bookName).arg(dark ? "dark" : "light").arg(ppi);
                    QTest::newRow(qPrintable(name))
                        << bookName << (bookName == "formatting-unsupported-writing.epub") << dark << ppi;
                }
            }
        }
    }
    void formattingInspectionKeepsBookReadable() {
        QFETCH(QString, bookName);
        QFETCH(bool, expectedWarning);
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        const auto previousDensity = ui::density();
        const auto previousPpi = readingPpi.load();
        struct Restore {
            int density, ppi;
            ~Restore() {
                ui::setDensity(density);
                readingPpi.store(ppi);
            }
        } restore{previousDensity, previousPpi};
        ui::setDensity(ppi);
        readingPpi.store(ppi);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        QVERIFY(storage::writeJson(paths.preferences(), {{"dark_mode", dark}}));
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        const auto book = qEnvironmentVariable("KOBALT_FIXTURES") + "/" + bookName;
        host.open(book, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QCOMPARE(host.session()->formattingWarning(), expectedWarning);
        QVERIFY(!host.session()->image().isNull());
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *warning = host.window()->findChild<QPushButton *>("rrFormattingWarning");
        auto *bookmarks = host.window()->findChild<QPushButton *>("rrBookmarks");
        QVERIFY(warning && bookmarks);
        QCOMPARE(warning->isVisible(), expectedWarning);
        const auto captureName = QString("%1-%2-%3").arg(bookName).arg(dark ? "dark" : "light").arg(ppi);
        QVERIFY(canvas.capture().save("native-reader-warning-menu-" + captureName + ".png"));
        if (expectedWarning) {
            QCOMPARE(warning->size(), bookmarks->size());
            QVERIFY(warning->geometry().right() < bookmarks->geometry().left());
            QVERIFY(click(canvas, warning));
            auto *notice = host.window()->findChild<QWidget *>("rrFormattingWarningDialog");
            QVERIFY(notice && notice->isVisible());
            QVERIFY(canvas.capture().save("native-reader-warning-popup-" + captureName + ".png"));
            QVERIFY(click(canvas, notice->findChild<QPushButton *>("rrFormattingWarningConfirm")));
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrFormattingWarningDialog") == nullptr);
        }
        const int page = host.session()->page();
        QVERIFY(host.session()->turn(1));
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() > page);
        QCOMPARE(host.session()->formattingWarning(), expectedWarning);
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(book, nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QCOMPARE(host.session()->formattingWarning(), expectedWarning);
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 3, 30000);
        QVERIFY(!host.session()->formattingWarning());
        QVERIFY(host.window()->findChild<QPushButton *>("rrFormattingWarning")->isHidden());
        host.shutdown();
        QTRY_VERIFY(host.idle());
    }
    void systemRotationHidesManualControls() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.configureServices = [](ReaderHostServices &services) {
            services.systemRotation = true;
        };
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QTRY_VERIFY(host.window()->readyForPresentation());
        QVERIFY(!host.window()->findChild<QWidget *>("rrRotation"));
        QVERIFY(!host.window()->findChild<QPushButton *>("rrAutoRotate"));
        QVERIFY(host.window()->findChild<QWidget *>("rrHeaderTextSize"));
        QVERIFY(host.window()->findChild<QWidget *>("rrFooterTextSize"));
    }
    void openingWaitsForTheFinalViewport() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        auto *reader = host.window();
        QVERIFY(reader);
        QVERIFY(!reader->readyForPresentation());
        reader->setWindowFlags(Qt::Widget);
        reader->setWindowModality(Qt::NonModal);
        QWidget window;
        window.resize(400, 800);
        window.setContentsMargins(0, 44, 0, 24);
        auto *layout = new QStackedLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *loading = new QWidget;
        QPalette colors;
        colors.setColor(QPalette::Window, Qt::red);
        loading->setPalette(colors);
        loading->setAutoFillBackground(true);
        layout->addWidget(loading);
        auto *canvas = new android::WidgetCanvas(&window);
        layout->addWidget(canvas);
        canvas->viewportResized = [canvas] {
            canvas->fitContent();
        };
        layout->setStackingMode(QStackedLayout::StackAll);
        canvas->setContent(reader);
        window.show();
        canvas->fitContent();
        QTRY_VERIFY_WITH_TIMEOUT(reader->readyForPresentation(), 10000);
        QCOMPARE(window.grab().toImage().pixelColor(200, 300), QColor(Qt::red));
        const auto previous = host.session()->image().size();
        window.setContentsMargins(0, 0, 0, 0);
        layout->activate();
        canvas->fitContent();
        QVERIFY(!reader->readyForPresentation());
        QTRY_VERIFY_WITH_TIMEOUT(reader->readyForPresentation(), 10000);
        QVERIFY(host.session()->image().size() != previous);
        auto *page = static_cast<ReadingPage *>(reader->findChild<QWidget *>("rrPage"));
        QVERIFY(page);
        QCOMPARE(page->image.size(),
                 QSize(reader->width(), reader->height() - page->topHeight() - page->bottomHeight()));
        QCOMPARE(window.grab().toImage().pixelColor(200, 300), QColor(Qt::red));
        layout->setCurrentWidget(canvas);
        layout->setStackingMode(QStackedLayout::StackOne);
        QVERIFY(!loading->isVisible());
        QVERIFY(reader->readyForPresentation());
    }
    void unchangedReaderStateDoesNotRepaint_data() {
        QTest::addColumn<QString>("panel");
        QTest::newRow("reading") << QString();
        QTest::newRow("controls") << QStringLiteral("controls");
        QTest::newRow("stats") << QStringLiteral("rrNavStats");
        QTest::newRow("appearance") << QStringLiteral("rrNavText");
        QTest::newRow("layout") << QStringLiteral("rrNavLayout");
        QTest::newRow("settings") << QStringLiteral("rrSettings");
        QTest::newRow("chapters") << QStringLiteral("rrNavChapters");
        QTest::newRow("font-picker") << QStringLiteral("font-picker");
    }
    void unchangedReaderStateDoesNotRepaint() {
        QFETCH(QString, panel);
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *readingPage = host.window()->findChild<ReadingPage *>("rrPage");
        auto *appearancePanel = host.window()->findChild<QWidget *>("rrMenu");
        QVERIFY(readingPage);
        QVERIFY(appearancePanel);
        const auto before = readingPage->image.cacheKey();
        PanelPlacement placement;
        appearancePanel->installEventFilter(&placement);
        auto *graphPanel = host.window()->findChild<QWidget *>("rrChapterGraph");
        QVERIFY(graphPanel);
        PanelPlacement graphPlacement;
        graphPanel->installEventFilter(&graphPlacement);
        if (!panel.isEmpty()) {
            QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
            if (panel != "controls") {
                auto *button = host.window()->findChild<QPushButton *>(panel == "font-picker" ? "rrNavText" : panel);
                QVERIFY(button);
                QVERIFY(click(canvas, button));
            }
            QVERIFY(!host.session()->busy());
            if (panel == "font-picker") {
                auto *button = host.window()->findChild<QPushButton *>("rrFont");
                QVERIFY(button);
                QVERIFY(click(canvas, button));
            }
        }
        QCoreApplication::processEvents();
        if (panel == "rrNavText" || panel == "rrNavLayout") {
            QCOMPARE(placement.shown, appearancePanel->geometry());
            QCOMPARE(placement.visibleMoves, 0);
            QCOMPARE(readingPage->image.cacheKey(), before);
        }
        if (panel == "rrNavStats") {
            QCOMPARE(graphPlacement.shown, graphPanel->geometry());
            QCOMPARE(graphPlacement.visibleMoves, 0);
        }
        PaintRegions paints;
        for (auto *widget : host.window()->findChildren<QWidget *>()) {
            widget->installEventFilter(&paints);
        }
        host.window()->installEventFilter(&paints);
        QCoreApplication::processEvents();
        paints.count = 0;
        paints.widgets.clear();
        QVERIFY(bool(host.session()->updated));
        host.session()->updated();
        QCoreApplication::processEvents();
        if (paints.count) {
            qWarning() << paints.widgets;
        }
        QCOMPARE(paints.count, 0);
    }
    void uneditedPanelTransitions_data() {
        QTest::addColumn<QString>("start");
        QTest::newRow("text") << "rrNavText";
        QTest::newRow("layout") << "rrNavLayout";
        QTest::newRow("light") << "rrNavLight";
        QTest::newRow("stats") << "rrNavStats";
        QTest::newRow("chapters") << "rrNavChapters";
    }
    void uneditedPanelTransitions() {
        QFETCH(QString, start);
        if (start == "rrNavLight" && !test::fixtureDeviceControls())
            QSKIP("Frontlight requires the device fixture");
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        auto *toolbar = host.window()->findChild<QWidget *>("rrControlsTop");
        QVERIFY(page);
        QVERIFY(toolbar);
        const auto image = page->image.cacheKey();
        auto state = host.session()->state();
        state.remove("cached_pages");
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(toolbar->isVisible());

        const QStringList tabs = {"rrNavText", "rrNavLayout", "rrNavLight", "rrNavStats", "rrNavChapters"};
        const QStringList panels = {"rrMenu", "rrMenu", "rrLightPanel", "rrChapterGraph", "rrChoices"};
        auto *startButton = host.window()->findChild<QPushButton *>(start);
        QVERIFY(startButton);
        QVERIFY(click(canvas, startButton));
        const int startIndex = tabs.indexOf(start);
        QVERIFY(startIndex >= 0);
        auto *startPanel = host.window()->findChild<QWidget *>(panels[startIndex]);
        QVERIFY(startPanel);
        QTRY_VERIFY(startPanel->isVisible());
        for (int index = 0; index < tabs.size(); ++index) {
            if (tabs[index] == "rrNavLight" && !test::fixtureDeviceControls())
                continue;
            auto *button = host.window()->findChild<QPushButton *>(tabs[index]);
            auto *destination = host.window()->findChild<QWidget *>(panels[index]);
            QVERIFY(button);
            QVERIFY(destination);
            QVERIFY2(button->isVisible() && button->isEnabled(), qPrintable(tabs[index]));
            QVERIFY(click(canvas, button));
            QVERIFY2(!host.session()->busy(), qPrintable(tabs[index]));
            if (tabs[index] == start) {
                QTRY_VERIFY(!startPanel->isVisible());
            } else {
                QTRY_VERIFY(destination->isVisible());
                if (tabs[index] != "rrNavText" && tabs[index] != "rrNavLayout")
                    QVERIFY(!startPanel->isVisible());
            }
            QTRY_VERIFY(!host.session()->busy());
            auto current = host.session()->state();
            current.remove("cached_pages");
            QCOMPARE(current, state);
            QCOMPARE(page->image.cacheKey(), image);
            QVERIFY(toolbar->isVisible());
            QVERIFY2(startButton->isVisible() && startButton->isEnabled(), qPrintable(start));
            QVERIFY(click(canvas, startButton));
            QVERIFY2(!host.session()->busy(), qPrintable(start));
            QTRY_VERIFY(startPanel->isVisible());
            QVERIFY(!host.session()->busy());
            QCOMPARE(page->image.cacheKey(), image);
            QVERIFY(toolbar->isVisible());
        }
        if (start == "rrNavText") {
            auto *font = host.window()->findChild<QPushButton *>("rrFont");
            auto *choices = host.window()->findChild<QWidget *>("rrChoices");
            QVERIFY(font);
            QVERIFY(choices);
            QVERIFY(click(canvas, font));
            QTRY_VERIFY(choices->isVisible());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
            QTRY_VERIFY(startPanel->isVisible() && !choices->isVisible());
            QCOMPARE(page->image.cacheKey(), image);
            QVERIFY(toolbar->isVisible());
        }
    }
    void appearanceApplyOnNavigation_data() {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("panel");
        QTest::newRow("stats") << "rrNavStats" << "rrChapterGraph";
        QTest::newRow("chapters") << "rrNavChapters" << "rrChoices";
        QTest::newRow("light") << "rrNavLight" << "rrLightPanel";
        QTest::newRow("search") << "rrNavSearch" << "rrSearch";
        QTest::newRow("bookmarks") << "rrBookmarks" << "rrBookmarkList";
    }
    void appearanceApplyOnNavigation() {
        QFETCH(QString, action);
        QFETCH(QString, panel);
        if (action == "rrNavLight" && !test::fixtureDeviceControls())
            QSKIP("Frontlight requires the device fixture");
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        QVERIFY(!host.session()->busy());
        auto *larger = sizeStep(host);
        QVERIFY(larger);
        QTRY_VERIFY(larger->isVisible() && larger->isEnabled());
        QVERIFY(click(canvas, larger));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        const auto textDraft = inspectReader(host)["preview_settings"].toObject();
        QVERIFY(textDraft["font_pixels"] != original["font_pixels"]);
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
        auto *layout = host.window()->findChild<QPushButton *>("rrNavLayout");
        QVERIFY(layout);
        QVERIFY(click(canvas, layout));
        auto *wider = host.window()->findChild<QPushButton *>("rrWider");
        QVERIFY(wider);
        QTRY_VERIFY(wider->isVisible() && wider->isEnabled());
        for (const auto *name : {"rrMargins", "rrVerticalMargins"}) {
            auto *slider = host.window()->findChild<QSlider *>(name);
            QVERIFY(slider);
            QCOMPARE(slider->minimum(), 0);
            QCOMPARE(slider->maximum(), 120);
            QCOMPARE(slider->singleStep(), 3);
        }
        QCOMPARE(inspectReader(host)["preview_settings"].toObject()["font_pixels"], textDraft["font_pixels"]);
        QVERIFY(click(canvas, wider));
        QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool());
        auto *verticalMarginsMore = host.window()->findChild<QPushButton *>("rrVerticalMarginsMore");
        QVERIFY(verticalMarginsMore);
        QVERIFY(click(canvas, verticalMarginsMore));
        QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool());
        const auto draft = inspectReader(host)["preview_settings"].toObject();
        QVERIFY(draft["margin_pixels"] != original["margin_pixels"]);
        QVERIFY(draft["top_margin_pixels"] != original["top_margin_pixels"]);
        QVERIFY(draft["bottom_margin_pixels"] != original["bottom_margin_pixels"]);
        QCOMPARE(draft["top_margin_pixels"], draft["bottom_margin_pixels"]);
        QCOMPARE(host.session()->state()["top_margin_pixels"], original["top_margin_pixels"]);
        QCOMPARE(host.session()->state()["bottom_margin_pixels"], original["bottom_margin_pixels"]);
        auto *destination = host.window()->findChild<QPushButton *>(action);
        QVERIFY(destination);
        QTRY_VERIFY(destination->isEnabled());
        QVERIFY(click(canvas, destination));
        auto *target = host.window()->findChild<QWidget *>(panel);
        QVERIFY(target);
        QTRY_VERIFY_WITH_TIMEOUT(target->isVisible() && !host.session()->busy(), 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
        QCOMPARE(host.session()->state()["margin_pixels"], draft["margin_pixels"]);
        QCOMPARE(host.session()->state()["top_margin_pixels"], draft["top_margin_pixels"]);
        QCOMPARE(host.session()->state()["bottom_margin_pixels"], draft["bottom_margin_pixels"]);
        QVERIFY(!inspectReader(host)["preview_ready"].toBool());
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
        QCOMPARE(host.session()->state()["margin_pixels"], draft["margin_pixels"]);
        QCOMPARE(host.session()->state()["top_margin_pixels"], draft["top_margin_pixels"]);
        QCOMPARE(host.session()->state()["bottom_margin_pixels"], draft["bottom_margin_pixels"]);
    }
    void appearanceRelayoutPreservesPassage() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto paths = testPaths();
        paths.directory = directory.path();
        paths.startup = paths.directory + "/startup-session";
        paths.active = paths.directory + "/active-session.json";
        paths.stateReport = paths.directory + "/state.json";
        paths.sessionReport = paths.directory + "/session.json";
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto inspect = [&] {
            host.window()->checkpoint();
            return storage::readJson(paths.stateReport);
        };
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        const auto first = host.session()->state().value("position").toString();
        const bool rtl = host.session()->state().value("page_progression_rtl").toBool();
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), QPoint(rtl ? page->width() / 6 : page->width() * 5 / 6,
                                                                     page->height() / 2))));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state().value("position").toString() != first, 10000);
        QVERIFY(host.session()->page() > 0);
        const auto anchor = host.session()->state().value("position").toString();
        QVERIFY(!anchor.isEmpty());
        const auto menu = [&] {
            if (!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible())
                QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->rect().center())));
        };
        const auto preserved = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy(), 30000);
            // Reflow restores the canonical bookmark, rather than the new page start.
            QCOMPARE(host.session()->state().value("position").toString(), anchor);
        };
        const auto settingsToggle = [&](const char *name, const char *key) {
            menu();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            auto *option = host.window()->findChild<QCheckBox *>(name);
            QVERIFY(option);
            const bool before = option->isChecked();
            QVERIFY(click(canvas, option));
            QCOMPARE(option->isChecked(), !before);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            QTRY_VERIFY_WITH_TIMEOUT(!host.window()->findChild<QWidget *>("rrGeneralSettings")->isVisible() &&
                                         !host.session()->busy() &&
                                         host.session()->state().value(key).toBool() == !before,
                                     30000);
            preserved();
            QCOMPARE(host.session()->state().value(key).toBool(), !before);
        };
        // Weight controls are disabled while Bionic Reading is active.
        if (host.session()->state().value("bold_reading").toString() != "off") {
            menu();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            auto *option = host.window()->findChild<QCheckBox *>("rrBionicReading");
            QVERIFY(option && option->isChecked());
            QVERIFY(click(canvas, option));
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            QTRY_VERIFY_WITH_TIMEOUT(
                !host.session()->busy() && host.session()->state().value("bold_reading").toString() == "off", 30000);
            preserved();
            QCOMPARE(host.session()->state().value("bold_reading").toString(), QString("off"));
        }
        const auto openText = [&] {
            menu();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
            QVERIFY(!host.session()->busy());
        };
        const auto apply = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(!inspect()["preview_busy"].toBool(), 30000);
            const auto expected = inspect()["preview_settings"].toObject();
            QVERIFY(!expected.isEmpty());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavStats")));
            auto *graph = host.window()->findChild<QWidget *>("rrChapterGraph");
            QVERIFY(graph);
            QTRY_VERIFY_WITH_TIMEOUT(
                graph->isVisible() && !host.session()->busy() && !inspect()["preview_ready"].toBool(), 30000);
            for (const auto *key : {"font_face", "font_pixels", "weight_steps", "margin_pixels"})
                QCOMPARE(host.session()->state().value(key), expected.value(key));
            preserved();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavStats")));
        };
        openText();
        const auto oldFace = host.session()->state().value("font_face");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
        const QString targetFace =
            oldFace.toString() == "Libron" ? kobalt::test::alternateReadingFace() : QString("Libron");
        const QString targetLabel =
            oldFace.toString() == "Libron" ? kobalt::test::alternateReadingLabel() : QString("Libron");
        QPushButton *choice = nullptr;
        for (auto *label : host.window()->findChildren<QLabel *>())
            if (label->objectName().endsWith("Title") && label->text() == targetLabel)
                choice = qobject_cast<QPushButton *>(label->parentWidget());
        QVERIFY(choice);
        QVERIFY(choice->isVisible());
        QVERIFY(click(canvas, choice));
        QTRY_COMPARE_WITH_TIMEOUT(inspect()["preview_settings"].toObject().value("font_face").toString(), targetFace,
                                  30000);
        QTRY_VERIFY_WITH_TIMEOUT(!inspect()["preview_busy"].toBool(), 30000);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
        QTRY_VERIFY(host.window()->findChild<QSlider *>("rrTextSize")->isVisible());
        apply();
        QCOMPARE(host.session()->state().value("font_face").toString(), targetFace);
        QVERIFY(host.session()->state().value("font_face") != oldFace);
        openText();
        const auto oldPixels = host.session()->state().value("font_pixels");
        QVERIFY(click(canvas, sizeStep(host)));
        apply();
        QVERIFY(host.session()->state().value("font_pixels") != oldPixels);
        openText();
        auto *weight = host.window()->findChild<QSlider *>("rrFontWeight");
        QVERIFY(weight);
        const auto oldWeight = host.session()->state().value("weight_steps");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>(
                                  weight->value() == weight->maximum() ? "rrLighter" : "rrHeavier")));
        apply();
        QVERIFY(host.session()->state().value("weight_steps") != oldWeight);
        menu();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavLayout")));
        QVERIFY(!host.session()->busy());
        auto *margin = host.window()->findChild<QSlider *>("rrMargins");
        QVERIFY(margin);
        const auto oldMargin = host.session()->state().value("margin_pixels");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>(
                                  margin->value() == margin->maximum() ? "rrNarrower" : "rrWider")));
        apply();
        QVERIFY(host.session()->state().value("margin_pixels") != oldMargin);
        menu();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        auto *bionic = host.window()->findChild<QCheckBox *>("rrBionicReading");
        QVERIFY(bionic && !bionic->isChecked());
        QVERIFY(click(canvas, bionic));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state().value("bold_reading").toString() == "bold", 30000);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        preserved();
        QCOMPARE(host.session()->state().value("bold_reading").toString(), QString("bold"));
        settingsToggle("rrDarkMode", "dark_mode");
        settingsToggle("rrDarkMode", "dark_mode");
        for (const auto &size : {QSize(800, 1200), QSize(1400, 1000)}) {
            host.window()->setFixedSize(size);
            QTRY_COMPARE_WITH_TIMEOUT(inspect().value("viewport_width").toInt(), size.width(), 10000);
            preserved();
        }
    }
    void fontDefaultPersistence() {
        const auto originalPreferences = storage::readJson(testPaths().preferences());
        QVERIFY(storage::writeJson(testPaths().preferences(), originalPreferences));
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
        QVERIFY(!host.session()->busy());
        const QJsonValue selected = host.session()->state().value("font_face");
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontDefaultSlot")));
        auto *ok = host.window()->findChild<QPushButton *>("rrFontDefaultConfirm");
        QVERIFY(ok);
        QCOMPARE(ok->text(), QString("OK"));
        QVERIFY(click(canvas, ok));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrFontDefaultDialog"));
        QPushButton *candidate = nullptr;
        for (auto *label : host.window()->findChildren<QLabel *>())
            if (label->objectName().endsWith("Title") && label->text() == kobalt::test::alternateReadingLabel())
                candidate = qobject_cast<QPushButton *>(label->parentWidget());
        QVERIFY(candidate);
        QVERIFY(candidate->isVisible());
        canvas.ensureWidgetVisible(candidate);
        QCoreApplication::processEvents();
        const auto point = candidate->mapTo(&canvas, candidate->rect().center());
        QTest::mousePress(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrFontDefaultDialog"));
        QTest::mouseRelease(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(host.session()->state()["font_face"], selected);
        QVERIFY(!host.window()->findChild<QPushButton *>("rrFontDefaultSans"));
        QVERIFY(!host.window()->findChild<QPushButton *>("rrFontDefaultMonospace"));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontDefaultCancel")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrFontDefaultDialog"));
        QVERIFY(!storage::readJson(testPaths().preferences()).contains("default_font_face"));
        QContextMenuEvent event(QContextMenuEvent::Keyboard, candidate->rect().center());
        QApplication::sendEvent(candidate, &event);
        {
            BlockedFile blocked(testPaths().preferences());
            QVERIFY(blocked.active());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontDefaultConfirm")));
            auto *message = host.window()->findChild<QLabel *>("rrFontDefaultMessage");
            QVERIFY(message);
            QCOMPARE(message->text(), strings::get("reader.could_not_save_reader_settings"));
            auto *notice = host.window()->findChild<QWidget *>("rrSettingsWriteError");
            QVERIFY(notice && notice->isVisible());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
            QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSettingsWriteError"));
            const auto current = inspectReader(host)["preview_ready"].toBool()
                                     ? inspectReader(host)["preview_settings"].toObject()
                                     : host.session()->state();
            QCOMPARE(current["default_font_face"].toString(), QString("Libron"));
            QVERIFY(blocked.restore());
        }
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontDefaultConfirm")));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        QCOMPARE(storage::readJson(testPaths().preferences())["default_font_face"].toString(),
                 kobalt::test::alternateReadingFace());
        QCOMPARE(host.session()->state()["font_face"], selected);
        auto *help = host.window()->findChild<QPushButton *>("rrFontDefaultSlot");
        QVERIFY(help);
        QVERIFY(help->isVisible());
        auto *helpSample = help->findChild<QLabel *>("rrFontDefaultSlotTitle");
        QVERIFY(helpSample);
        QCOMPARE(helpSample->text(), QString("Aa"));
        QCOMPARE(QFontInfo(helpSample->font()).family(), kobalt::test::alternateReadingFace());
        QLabel *defaultCaption = nullptr;
        for (auto *label : host.window()->findChildren<QLabel *>()) {
            if (label->objectName().endsWith("Title") && label->text() == kobalt::test::alternateReadingLabel())
                defaultCaption = label->parentWidget()->findChild<QLabel *>(label->objectName().chopped(5) + "Detail");
        }
        QVERIFY(defaultCaption);
        QCOMPARE(defaultCaption->text(), QString("DEFAULT FONT"));
        for (auto *label : host.window()->findChildren<QLabel *>()) {
            if (label->objectName().endsWith("Title") && label->text() == "Libron") {
                auto *caption = label->parentWidget()->findChild<QLabel *>(label->objectName().chopped(5) + "Detail");
                QVERIFY(caption);
                QVERIFY(caption->text().isEmpty());
            }
        }
        QVERIFY(defaultCaption->parentWidget()->y() > help->y());
        const auto preferencesBeforeHelp = storage::readJson(testPaths().preferences());
        const QJsonValue previewBeforeHelp = inspectReader(host)["preview_settings"];
        QVERIFY(click(canvas, help));
        auto *helpOk = host.window()->findChild<QPushButton *>("rrFontDefaultConfirm");
        QVERIFY(helpOk);
        QCOMPARE(helpOk->text(), QString("OK"));
        QVERIFY(click(canvas, helpOk));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrFontDefaultDialog"));
        QCOMPARE(storage::readJson(testPaths().preferences()), preferencesBeforeHelp);
        QCOMPARE(inspectReader(host)["preview_settings"], previewBeforeHelp);
        auto *detail = host.window()->findChild<QLabel *>("rrFontChoice0Detail");
        QVERIFY(detail);
        QVERIFY(detail->text().contains("USES LIBRON"));
        QCOMPARE(detail->font().pixelSize(), ui::px(18));
        QCOMPARE(QFontInfo(detail->font()).family(), QFontInfo(ui::captionFont(ui::px(18))).family());
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrBookmarks")));
        QTRY_VERIFY(!host.session()->busy() && !inspectReader(host)["preview_ready"].toBool());
        QCOMPARE(host.session()->state()["default_font_face"].toString(), kobalt::test::alternateReadingFace());
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QCOMPARE(host.session()->state()["default_font_face"].toString(), kobalt::test::alternateReadingFace());
        QCOMPARE(host.session()->state()["font_face"], selected);
        QVERIFY(storage::writeJson(testPaths().preferences(), originalPreferences));
    }
    void overflowClosesCurrentPanel_data() {
        QTest::addColumn<QString>("target");
        for (const auto *target : {"text", "text-after-fonts", "fonts", "layout", "light", "stats"})
            QTest::newRow(target) << QString::fromLatin1(target);
    }
    void overflowClosesCurrentPanel() {
        QFETCH(QString, target);
        if (target == "light" && !test::fixtureDeviceControls())
            QSKIP("Frontlight requires the device fixture");
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        const auto action = target == "layout"  ? "rrNavLayout"
                            : target == "light" ? "rrNavLight"
                            : target == "stats" ? "rrNavStats"
                                                : "rrNavText";
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>(action)));
        QVERIFY(!host.session()->busy());
        if (target == "fonts" || target == "text-after-fonts") {
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrChoices")->isVisible());
            if (target == "text-after-fonts")
                QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
        }
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrGeneralSettings")->isVisible());
        for (const auto *name : {"rrMenu", "rrChoices", "rrLightPanel", "rrChapterGraph", "rrBookmarkList", "rrSearch",
                                 "rrControlsBottom"}) {
            auto *panel = host.window()->findChild<QWidget *>(name);
            QVERIFY(panel);
            QVERIFY2(!panel->isVisible(), name);
        }
        QVERIFY(!host.window()->findChild<QPushButton *>("rrNavText")->isChecked());
        QVERIFY(!host.window()->findChild<QPushButton *>("rrNavLayout")->isChecked());
    }
    void appearanceSettingsRetainsDraft() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        QVERIFY(!host.session()->busy());
        auto *larger = sizeStep(host);
        QVERIFY(larger);
        QTRY_VERIFY(larger->isEnabled());
        QVERIFY(click(canvas, larger));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        const auto draft = inspectReader(host)["preview_settings"].toObject();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QVERIFY(click(canvas, settings));
        auto *panel = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrMenu")->isVisible());
        QCOMPARE(inspectReader(host)["preview_settings"].toObject(), draft);
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
        QVERIFY(click(canvas, text));
        QTRY_VERIFY_WITH_TIMEOUT(larger->isVisible() && larger->isEnabled() && !host.session()->busy(), 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
    }
    void appearanceWriteFailureRetainsDraft_data() {
        QTest::addColumn<bool>("exit");
        QTest::newRow("navigation-retry") << false;
        QTest::newRow("exit-retry") << true;
    }
    void appearanceWriteFailureRetainsDraft() {
        QFETCH(bool, exit);
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *layout = host.window()->findChild<QPushButton *>("rrNavLayout");
        QVERIFY(layout);
        QVERIFY(click(canvas, layout));
        auto *wider = host.window()->findChild<QPushButton *>("rrWider");
        QVERIFY(wider);
        QTRY_VERIFY(wider->isEnabled());
        QVERIFY(click(canvas, wider));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        const auto draft = inspectReader(host)["preview_settings"].toObject();
        auto *clockOff = host.window()->findChild<QPushButton *>("rrHideClock");
        auto *clockOn = host.window()->findChild<QPushButton *>("rrShowClock");
        QVERIFY(clockOff);
        QVERIFY(clockOn);
        QVERIFY(click(canvas, clockOff->isChecked() ? clockOn : clockOff));
        const QString path = testPaths().preferences();
        QFile savedFile(path);
        const bool existed = savedFile.exists();
        QByteArray savedBytes;
        if (existed) {
            QVERIFY(savedFile.open(QIODevice::ReadOnly));
            savedBytes = savedFile.readAll();
            savedFile.close();
            QVERIFY(savedFile.remove());
        }
        QVERIFY(QDir().mkpath(path));
        struct RestoreFile {
            QString path;
            bool existed;
            QByteArray bytes;
            ~RestoreFile() {
                if (QFileInfo(path).isDir())
                    QDir(path).removeRecursively();
                else
                    QFile::remove(path);
                if (existed) {
                    QFile file(path);
                    if (file.open(QIODevice::WriteOnly))
                        file.write(bytes);
                }
            }
        } restore{path, existed, savedBytes};
        auto *search = host.window()->findChild<QPushButton *>("rrNavSearch");
        QVERIFY(search);
        QVERIFY(click(canvas, search));
        auto *error = host.window()->findChild<QLabel *>("rrPreviewLabel");
        QVERIFY(error);
        QTRY_VERIFY(error->isVisible() && !error->text().isEmpty());
        QVERIFY(inspectReader(host)["preview_ready"].toBool());
        QCOMPARE(inspectReader(host)["preview_settings"].toObject(), draft);
        QCOMPARE(host.session()->state()["margin_pixels"], original["margin_pixels"]);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSearch")->isVisible());
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSettingsWriteError"));
        if (exit) {
            host.window()->close();
            QVERIFY(!host.idle());
            QVERIFY(host.window()->isEnabled());
            QVERIFY(inspectReader(host)["preview_ready"].toBool());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
            QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSettingsWriteError"));
        }
        QVERIFY(QDir(path).removeRecursively());
        if (exit) {
            QVERIFY(click(canvas, layout));
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
            host.open(fixture(), nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        } else {
            QVERIFY(click(canvas, search));
            QTRY_VERIFY_WITH_TIMEOUT(
                host.window()->findChild<QWidget *>("rrSearch")->isVisible() && !host.session()->busy(), 10000);
        }
        QCOMPARE(host.session()->state()["margin_pixels"], draft["margin_pixels"]);
    }
    void appearanceSleepRetainsDraft() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        QVERIFY(!host.session()->busy());
        auto *larger = sizeStep(host);
        QVERIFY(larger);
        QTRY_VERIFY(larger->isEnabled());
        QVERIFY(click(canvas, larger));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        const auto draft = inspectReader(host)["preview_settings"].toObject();
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
        QCOMPARE(inspectReader(host)["preview_settings"].toObject(), draft);
        auto *search = host.window()->findChild<QPushButton *>("rrNavSearch");
        QVERIFY(search);
        QTRY_VERIFY(search->isVisible());
        QVERIFY(click(canvas, search));
        QTRY_VERIFY_WITH_TIMEOUT(
            host.window()->findChild<QWidget *>("rrSearch")->isVisible() && !host.session()->busy(), 10000);
        QCOMPARE(host.session()->state()["font_pixels"], draft["font_pixels"]);
    }
    void appearanceFailedPreviewDismisses_data() {
        QTest::addColumn<bool>("exit");
        QTest::newRow("navigation") << false;
        QTest::newRow("exit") << true;
    }
    void appearanceFailedPreviewDismisses() {
        QFETCH(bool, exit);
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        // Revoke the real source lease after opening to fail the next preview deterministically.
        host.session()->revokeSource();
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        auto *step = sizeStep(host);
        QVERIFY(step);
        QVERIFY(click(canvas, step));
        auto *error = host.window()->findChild<QLabel *>("rrPreviewLabel");
        QVERIFY(error);
        QTRY_VERIFY_WITH_TIMEOUT(error->isVisible() && !error->text().isEmpty(), 10000);
        QVERIFY(!inspectReader(host)["preview_ready"].toBool());
        if (exit) {
            host.window()->close();
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
        } else {
            auto *search = host.window()->findChild<QPushButton *>("rrNavSearch");
            QVERIFY(search);
            QVERIFY(click(canvas, search));
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrSearch")->isVisible());
            QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
            QVERIFY(!inspectReader(host)["preview_busy"].toBool());
        }
    }
    void appearanceApplyFailureCanExit() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        QVERIFY(click(canvas, text));
        QVERIFY(!host.session()->busy());
        auto *step = sizeStep(host);
        QVERIFY(step);
        QTRY_VERIFY(step->isEnabled());
        QVERIFY(click(canvas, step));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        // Reject a real engine update before Reader observes apply completion.
        // Revoking a source alone cannot fail a reflow of an already cached document.
        const auto productionUpdated = host.session()->updated;
        bool rejectUpdate = true;
        struct RestoreUpdated {
            QPointer<ReaderSession> session;
            std::function<void()> callback;
            ~RestoreUpdated() {
                if (session)
                    session->updated = callback;
            }
        } restore{host.session(), productionUpdated};
        host.session()->updated = [&, productionUpdated] {
            if (rejectUpdate) {
                rejectUpdate = false;
                auto invalid = host.session()->state();
                invalid["font_pixels"] = 0;
                host.session()->apply(invalid);
            } else if (productionUpdated)
                productionUpdated();
        };
        auto *search = host.window()->findChild<QPushButton *>("rrNavSearch");
        QVERIFY(search);
        QVERIFY(click(canvas, search));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy() && !host.session()->error().isEmpty(), 10000);
        QVERIFY(host.window()->isEnabled());
        QVERIFY(!host.window()->findChild<QWidget *>("rrSearch")->isVisible());
        host.window()->close();
        QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
    }
    void appearanceCloseDuringPreview_data() {
        QTest::addColumn<bool>("edit");
        QTest::addColumn<bool>("shutdown");
        QTest::newRow("exit-unedited") << false << false;
        QTest::newRow("exit-reflow") << true << false;
        QTest::newRow("shutdown-unedited") << false << true;
        QTest::newRow("shutdown-reflow") << true << true;
    }
    void appearanceCloseDuringPreview() {
        QFETCH(bool, edit);
        QFETCH(bool, shutdown);
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened), closed(&host, &test::ReaderHost::closed);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        int expectedPixels = original["font_pixels"].toInt();
        if (edit) {
            QVERIFY(click(canvas, text));
            auto *larger = sizeStep(host);
            auto *size = host.window()->findChild<QSlider *>("rrTextSize");
            QVERIFY(larger);
            QVERIFY(size);
            QTRY_VERIFY(larger->isEnabled());
            larger->click();
            if (!shutdown)
                expectedPixels = halfPointPixels(size->value());
        } else
            text->click();
        if (edit)
            QVERIFY(inspectReader(host)["preview_busy"].toBool());
        if (shutdown)
            host.shutdown();
        else
            host.window()->close();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY(host.idle());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QCOMPARE(host.session()->state()["font_pixels"].toInt(), expectedPixels);
    }
    void appearanceSleepDuringFirstEdit() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *text = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(text);
        text->click();
        QVERIFY(!host.session()->busy());
        auto *step = sizeStep(host);
        QVERIFY(step);
        step->click();
        QVERIFY(inspectReader(host)["preview_busy"].toBool());
        host.window()->sleepReader();
        host.window()->sleepViewReady();
        QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool());
        QVERIFY(inspectReader(host)["preview_ready"].toBool());
        host.window()->wakeReader();
        auto *font = host.window()->findChild<QPushButton *>("rrFont");
        QVERIFY(font);
        QTRY_VERIFY(font->isVisible() && font->isEnabled());
        QCOMPARE(host.session()->state()["font_pixels"], original["font_pixels"]);
    }
    // Physical Clara BW reports: a second drag right after the first did
    // nothing, and Overflow ignored taps while the preview rendered.
    void appearanceDragsDuringPreview_data() {
        QTest::addColumn<bool>("slow");
        QTest::newRow("preview-ready") << false;
        // A held engine worker stands in for the Clara BW, where the preview
        // is still opening and rendering while the finger moves.
        QTest::newRow("preview-rendering") << true;
    }
    void marginReflowDoesNotRepaintRotation_data() {
        QTest::addColumn<QString>("stepName");
        QTest::newRow("horizontal") << "rrWider";
        QTest::newRow("vertical") << "rrVerticalMarginsMore";
    }
    void marginReflowDoesNotRepaintRotation() {
        QFETCH(QString, stepName);
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavLayout")));
        auto *step = host.window()->findChild<QPushButton *>(stepName);
        auto *rotation = host.window()->findChild<QPushButton *>("rrOrientation1");
        QVERIFY(step && rotation);
        QTRY_VERIFY(rotation->isVisible() && rotation->isEnabled());
        step->click();
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool() && !inspectReader(host)["preview_busy"].toBool());
        QCoreApplication::processEvents();
        PaintRegions paints;
        auto *row = rotation->parentWidget();
        row->installEventFilter(&paints);
        for (auto *widget : row->findChildren<QWidget *>())
            widget->installEventFilter(&paints);
        struct Release {
            ~Release() {
                engineActivity().pause(false);
            }
        } release;
        engineActivity().pause(true);
        step->click();
        QTRY_VERIFY(inspectReader(host)["preview_busy"].toBool());
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        for (auto *button : row->findChildren<QPushButton *>()) {
            const bool checked = button->isChecked();
            QSignalSpy activated(button, &QPushButton::clicked);
            QTest::mouseClick(button, Qt::LeftButton);
            QTest::keyClick(button, Qt::Key_Space);
            QCOMPARE(activated.count(), 0);
            QCOMPARE(button->isChecked(), checked);
        }
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        engineActivity().pause(false);
        QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool());
        QCoreApplication::processEvents();
        QCOMPARE(paints.count, 0);
        QSignalSpy activated(rotation, &QPushButton::clicked);
        QTest::mouseClick(rotation, Qt::LeftButton);
        QCOMPARE(activated.count(), 1);
    }
    void appearanceDragsDuringPreview() {
        QFETCH(bool, slow);
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
        auto *weight = host.window()->findChild<QSlider *>("rrFontWeight");
        auto *size = host.window()->findChild<QSlider *>("rrTextSize");
        auto *overflow = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(weight && size && overflow);
        QTRY_VERIFY(weight->isVisible());
        struct Release {
            ~Release() {
                engineActivity().pause(false);
            }
        } release;
        if (slow)
            engineActivity().pause(true);
        // Drag before the preview has opened, then drag again at once.
        QStringList disabled;
        const auto watch = [&] {
            for (auto *widget :
                 {static_cast<QWidget *>(weight), static_cast<QWidget *>(size), static_cast<QWidget *>(overflow)})
                if (!widget->isEnabled() && !disabled.contains(widget->objectName()))
                    disabled.append(widget->objectName());
        };
        const int thickness = weight->value();
        drag(canvas, weight, thickness, thickness + 3, watch);
        watch();
        QCOMPARE(weight->value(), thickness + 3);
        drag(canvas, weight, thickness + 3, thickness + 1, watch);
        watch();
        QCOMPARE(weight->value(), thickness + 1);
        // Text size snaps to stops, so keep the value the finger lands on.
        const int halfPoints = size->value();
        // An earlier row may have saved a large size for this book.
        const bool smaller = halfPoints > (size->minimum() + size->maximum()) / 2;
        dragAcross(canvas, size, smaller ? .8 : .3, smaller ? .2 : .8, watch);
        watch();
        const int landed = size->value();
        QVERIFY(landed != halfPoints);
        QVERIFY2(disabled.isEmpty(), qPrintable(disabled.join(',')));
        // The latest values stay on the controls while the preview renders.
        QCoreApplication::processEvents();
        QCOMPARE(weight->value(), thickness + 1);
        QCOMPARE(size->value(), landed);
        // Overflow opens at once and keeps the pending edits.
        QVERIFY(click(canvas, overflow));
        auto *general = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(general);
        QTRY_VERIFY(general->isVisible());
        if (slow) {
            // The edits are still queued: closing Settings waits for the
            // preview instead of applying the unedited book settings.
            QVERIFY(!inspectReader(host)["preview_ready"].toBool());
            QVERIFY(click(canvas, overflow));
            QCoreApplication::processEvents();
            QVERIFY(general->isVisible());
            QVERIFY(!host.session()->busy());
            engineActivity().pause(false);
        } else
            QVERIFY(click(canvas, overflow));
        QTRY_VERIFY_WITH_TIMEOUT(!general->isVisible() && !host.session()->busy() &&
                                     !inspectReader(host)["appearance_panel_open"].toBool(),
                                 15000);
        QCOMPARE(host.session()->state()["weight_steps"].toInt(), thickness + 1);
        QCOMPARE(host.session()->state()["font_pixels"].toInt(), kobalt::halfPointPixels(landed));
        QVERIFY(host.session()->state()["font_pixels"] != original["font_pixels"]);
    }
    // A thickness drag repaints only its slider row while the finger moves.
    // After release, the new page and the font sample share one paint pass,
    // so the display shows them in one update.
    void thicknessDragPaints() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        // Kobo buttons and the reader window take no click focus, so a press
        // elsewhere does not repaint the last tapped button.
        canvas.setFocusPolicy(Qt::NoFocus);
        for (auto *button : host.window()->findChildren<QPushButton *>())
            button->setFocusPolicy(Qt::NoFocus);
        // Scrolling the canvas would repaint the panel. Show the whole screen.
        canvas.resize(nativeSize + QSize(40, 40));
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto pages = host.session()->pages();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
        QTRY_VERIFY(inspectReader(host)["preview_ready"].toBool());
        QTest::qWait(100);
        const int previewPages = inspectReader(host)["preview_settings"].toObject()["pages"].toInt();
        QVERIFY(previewPages > 0);
        auto *weight = host.window()->findChild<QSlider *>("rrFontWeight");
        auto *menu = host.window()->findChild<QWidget *>("rrMenu");
        auto *row = weight->parentWidget();
        QVERIFY(weight && menu && row);
        PaintRegions paints;
        paints.coordinateRoot = host.window();
        for (auto *widget : host.window()->findChildren<QWidget *>())
            widget->installEventFilter(&paints);
        const auto take = [&] {
            auto widgets = paints.widgets;
            widgets.removeDuplicates();
            const auto region = paints.painted;
            paints.count = 0;
            paints.painted = {};
            paints.widgets.clear();
            return std::make_pair(widgets, region);
        };
        const QRect rowRect(row->mapTo(host.window(), QPoint()), row->size());
        const int thickness = weight->value();
        take();
        int moves = 0;
        drag(canvas, weight, thickness, thickness + 2, [&] {
            const auto [widgets, region] = take();
            if (region.isEmpty())
                return;
            ++moves;
            QVERIFY2((region - rowRect).isEmpty(), qPrintable(widgets.join(' ')));
        });
        QVERIFY(moves > 0);
        QStringList passes;
        bool pageAndSample = false;
        for (int i = 0; i < 60 && !pageAndSample; ++i) {
            QCoreApplication::processEvents();
            const auto [widgets, region] = take();
            if (!region.isEmpty())
                passes.append(widgets.join(' '));
            QVERIFY2(!widgets.contains("rrControlsTop"), qPrintable(passes.join(" | ")));
            QVERIFY2(!widgets.contains("rrFont") || widgets.contains("rrPage"), qPrintable(passes.join(" | ")));
            pageAndSample = widgets.contains("rrFont") && widgets.contains("rrPage");
            QTest::qWait(25);
        }
        QVERIFY2(pageAndSample, qPrintable(passes.join(" | ")));
        QTRY_VERIFY(!inspectReader(host)["preview_busy"].toBool());
        QCOMPARE(inspectReader(host)["preview_settings"].toObject()["weight_steps"].toInt(), thickness + 2);
        // Thickness redraws glyphs without changing the preview's pagination.
        QCOMPARE(inspectReader(host)["preview_settings"].toObject()["pages"].toInt(), previewPages);
        QCOMPARE(host.session()->pages(), pages);
        // Closing the panel applies the change without repaginating the book.
        // A held engine keeps the apply running while the panel is visible:
        // the panel keeps its pixels and ignores taps until it closes.
        struct Release {
            ~Release() {
                engineActivity().pause(false);
            }
        } release;
        engineActivity().pause(true);
        take();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
        QTRY_VERIFY(host.session()->busy());
        QVERIFY(menu->isVisible());
        auto *heavier = host.window()->findChild<QPushButton *>("rrHeavier");
        QVERIFY(click(canvas, heavier));
        QCoreApplication::processEvents();
        QCOMPARE(weight->value(), thickness + 2);
        const auto [applying, applyingRegion] = take();
        const QRect panelRect(menu->mapTo(host.window(), QPoint()), menu->size());
        QVERIFY2(!applyingRegion.intersects(panelRect), qPrintable(applying.join(' ')));
        engineActivity().pause(false);
        QTRY_VERIFY(!host.session()->busy() && !menu->isVisible());
        QCOMPARE(host.session()->state()["weight_steps"].toInt(), thickness + 2);
        QCOMPARE(host.session()->pages(), pages);
    }
    // Settings ends in the heavy navigation rule that separates it from the
    // page. Raising the panel must not cover that rule.
    void overflowShowsBottomRule() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(nativeSize + QSize(40, 40));
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        auto *general = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QTRY_VERIFY(general->isVisible());
        QCoreApplication::processEvents();
        const QImage image = canvas.capture();
        const QRgb ink = general->palette().color(QPalette::WindowText).rgb();
        const int bottom = general->geometry().bottom();
        for (const int x : {20, nativeSize.width() / 2, nativeSize.width() - 20})
            for (const int y : {bottom - 1, bottom})
                QVERIFY2(image.pixel(x, y) == ink, qPrintable(QString("%1,%2").arg(x).arg(y)));
    }
    // Hardware page buttons page an open fullscreen list. They never turn
    // the book behind it, including when the list has only one page.
    void pageButtonsPageFullscreenLists() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(nativeSize + QSize(40, 40));
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const int bookPage = host.session()->page();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavChapters")));
        auto *choices = host.window()->findChild<QWidget *>("rrChoices");
        QVERIFY(choices);
        QTRY_VERIFY(choices->isVisible());
        host.window()->pageButton(1);
        QTest::qWait(30);
        QVERIFY(choices->isVisible());
        QCOMPARE(host.session()->page(), bookPage);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavChapters")));
        QTRY_VERIFY(!choices->isVisible());

        auto *top = host.window()->findChild<QWidget *>("rrControlsTop");
        QVERIFY(top);
        if (!top->isVisible())
            QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrGeneralSettings")->isVisible());
        auto *openLicenses = host.window()->findChild<QPushButton *>("rrOpenAcknowledgments");
        QVERIFY(openLicenses);
        openLicenses->click();
        auto *licenses = host.window()->findChild<QWidget *>("rrAcknowledgments");
        QVERIFY(licenses);
        QTRY_VERIFY(licenses->isVisible());
        auto *counter = licenses->findChild<QLabel *>("rrAcknowledgmentsPage");
        auto *next = licenses->findChild<QPushButton *>("rrAcknowledgmentsNext");
        QVERIFY(counter && next);
        QVERIFY(next->isVisible() && next->isEnabled());
        const auto first = counter->text();
        host.window()->pageButton(1);
        QTRY_VERIFY(counter->text() != first);
        host.window()->pageButton(-1);
        QTRY_COMPARE(counter->text(), first);
        host.window()->pageButton(-1);
        const auto finalPage = counter->text().split(" of ");
        QCOMPARE(finalPage.first(), finalPage.last());
        host.window()->pageButton(1);
        QTRY_COMPARE(counter->text(), first);
        QCOMPARE(host.session()->page(), bookPage);
    }
    // With a panel open, the active tab's fill meets the panel's bottom rule.
    // The bar's own rule band must not leave a paper gap above the tab.
    void activeTabMeetsPanelRule() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(nativeSize + QSize(40, 40));
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *stats = host.window()->findChild<QPushButton *>("rrNavStats");
        QVERIFY(click(canvas, stats));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrChapterGraph")->isVisible());
        QCoreApplication::processEvents();
        const QImage image = canvas.capture();
        auto *bar = host.window()->findChild<QWidget *>("rrBottomNavigation");
        QVERIFY(bar);
        const QRgb ink = stats->palette().color(QPalette::Text).rgb();
        const int x = stats->mapTo(canvas.surface(), stats->rect().center()).x();
        for (int y = bar->mapTo(canvas.surface(), QPoint()).y(); y <= stats->mapTo(canvas.surface(), QPoint()).y(); ++y)
            QVERIFY2(image.pixel(x, y) == ink, qPrintable(QString::number(y)));
    }
    void linkHighlightsPaintOnlyChangedOutlines() {
        ReadingPage page(nullptr, [] {});
        page.resize(400, 600);
        page.image = QImage(400, 480, QImage::Format_RGB32);
        page.image.fill(Qt::white);
        page.refreshContent();
        page.show();
        QCoreApplication::processEvents();
        PaintRegions paints;
        page.installEventFilter(&paints);
        QImage before = page.grab().toImage();
        const QVector<QRect> upper{QRect(40, 40, 120, 24), QRect(40, 68, 120, 24)};
        const QVector<QRect> lower{QRect(220, 340, 100, 24)};
        QVector<QRect> previous;
        for (const auto &next : {upper, lower, lower, QVector<QRect>{}}) {
            QRegion allowed;
            const int pad = ui::metrics::links::revealPadding;
            for (const auto &rect : previous + next)
                allowed += rect.adjusted(-pad, -pad, pad, pad).translated(0, page.topHeight());
            paints.painted = {};
            page.revealLinks(next);
            QCoreApplication::processEvents();
            const auto damage = paints.painted;
            QVERIFY((damage - allowed).isEmpty());
            if (previous == next)
                QVERIFY(damage.isEmpty());
            else
                QVERIFY(!damage.isEmpty());
            const auto after = page.grab().toImage();
            for (int y = 0; y < after.height(); ++y)
                for (int x = 0; x < after.width(); ++x)
                    if (before.pixel(x, y) != after.pixel(x, y))
                        QVERIFY(damage.contains(QPoint(x, y)));
            before = after;
            previous = next;
        }
    }
    void readingStripChangesLeaveBodyUnpainted() {
        ReadingPage page(nullptr, [] {});
        page.resize(400, 600);
        page.image = QImage(400, 480, QImage::Format_RGB32);
        page.image.fill(Qt::white);
        page.clockText = "09:00";
        page.progress = "1 / 10";
        page.refreshContent();
        page.show();
        QCoreApplication::processEvents();
        PaintRegions paints;
        page.installEventFilter(&paints);
        page.clockText = "09:01";
        page.progress = "2 / 10";
        page.refreshContent();
        QCoreApplication::processEvents();
        QVERIFY(!paints.painted.isEmpty());
        QVERIFY(!paints.painted.intersects(
            QRect(0, page.topHeight(), page.width(), page.height() - page.topHeight() - page.bottomHeight())));
        QVERIFY(!page.property("reader_page_turn").toBool());
        paints.painted = {};
        page.refreshContent();
        QCoreApplication::processEvents();
        QVERIFY(paints.painted.isEmpty());
    }
    void refreshRegionsIncludeOutgoingAndIncomingImages() {
        ReadingPage page(nullptr, [] {});
        page.resize(300, 400);
        page.showHeader = false;
        page.showFooter = false;
        QImage text(300, 400, QImage::Format_RGB32);
        text.fill(Qt::white);
        text.setText("kobalt.image_regions", "[]");
        text.setText("kobalt.colour_regions", "[]");
        text.setText("kobalt.animation_text_only", "true");
        QImage illustrated = text.copy();
        illustrated.setText("kobalt.image_regions", "[[20,40,200,100]]");
        illustrated.setText("kobalt.animation_text_only", "false");
        page.image = text;
        page.refreshContent();
        page.image = illustrated;
        page.refreshContent(true);
        const QRegion expected = QRegion(QRect(20, 40, 200, 100)).translated(0, page.topHeight());
        QCOMPARE(page.property("reader_mono_repairs").value<QRegion>(), expected);
        page.image = text;
        page.refreshContent(true);
        QCOMPARE(page.property("reader_mono_repairs").value<QRegion>(), expected);
        page.refreshContent(true);
        QVERIFY(page.property("reader_mono_repairs").value<QRegion>().isEmpty());
    }
    void footerTextFollowsProgressEdge() {
        const int originalDensity = ui::density();
        struct RestoreDensity {
            int value;
            ~RestoreDensity() {
                ui::setDensity(value);
            }
        } restore{originalDensity};
        for (int density : {167, 212, 300}) {
            ui::setDensity(density);
            for (const QString &size : {QString("small"), QString("medium"), QString("large")}) {
                ReadingPage page(nullptr, [] {});
                page.resize(1264, 1680);
                page.showHeader = false;
                page.showClock = false;
                page.footerSize = size;
                page.footerTextPixels = ui::metrics::reading::textPixels(size, true);
                page.progress = "888 / 888";
                page.progressFraction = .5;
                const int strip = ui::metrics::reading::stripHeight(size);
                const auto textRows = [&] {
                    page.refreshContent();
                    const QImage pixels = page.grab().toImage();
                    QVector<int> rows;
                    const QColor paper = page.palette().color(QPalette::Window);
                    const int top = page.height() - page.bottomHeight();
                    for (int y = 0; y < strip; ++y) {
                        bool ink = false;
                        for (int x = 0; x < pixels.width(); ++x)
                            if (pixels.pixelColor(x, top + y) != paper) {
                                ink = true;
                                break;
                            }
                        if (ink)
                            rows.append(y);
                    }
                    return rows;
                };
                page.showProgress = false;
                const auto withoutLine = textRows();
                QVERIFY(!withoutLine.isEmpty());
                page.showProgress = true;
                const auto withLine = textRows();
                const int fillTop = page.bottomHeight() - int(ui::metrics::reading::progressBottom) -
                                    int(ui::metrics::reading::progressFillHeight);
                const int offset = fillTop - strip;
                QVector<int> expected;
                for (int row : withoutLine)
                    expected.append(row + offset);
                QCOMPARE(withLine, expected);
                QCOMPARE(page.bottomHeight(), strip + int(ui::metrics::reading::progressHeight));
                const QImage pixels = page.grab().toImage();
                QCOMPARE(pixels.pixelColor(0, page.height() - page.bottomHeight() + fillTop),
                         page.palette().color(QPalette::WindowText));
            }
        }
    }
    void statusDividerFollowsPanelPlacement() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        QVERIFY(page->topHeight() > 0);
        ui::Panel panel(host.window());
        panel.hide();
        const QPoint edge(10, page->topHeight() - 1);
        const auto colour = [&] {
            return page->grab().toImage().pixelColor(edge);
        };
        const auto paper = page->palette().color(QPalette::Window);
        QCOMPARE(colour(), paper);
        panel.setGeometry(0, page->topHeight(), host.window()->width(), 400);
        panel.show();
        QTRY_VERIFY(colour() != paper);
        panel.move(0, page->topHeight() + 100);
        QTRY_COMPARE(colour(), paper);
        panel.move(0, page->topHeight());
        QTRY_VERIFY(colour() != paper);
        panel.hide();
        QTRY_COMPARE(colour(), paper);
        // A sheet that covers the header has no adjacent status strip.
        panel.move(0, 0);
        panel.show();
        QTRY_COMPARE(colour(), paper);
    }

    void rightToLeftPageTurns() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(qEnvironmentVariable("KOBALT_FIXTURES") + "/rtl.epub", nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->state().value("page_progression_rtl").toBool());
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(80, nativeSize.height() / 2)));
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 2);
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() - 80, nativeSize.height() / 2)));
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        // Exercise the direction delivered by the page's swipe recognizer.
        page->swiped(-1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 2);
        page->swiped(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
        // Android volume/page keys use logical directions even in RTL books.
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 2);
        host.window()->pageButton(-1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 1);
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        auto *settings = host.window()->findChild<QWidget *>("rrSettingsPages");
        QVERIFY(settings);
        QTRY_VERIFY(settings->isVisible());
        host.window()->pageButton(1);
        QTest::qWait(30);
        QCOMPARE(host.session()->page(), 1);
    }
    void chromeSizeSegmentsApplyAndPersist() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{directory.path()};
        paths.stateReport = paths.directory + "/state.json";
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavLayout")));
        const auto previewReady = [&] {
            host.window()->checkpoint();
            const auto state = storage::readJson(paths.stateReport);
            return state["preview_ready"].toBool() && !state["preview_busy"].toBool();
        };
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        for (const auto &part : {QString("Header"), QString("Footer")}) {
            const bool footer = part == "Footer";
            const QString visibility = footer ? "show_footer" : "show_header";
            const QString sizeKey = footer ? "footer_text_size" : "header_text_size";
            for (const auto &value : {QString("small"), QString("medium"), QString("large"), QString("off")}) {
                auto *button = host.window()->findChild<QPushButton *>("rr" + part + "TextSize_" + value);
                QVERIFY(button);
                QVERIFY(click(canvas, button));
                QVERIFY(button->isChecked());
                QVERIFY(host.window()->findChild<QWidget *>("rrChoices")->isHidden());
                QTRY_VERIFY(previewReady());
                QCOMPARE(footer ? page->showFooter : page->showHeader, value != "off");
                QCOMPARE(footer ? page->footerSize : page->headerSize, value == "off" ? QString("large") : value);
                const auto saved = storage::readJson(paths.bookState(identity));
                QCOMPARE(saved.value(visibility).toBool(), value != "off");
                QCOMPARE(saved.value(sizeKey).toString(), value == "off" ? QString("large") : value);
                for (auto *sibling : button->parentWidget()->findChildren<QPushButton *>()) {
                    QCOMPARE(sibling->isChecked(), sibling == button);
                }
            }
        }
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavStats")));
        QTRY_VERIFY(!host.session()->busy());
        host.shutdown();
        QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QVERIFY(host.window()->findChild<QPushButton *>("rrHeaderTextSize_off")->isChecked());
        QVERIFY(host.window()->findChild<QPushButton *>("rrFooterTextSize_off")->isChecked());
    }
    void chromeSizeLabelsFollowAvailableWidth_data() {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("compact");
        QTest::newRow("libra") << 300 << 1264 << false;
        QTest::newRow("nia") << 212 << 758 << false;
        QTest::newRow("narrow-300") << 300 << 800 << true;
        QTest::newRow("narrow-212") << 212 << 566 << true;
    }
    void chromeSizeLabelsFollowAvailableWidth() {
        QFETCH(int, ppi);
        QFETCH(int, width);
        QFETCH(bool, compact);
        const int previous = ui::density();
        struct RestoreDensity {
            int value;
            ~RestoreDensity() {
                ui::setDensity(value);
            }
        } restore{previous};
        ui::setDensity(ppi);
        QWidget owner;
        auto *typography = new QWidget(&owner), *layout = new QWidget(&owner);
        auto *typeRows = new QVBoxLayout(typography), *pageRows = new QVBoxLayout(layout);
        pageRows->setContentsMargins(ui::metrics::panel::contentMargins);
        reader::AppearancePanel panel({typography, layout, typeRows, pageRows}, {});
        owner.resize(width, ui::px(900));
        layout->resize(owner.size());
        owner.show();
        layout->show();
        QCoreApplication::processEvents();
        const QStringList full = {"Off", "Small", "Medium", "Large"};
        const QStringList shortLabels = {"Off", "S", "M", "L"};
        for (const auto &buttons : {panel.headerSizeButtons, panel.footerSizeButtons}) {
            for (int i = 0; i < buttons.size(); ++i) {
                auto *button = buttons[i];
                QCOMPARE(button->text(), (compact ? shortLabels : full)[i]);
                QCOMPARE(button->accessibleName(), full[i]);
                QCOMPARE(button->font().pixelSize(), int(ui::metrics::text::secondary));
                QVERIFY(ui::interfaceTextWidth(button->text(), button->font()) +
                            2 * int(ui::metrics::control::segmentHorizontalPadding) <=
                        button->width());
            }
        }
        owner.resize(1264, owner.height());
        layout->resize(owner.size());
        QCoreApplication::processEvents();
        for (int i = 0; i < full.size(); ++i) {
            QCOMPARE(panel.headerSizeButtons[i]->text(), full[i]);
            QCOMPARE(panel.footerSizeButtons[i]->text(), full[i]);
        }
    }
    void bookLayoutAndGlobalBionicReopen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{directory.path()};
        const auto firstPath = fixture();
        const auto secondPath = qEnvironmentVariable("KOBALT_FIXTURES") + "/rtl.epub";
        const auto identity = [](const QString &path) {
            return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
        };
        QVERIFY(storage::writeJson(paths.bookState(identity(firstPath)), QJsonObject{{"book", identity(firstPath)},
                                                                                     {"show_header", false},
                                                                                     {"show_clock", false},
                                                                                     {"header_text_size", "large"},
                                                                                     {"bold_reading", "off"}}));
        QVERIFY(storage::writeJson(paths.preferences(), QJsonObject{{"bold_reading", "bold"}}));
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        for (const auto &path : {firstPath, secondPath, firstPath}) {
            const int count = opened.count();
            host.open(path, nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), count + 1, 30000);
            QTRY_VERIFY(!host.session()->busy());
            QCOMPARE(host.session()->state()["bold_reading"].toString(), QString("bold"));
            auto *page = host.window()->findChild<ReadingPage *>("rrPage");
            QVERIFY(page);
            QCOMPARE(page->showHeader, path != firstPath);
            QCOMPARE(page->showClock, path != firstPath);
            host.shutdown();
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
        }
    }
    void boldReadingAppliesImmediately_data() {
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<QString>("savedMode");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("ppi");
        for (bool enabled : {false, true})
            for (bool dark : {false, true})
                for (int ppi : {300, 212}) {
                    const auto name =
                        QString("%1-%2-%3").arg(enabled ? "on" : "default-off", dark ? "dark" : "light").arg(ppi);
                    QTest::newRow(qPrintable(name))
                        << enabled << (enabled ? QString("bold") : QString()) << dark << ppi;
                }
        QTest::newRow("legacy-synthetic") << true << QString("synthetic") << false << 300;
    }
    void boldReadingAppliesImmediately() {
        QFETCH(bool, enabled);
        QFETCH(QString, savedMode);
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        struct RestoreDensity {
            int density = ui::density(), ppi = readingPpi.load();
            ~RestoreDensity() {
                ui::setDensity(density);
                readingPpi.store(ppi);
            }
        } restore;
        ui::setDensity(ppi);
        readingPpi.store(ppi);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{directory.path()};
        QJsonObject saved{{"dark_mode", dark}};
        if (!savedMode.isEmpty())
            saved["bold_reading"] = savedMode;
        QVERIFY(storage::writeJson(paths.preferences(), saved));
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->image();
        const QJsonValue position = host.session()->state()["position"];
        const auto initialMode = enabled ? QStringLiteral("bold") : QStringLiteral("off");
        const auto changedMode = enabled ? QStringLiteral("off") : QStringLiteral("bold");
        QCOMPARE(host.session()->state()["bold_reading"].toString(), initialMode);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        auto *option = host.window()->findChild<QCheckBox *>("rrBionicReading");
        auto *panel = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(option && panel && panel->isVisible());
        QCOMPARE(option->isChecked(), enabled);
        {
            BlockedFile blocked(paths.preferences());
            QVERIFY(blocked.active());
            QVERIFY(click(canvas, option));
            QCOMPARE(option->isChecked(), enabled);
            QCOMPARE(host.session()->state()["bold_reading"].toString(), initialMode);
            QVERIFY(!host.session()->busy());
            QCOMPARE(host.session()->image(), original);
            auto *notice = host.window()->findChild<QWidget *>("rrSettingsWriteError");
            QVERIFY(notice && notice->isVisible());
            QVERIFY(
                capture(canvas, host, QString("native-reader-settings-write-error-%1").arg(QTest::currentDataTag())));
            const auto page = host.session()->page();
            QTest::keyClick(host.window(), Qt::Key_Right);
            QCOMPARE(host.session()->page(), page);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
            QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSettingsWriteError"));
            QVERIFY(blocked.restore());
            QCOMPARE(storage::readJson(paths.preferences()), saved);
        }
        QVERIFY(click(canvas, option));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state()["bold_reading"].toString() == changedMode, 30000);
        QVERIFY(panel->isVisible());
        QCOMPARE(storage::readJson(paths.preferences())["bold_reading"].toString(), changedMode);
        QVERIFY(host.session()->image() != original);
        QCOMPARE(host.session()->state()["position"], position);
        {
            struct ResumeEngine {
                ~ResumeEngine() {
                    engineActivity().pause(false);
                }
            } resume;
            engineActivity().pause(true);
            QVERIFY(click(canvas, option));
            QVERIFY(host.session()->busy());
            QVERIFY(click(canvas, option));
            QCOMPARE(host.session()->requestedState()["bold_reading"].toString(), changedMode);
            QCOMPARE(storage::readJson(paths.preferences())["bold_reading"].toString(), changedMode);
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state()["bold_reading"].toString() == changedMode, 30000);
        QVERIFY(click(canvas, option));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state()["bold_reading"].toString() == initialMode, 30000);
        QVERIFY(panel->isVisible());
        QCOMPARE(storage::readJson(paths.preferences())["bold_reading"].toString(), initialMode);
    }
    void boldReadingPreservesTypographyDraft() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{directory.path()};
        paths.stateReport = directory.path() + "/state.json";
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!host.session()->busy());
        const QJsonValue pixels = host.session()->state()["font_pixels"];
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QCheckBox *option = nullptr;
        {
            struct ResumeEngine {
                ~ResumeEngine() {
                    engineActivity().pause(false);
                }
            } resume;
            engineActivity().pause(true);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
            QVERIFY(click(canvas, sizeStep(host)));
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            option = host.window()->findChild<QCheckBox *>("rrBionicReading");
            QVERIFY(option && !option->isChecked());
            QVERIFY(click(canvas, option));
            QCOMPARE(host.session()->requestedState()["bold_reading"].toString(), QString("bold"));
            QCOMPARE(storage::readJson(paths.preferences())["bold_reading"].toString(), QString("bold"));
        }
        const auto inspect = [&] {
            host.window()->checkpoint();
            return storage::readJson(paths.stateReport);
        };
        for (const auto *mode : {"bold", "off"}) {
            if (QString::fromLatin1(mode) == "off")
                QVERIFY(click(canvas, option));
            QTRY_VERIFY_WITH_TIMEOUT(
                !host.session()->busy() && host.session()->state()["bold_reading"].toString() == mode, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(inspect()["preview_ready"].toBool() && !inspect()["preview_busy"].toBool(), 30000);
            const auto draft = inspect()["preview_settings"].toObject();
            QCOMPARE(draft["bold_reading"].toString(), QString::fromLatin1(mode));
            QVERIFY(draft["font_pixels"] != pixels);
            QCOMPARE(host.session()->state()["font_pixels"], pixels);
        }
        const auto draft = inspect()["preview_settings"].toObject();
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state()["font_pixels"] == draft["font_pixels"], 30000);
        QCOMPARE(host.session()->state()["bold_reading"].toString(), QString("off"));
    }
    void readerSettingsWriteErrors_data() {
        QTest::addColumn<QString>("optionName");
        QTest::addColumn<bool>("immediate");
        QTest::newRow("dark-mode") << QString("rrDarkMode") << true;
        QTest::newRow("interface-weight") << QString("rrHeavierText") << true;
        QTest::newRow("deferred-progress") << QString("rrShowProgress") << false;
    }
    void readerSettingsWriteErrors() {
        QFETCH(QString, optionName);
        QFETCH(bool, immediate);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{directory.path()};
        const QJsonObject saved{{"dark_mode", false}, {"heavier_interface_text", true}, {"show_progress_bar", true}};
        QVERIFY(storage::writeJson(paths.preferences(), saved));
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!host.session()->busy());
        const auto image = host.session()->image();
        const auto interfaceWeight = ui::interfaceWeight();
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(click(canvas, settings));
        if (optionName == "rrHeavierText")
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsAdvanced")));
        auto *option = host.window()->findChild<QCheckBox *>(optionName);
        QVERIFY(option);
        const bool checked = option->isChecked();
        BlockedFile blocked(paths.preferences());
        QVERIFY(blocked.active());
        QVERIFY(click(canvas, option));
        if (!immediate)
            QVERIFY(click(canvas, settings));
        auto *notice = host.window()->findChild<QWidget *>("rrSettingsWriteError");
        QVERIFY(notice && notice->isVisible());
        if (immediate)
            QCOMPARE(option->isChecked(), checked);
        QVERIFY(!host.session()->busy());
        QCOMPARE(host.session()->image(), image);
        QCOMPARE(ui::interfaceWeight(), interfaceWeight);
        QVERIFY(host.window()->findChild<ReadingPage *>("rrPage")->showProgress);
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettingsWriteErrorConfirm")));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSettingsWriteError"));
        QVERIFY(blocked.restore());
        QCOMPARE(storage::readJson(paths.preferences()), saved);
    }
    void bionicModesAndThickness() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<QWidget *>("rrPage");
        QVERIFY(page);
        auto pageTap = [&] {
            QTest::mouseClick(page, Qt::LeftButton, Qt::NoModifier, page->rect().center());
        };
        auto mode = [&](const char *value) {
            if (!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible())
                pageTap();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrSettings")));
            auto *option = host.window()->findChild<QCheckBox *>("rrBionicReading");
            QVERIFY(option);
            QVERIFY(option->isVisible());
            if (option->isChecked() != (QString::fromLatin1(value) == "bold"))
                QVERIFY(click(canvas, option));
            pageTap();
            QTRY_VERIFY_WITH_TIMEOUT(
                !host.session()->busy() && host.session()->state().value("bold_reading").toString() == value, 30000);
        };
        mode("off");
        QVERIFY(host.session()->setWeight(3));
        QTRY_VERIFY(!host.session()->busy());
        for (const auto *value : {"bold"}) {
            mode(value);
            QCOMPARE(host.session()->weight(), 3);
            QVERIFY(!host.session()->setWeight(7));
            pageTap();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
            auto *weight = host.window()->findChild<QSlider *>("rrFontWeight");
            QVERIFY(weight);
            QTRY_VERIFY(weight->isVisible());
            QVERIFY(!weight->isEnabled());
            QCOMPARE(weight->value(), 0);
            QVERIFY(!host.window()->findChild<QPushButton *>("rrHeavier")->isEnabled());
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
            auto *sample = host.window()->findChild<ui::SampleLabel *>("rrFontChoice1Title");
            QVERIFY(sample);
            QTRY_VERIFY(sample->isVisible());
            QCOMPARE(sample->sampleAppearance().bionic, QString::fromLatin1(value));
            QCOMPARE(sample->sampleAppearance().weight, 0);
            const auto emphasized = sample->grab().toImage();
            sample->setAppearance({0, "off"});
            QVERIFY(sample->grab().toImage() != emphasized);
            sample->setAppearance({0, QString::fromLatin1(value)});
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
            pageTap();
            QTRY_VERIFY(!host.session()->busy());
            mode("off");
            QCOMPARE(host.session()->weight(), 3);
            pageTap();
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavText")));
            QTRY_VERIFY(weight->isVisible());
            QVERIFY(weight->isEnabled());
            QCOMPARE(weight->value(), 3);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFont")));
            auto *normalSample = host.window()->findChild<ui::SampleLabel *>("rrFontChoice1Title");
            QVERIFY(normalSample);
            QTRY_VERIFY(normalSample->isVisible());
            QCOMPARE(normalSample->sampleAppearance().bionic, QString("off"));
            QCOMPARE(normalSample->sampleAppearance().weight, 3);
            QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrFontClose")));
            pageTap();
            QTRY_VERIFY(!host.session()->busy());
        }
        QVERIFY(host.session()->setWeight(0));
        QTRY_VERIFY(!host.session()->busy());
    }
    void openTurnAndPersist() {
        if (!qEnvironmentVariableIsSet("KOBALT_TEST_SETTINGS") && !qEnvironmentVariableIsSet("KOBALT_TEST_PAGE")) {
            // Earlier tests change this book's appearance and position.
            const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
            const auto statePath = testPaths().bookState(identity);
            QVERIFY(!QFile::exists(statePath) || QFile::remove(statePath));
            QVERIFY(storage::writeJson(testPaths().preferences(), {}));
        }
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        QSignalSpy opened(&host, &test::ReaderHost::opened), failed(&host, &test::ReaderHost::failed),
            closed(&host, &test::ReaderHost::closed);
        CloseHost cleanup{host};
        host.open(fixture(), nativeSize);
        QTRY_VERIFY_WITH_TIMEOUT(opened.count() || failed.count(), 30000);
        if (!failed.isEmpty())
            qWarning() << failed.first();
        QCOMPARE(opened.count(), 1);
        QVERIFY(host.session()->pages() > 2);
        QVERIFY(!host.session()->sourceHash().isEmpty());
        QVERIFY(!host.session()->image().isNull());
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        if (qEnvironmentVariableIsSet("KOBALT_TEST_PAGE")) {
            host.session()->go(qEnvironmentVariableIntValue("KOBALT_TEST_PAGE"));
            QTRY_VERIFY(!host.session()->busy());
        }
        QVERIFY(capture(canvas, host, "native-reader-reading"));
        const int before = host.session()->page();
        const QImage first = host.session()->image();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() - 80, nativeSize.height() / 2)));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy() && host.session()->page() != before, 10000);
        QVERIFY(host.session()->image() != first);
        QVERIFY(capture(canvas, host, "native-reader-page"));
        // Compare every panel at the same page as the supplied baseline.
        QVERIFY(host.session()->go(before));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *typography = host.window()->findChild<QPushButton *>("rrNavText");
        QVERIFY(typography);
        QTRY_VERIFY(typography->isVisible());
        QVERIFY(capture(canvas, host, "native-reader-menu"));
        QVERIFY(click(canvas, typography));
        auto *font = host.window()->findChild<QPushButton *>("rrFont");
        QVERIFY(font);
        QTRY_VERIFY(font->isVisible() && font->isEnabled());
        // The native 80-pixel rule must not distort our custom slider rows.
        const auto top = [&](const char *name) {
            auto *widget = host.window()->findChild<QWidget *>(name);
            return widget ? widget->mapTo(host.window(), QPoint()).y() : -1;
        };
        QVERIFY(top("rrLighter") > top("rrSmaller"));
        QVERIFY(top("rrTighter") > top("rrLighter"));
        QVERIFY(top("rrAlignPublisher") > top("rrTighter"));
        const auto left = [&](const char *name) {
            auto *widget = host.window()->findChild<QWidget *>(name);
            return widget ? widget->mapTo(host.window(), QPoint()).x() : -1;
        };
        QVERIFY(left("rrAlignLeft") < left("rrAlignJustify"));
        QVERIFY(left("rrAlignJustify") < left("rrAlignPublisher"));
        for (const char *name : {"rrTextSize", "rrFontWeight", "rrLineSpacing"}) {
            auto *slider = host.window()->findChild<QSlider *>(name);
            QVERIFY(slider);
            QCOMPARE(slider->height(), int(ui::metrics::control::sliderHeight));
            QVERIFY(slider->parentWidget()->rect().contains(slider->geometry()));
            QCOMPARE(slider->visibleRegion(), QRegion(slider->rect()));
        }
        QVERIFY(capture(canvas, host, "native-reader-typography"));
        QVERIFY(click(canvas, font));
        auto *choices = host.window()->findChild<QWidget *>("rrChoices");
        QVERIFY(choices);
        QTRY_VERIFY(choices->isVisible());
        auto *choice = choices->findChild<QPushButton *>("rrFontChoice0");
        QVERIFY(choice);
        QTRY_VERIFY(choice->isVisible());
        canvas.ensureWidgetVisible(choice);
        QCoreApplication::processEvents();
        QTest::mouseMove(canvas.windowHandle(), choice->mapTo(&canvas, choice->rect().center()));
        QCoreApplication::processEvents();
        QVERIFY(choices->isVisible());
        QVERIFY(capture(canvas, host, "native-reader-fonts"));
        QVERIFY(click(canvas, choice));
        QVERIFY(choices->isVisible());
        // Restore the baseline font after testing selection so later panels
        // describe the same reading state as the firmware reference.
        QLabel *libron = nullptr;
        for (auto *label : choices->findChildren<QLabel *>())
            if (label->objectName().endsWith("Title") && label->text() == "Libron")
                libron = label;
        QVERIFY(libron);
        QCOMPARE(libron->text(), QString("Libron"));
        QCOMPARE(QFontInfo(libron->font()).family(), QString("Libron"));
        QVERIFY(click(canvas, libron->parentWidget()));
        auto *done = choices->findChild<QPushButton *>("rrFontClose");
        QVERIFY(done);
        QVERIFY(done->isVisible());
        QVERIFY(click(canvas, done));
        QTRY_VERIFY(!choices->isVisible());
        QTRY_VERIFY(font->isVisible());
        QVERIFY(click(canvas, font));
        // A real press above the dropdown must dismiss only the dropdown.
        QVERIFY(clickAt(canvas, QPoint(10, 10)));
        QTRY_VERIFY(!choices->isVisible());
        auto *layout = host.window()->findChild<QPushButton *>("rrNavLayout");
        QVERIFY(layout);
        QTRY_VERIFY(layout->isEnabled());
        QVERIFY(click(canvas, layout));
        QTRY_VERIFY(!font->isVisible());
        auto *marginSlider = host.window()->findChild<QSlider *>("rrMargins");
        QVERIFY(marginSlider);
        QCOMPARE(marginSlider->height(), int(ui::metrics::control::sliderHeight));
        QVERIFY(marginSlider->parentWidget()->rect().contains(marginSlider->geometry()));
        auto *narrower = host.window()->findChild<QPushButton *>("rrNarrower");
        QVERIFY(narrower);
        QCOMPARE(narrower->mapTo(host.window(), narrower->rect().center()).y(),
                 marginSlider->mapTo(host.window(), marginSlider->rect().center()).y());
        QVERIFY(top("rrVerticalMargins") > top("rrMargins"));
        QTRY_VERIFY(host.window()->findChild<QPushButton *>("rrOrientation1")->isEnabled());
        QVERIFY(capture(canvas, host, "native-reader-layout"));
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QTRY_VERIFY(settings->isEnabled());
        QVERIFY(click(canvas, settings));
        auto *general = host.window()->findChild<QWidget *>("rrGeneralSettings");
        QVERIFY(general);
        QTRY_VERIFY(general->isVisible());
        auto *animations = host.window()->findChild<QWidget *>("rrPageAnimations");
        QVERIFY(animations);
        QCOMPARE(animations->isEnabled(), test::fixtureDeviceControls());
        QVERIFY(capture(canvas, host, "native-reader-settings"));
        host.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY(host.idle());
        QVERIFY(!host.session());
        QVERIFY(!host.window());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QVERIFY(host.session()->savedStateMatched());
        host.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 2, 10000);
    }
    void legacyInterfaceKeysIgnored() {
        // Older builds saved an interface style and a navigation switch. The
        // reader shows Kobalt navigation whatever they say, and later saves keep
        // them and every other setting it does not own.
        const QString settingsPath = testPaths().preferences();
        const auto original = storage::readJson(settingsPath);
        auto legacy = original;
        legacy["ui_style"] = QStringLiteral("reimagined");
        legacy["familiar_navigation"] = true;
        legacy["custom_setting"] = 42;
        QVERIFY(storage::writeJson(settingsPath, legacy));
        {
            test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
            CloseHost cleanup{host};
            QSignalSpy opened(&host, &test::ReaderHost::opened);
            host.open(fixture(), nativeSize);
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
            test::ReaderCanvas canvas(host.window(), nativeSize);
            canvas.resize(900, 900);
            canvas.show();
            QVERIFY(QTest::qWaitForWindowExposed(&canvas));
            QTRY_VERIFY(!host.session()->busy());
            QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
            auto *textTab = host.window()->findChild<QPushButton *>("rrNavText");
            QVERIFY(textTab);
            QTRY_VERIFY(textTab->isVisible());
            auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
            QVERIFY(settings);
            QTRY_VERIFY(settings->isVisible() && settings->isEnabled());
            QVERIFY(click(canvas, settings));
            auto *dark = host.window()->findChild<QCheckBox *>("rrDarkMode");
            QVERIFY(dark);
            QTRY_VERIFY(dark->isVisible());
            const bool wasDark = dark->isChecked();
            QVERIFY(click(canvas, dark));
            QTRY_COMPARE(storage::readJson(settingsPath).value("dark_mode").toBool(), !wasDark);
            const auto saved = storage::readJson(settingsPath);
            QCOMPARE(saved.value("ui_style").toString(), QStringLiteral("reimagined"));
            QCOMPARE(saved.value("familiar_navigation").toBool(), true);
            QCOMPARE(saved.value("custom_setting").toInt(), 42);
        }
        QVERIFY(storage::writeJson(settingsPath, original));
    }
    void bookmarkJumpAndReturn_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("ppi");
        for (bool dark : {false, true}) {
            for (int ppi : {300, 212}) {
                const auto name = QString("%1-%2").arg(dark ? "dark" : "light").arg(ppi);
                QTest::newRow(qPrintable(name)) << dark << ppi;
            }
        }
    }
    void bookmarkJumpAndReturn() {
        QFETCH(bool, dark);
        QFETCH(int, ppi);
        struct RestoreDensity {
            int density = ui::density(), ppi = readingPpi.load();
            ~RestoreDensity() {
                ui::setDensity(density);
                readingPpi.store(ppi);
            }
        } restore;
        ui::setDensity(ppi);
        readingPpi.store(ppi);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{directory.path()};
        QVERIFY(storage::writeJson(paths.preferences(), {{"dark_mode", dark}}));
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        // Keep the header visible while clicking the return control at the bottom.
        canvas.resize(nativeSize + QSize(40, 40));
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(2));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        const auto destination = host.session()->state().value("position").toString();
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->bookmarkCorner().center())));
        QTRY_VERIFY(page->bookmarked);
        QVERIFY(host.session()->go(8));
        QTRY_VERIFY(!host.session()->busy());
        const auto origin = host.session()->state().value("position").toString();
        const int originPage = host.session()->page();
        const auto originChapter = page->chapterTitle;
        const QPoint center(nativeSize.width() / 2, nativeSize.height() / 2);
        QVERIFY(clickAt(canvas, center));
        auto *bookmarks = host.window()->findChild<QPushButton *>("rrBookmarks");
        QVERIFY(bookmarks && bookmarks->isVisible());
        QVERIFY(click(canvas, bookmarks));
        auto *entry = host.window()->findChild<QPushButton *>("rrBookmark0");
        QVERIFY(entry && entry->isVisible());
        class PassagePaints final : public QObject {
        public:
            test::ReaderHost &host;
            ReadingPage &page;
            QString position;
            QVector<QRegion> frames;
            QVector<int> animations;
            PassagePaints(test::ReaderHost &host, ReadingPage &page) : host(host), page(page) {}

        protected:
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::Paint && !host.session()->busy() &&
                    host.session()->state().value("position").toString() == position) {
                    frames.append(static_cast<QPaintEvent *>(event)->region());
                    animations.append(page.window()->property("reader_animation_direction").toInt());
                }
                return false;
            }
        } paints(host, *page);
        page->installEventFilter(&paints);
        paints.position = destination;
        QVERIFY(click(canvas, entry));
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), destination);
        QTRY_VERIFY(!paints.frames.isEmpty());
        QCOMPARE(paints.frames.first().rectCount(), 1);
        QVERIFY(paints.frames.first().contains(page->ribbonRect()));
        QCOMPARE(paints.animations.first(), 0);
        QVERIFY(page->bookmarked);
        QVERIFY(host.window()->grab().save(
            QString("native-reader-bookmark-jump-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        QVERIFY(!host.window()->findChild<QWidget *>("rrBookmarkList")->isVisible());
        auto *back = host.window()->findChild<ui::CaptionButton *>("rrNavLinkReturn");
        QVERIFY(back);
        QVERIFY(!back->isVisible());
        QVERIFY(clickAt(canvas, center));
        QTRY_VERIFY(back->isVisible());
        QCOMPARE(back->text(), strings::get("reader.return_page").arg(originPage + 1));
        QCOMPARE(back->caption(), strings::get("reader.jumped_from_chapter").arg(originChapter));
        QVERIFY(host.window()->grab().save(
            QString("native-reader-bookmark-return-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        // Closing the menu and reading on keep the original return passage.
        QVERIFY(clickAt(canvas, center));
        QTRY_VERIFY(!back->isVisible());
        host.window()->pageButton(1);
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->page(), 3);
        // Return from another bookmarked page after reading on.
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->bookmarkCorner().center())));
        QTRY_VERIFY(page->bookmarked);
        QVERIFY(clickAt(canvas, center));
        QTRY_VERIFY(back->isVisible());
        paints.frames.clear();
        paints.animations.clear();
        paints.position = origin;
        QVERIFY(click(canvas, back));
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), origin);
        QTRY_VERIFY(!paints.frames.isEmpty());
        QCOMPARE(paints.frames.first().rectCount(), 1);
        QVERIFY(paints.frames.first().contains(page->ribbonRect()));
        QCOMPARE(paints.animations.first(), 0);
        QVERIFY(!page->bookmarked);
        QVERIFY(host.window()->grab().save(
            QString("native-reader-passage-return-%1-%2.png").arg(dark ? "dark" : "light").arg(ppi)));
        QVERIFY(clickAt(canvas, center));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QVERIFY(!back->isVisible());
        // Discarding a return entry leaves the bookmarked passage open.
        QVERIFY(click(canvas, bookmarks));
        entry = host.window()->findChild<QPushButton *>("rrBookmark0");
        QVERIFY(entry && entry->isVisible());
        QVERIFY(click(canvas, entry));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, center));
        QTRY_VERIFY(back->isVisible());
        auto *stay = host.window()->findChild<QPushButton *>("rrNavLinkStay");
        QVERIFY(stay && stay->isVisible());
        QVERIFY(click(canvas, stay));
        QVERIFY(!back->isVisible());
        QCOMPARE(host.session()->state().value("position").toString(), destination);
        // Selecting the current bookmark must not create another return entry.
        QVERIFY(click(canvas, bookmarks));
        entry = host.window()->findChild<QPushButton *>("rrBookmark0");
        QVERIFY(entry && entry->isVisible());
        QVERIFY(click(canvas, entry));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, center));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QVERIFY(!back->isVisible());
    }
    void bookmarkReturnSharesLinkHistory() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        storage::Paths paths{directory.path()};
        bool suspended = false;
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] {
                return suspended;
            };
        };
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(2));
        QTRY_VERIFY(!host.session()->busy());
        const auto first = host.session()->state().value("position").toString();
        QVERIFY(host.session()->go(5));
        QTRY_VERIFY(!host.session()->busy());
        const auto second = host.session()->state().value("position").toString();
        QVERIFY(host.session()->go(8));
        QTRY_VERIFY(!host.session()->busy());
        const auto origin = host.session()->state().value("position").toString();
        auto *list = host.window()->findChild<BookmarkList *>("rrBookmarkList");
        auto *links = host.window()->findChild<LinkPanel *>("rrLinkPanel");
        auto *back = host.window()->findChild<QPushButton *>("rrLinkReturn");
        QVERIFY(list && links && back);
        // Exercise the same callbacks as a bookmark row and Show in book.
        list->selected(first);
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), first);
        links->navigate(second);
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), second);
        list->selected(second);
        QTRY_VERIFY(!host.session()->busy());
        list->selected({});
        QVERIFY(!host.session()->busy());
        list->selected("not-a-valid-position");
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(!host.session()->error().isEmpty());
        QCOMPARE(host.session()->state().value("position").toString(), second);
        // A busy reader must keep its current command and return history.
        QVERIFY(host.session()->go(6));
        QVERIFY(host.session()->busy());
        list->selected(origin);
        back->click();
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->page(), 6);
        const auto beforeSleep = host.session()->state().value("position").toString();
        suspended = true;
        list->selected(origin);
        back->click();
        QVERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), beforeSleep);
        suspended = false;
        host.window()->sleepReader();
        list->selected(origin);
        back->click();
        QVERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), beforeSleep);
        host.window()->wakeReader();
        QTRY_VERIFY(!host.session()->busy());
        // Reflow changes page numbers; return still uses the saved passage.
        const auto state = host.session()->state();
        QVERIFY(host.session()->style(state.value("font_pixels").toInt() + 4, state.value("line_spacing").toInt()));
        QTRY_VERIFY(!host.session()->busy());
        back->click();
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), first);
        back->click();
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), origin);
        // Consuming both entries must not create a trip back to a destination.
        back->click();
        QVERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), origin);
        bool prepared = false;
        host.window()->prepareShutdown([&] {
            prepared = true;
        });
        QTRY_VERIFY(prepared);
        list->selected(first);
        QVERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), origin);
    }
    void searchAndBookmark() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() - 30, 30)));
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *bookmarks = host.window()->findChild<QPushButton *>("rrBookmarks");
        QVERIFY(bookmarks);
        QTRY_VERIFY(bookmarks->isVisible());
        QVERIFY(click(canvas, bookmarks));
        auto *entry = host.window()->findChild<QPushButton *>("rrBookmark0");
        QTRY_VERIFY((entry = host.window()->findChild<QPushButton *>("rrBookmark0")) && entry->isVisible());
        QVERIFY(capture(canvas, host, "native-reader-bookmarks"));
        QVERIFY(click(canvas, entry));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *search = host.window()->findChild<QPushButton *>("rrNavSearch");
        QVERIFY(search);
        QTRY_VERIFY(search->isVisible());
        QVERIFY(click(canvas, search));
        auto *query = host.window()->findChild<QLineEdit *>("rrSearchQuery");
        QVERIFY(query);
        QTRY_VERIFY(query->isVisible());
        QVERIFY(click(canvas, query));
        QTest::keyClicks(query, qEnvironmentVariable("KOBALT_TEST_QUERY", "chapter"));
        QTest::keyClick(query, Qt::Key_Return);
        QPushButton *result = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (result = host.window()->findChild<QPushButton *>("rrSearchResult0")) && result->isVisible(), 10000);
        QVERIFY(capture(canvas, host, "native-reader-search"));
        const QImage rendered("native-reader-search.png");
        for (auto *row : host.window()->findChildren<QPushButton *>()) {
            if (!row->objectName().startsWith("rrSearchResult") || !row->isVisible())
                continue;
            QVERIFY(row->updatesEnabled());
            // Count text inside each row, excluding its border. Geometry-only
            // checks missed rows left unpainted by QWidget::render.
            const QPoint origin = row->mapTo(canvas.surface(), QPoint());
            const QRect interior = QRect(origin, row->size()).adjusted(35, 25, -35, -25);
            int ink = 0;
            for (int y = interior.top(); y <= interior.bottom(); ++y)
                for (int x = interior.left(); x <= interior.right(); ++x)
                    ink += qGray(rendered.pixel(x, y)) < 180;
            QVERIFY2(ink > 100, qPrintable(row->objectName() + " has no rendered text"));
        }
        QVERIFY(captureSearchMetrics(result, query->text()));
        if (test::platformRequest.contains("freetype")) {
            const auto metrics = storage::readJson("native-reader-search-metrics.json");
            QCOMPARE(metrics["resolved_family"].toString(), QString("Libron"));
            QCOMPARE(metrics["pixel_size"].toInt(), 32);
            QCOMPARE(metrics["ascent"].toInt(), 26);
            QCOMPARE(metrics["descent"].toInt(), 6);
            QCOMPARE(metrics["documents"].toArray().at(2).toObject()["two_line_height"].toDouble(), 83.1875);
            QCOMPARE(result->height(), int(ui::metrics::list::threeLines));
        }
        QVERIFY(click(canvas, result));
        QTRY_VERIFY(!host.session()->busy());
        host.shutdown();
        QTRY_VERIFY(host.idle());
    }
    void chapterRowNavigatesAndPersists() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        paths.startup = root.path() + "/startup-session";
        paths.active = root.path() + "/active-session.json";
        paths.stateReport = root.path() + "/state.json";
        paths.sessionReport = root.path() + "/session.json";
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto chapters = host.session()->chapters();
        int target = -1;
        for (int index = 0; index < chapters.size(); ++index) {
            if (!chapters[index].position.isEmpty() && chapters[index].page > host.session()->page()) {
                target = index;
                break;
            }
        }
        QVERIFY2(target >= 0, "The fixture needs a chapter beyond the opening page");
        const auto origin = host.session()->state().value("position").toString();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavChapters")));
        auto *choices = host.window()->findChild<QWidget *>("rrChoices");
        QVERIFY(choices);
        QTRY_VERIFY(choices->isVisible());
        auto *row = choices->findChild<QPushButton *>(QString("rrChapter%1").arg(target));
        QVERIFY(row);
        QVERIFY(row->isVisible() && row->isEnabled());
        QVERIFY(click(canvas, row));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy() && host.session()->page() == chapters[target].page, 10000);
        QVERIFY(!choices->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QCOMPARE(host.session()->contentsSelection().current, target);
        const auto destination = host.session()->state().value("position").toString();
        QVERIFY(destination != origin);
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavChapters")));
        QTRY_VERIFY(choices->isVisible());
        row = choices->findChild<QPushButton *>(QString("rrChapter%1").arg(target));
        QVERIFY(row && row->isVisible());
        QVERIFY(row->isChecked());
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QTRY_VERIFY(!host.session()->busy());
        QCOMPARE(host.session()->state().value("position").toString(), destination);
        QCOMPARE(host.session()->contentsSelection().current, target);
    }
    void tabsCloseBookmarks_data() {
        QTest::addColumn<QString>("tab");
        QTest::addColumn<QString>("panel");
        QTest::newRow("chapters") << "rrNavChapters" << "rrChoices";
        QTest::newRow("text") << "rrNavText" << "rrMenu";
        QTest::newRow("layout") << "rrNavLayout" << "rrMenu";
        QTest::newRow("light") << "rrNavLight" << "rrLightPanel";
        QTest::newRow("stats") << "rrNavStats" << "rrChapterGraph";
        QTest::newRow("search") << "rrNavSearch" << "rrSearch";
    }
    void tabsCloseBookmarks() {
        QFETCH(QString, tab);
        QFETCH(QString, panel);
        if (tab == "rrNavLight" && !test::fixtureDeviceControls())
            QSKIP("Frontlight requires the device fixture");
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrBookmarks")));
        auto *list = host.window()->findChild<QWidget *>("rrBookmarkList");
        QVERIFY(list);
        QTRY_VERIFY(list->isVisible());
        auto *button = host.window()->findChild<QPushButton *>(tab);
        QVERIFY(button);
        QVERIFY(button->isVisible() && button->isEnabled());
        QVERIFY(click(canvas, button));
        QVERIFY2(!list->isVisible(), "Opening a navigation tab left the bookmarks list open");
        auto *destination = host.window()->findChild<QWidget *>(panel);
        QVERIFY(destination);
        QTRY_VERIFY(destination->isVisible());
    }
    void bookmarkListFollowsReaderResize() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *bookmarks = host.window()->findChild<QPushButton *>("rrBookmarks");
        QVERIFY(bookmarks);
        QTRY_VERIFY(bookmarks->isVisible());
        QVERIFY(click(canvas, bookmarks));
        auto *list = host.window()->findChild<QWidget *>("rrBookmarkList");
        QVERIFY(list);
        QTRY_VERIFY(list->isVisible());
        const int bottomInset = host.window()->height() - list->geometry().bottom() - 1;
        const int top = list->y();
        host.window()->setFixedSize(1400, 1200);
        QCOMPARE(list->geometry(), QRect(0, top, host.window()->width(), host.window()->height() - top - bottomInset));
    }
    void websiteRequiresConfirmation() {
        QWidget window;
        window.resize(nativeSize);
        window.show();
        WebsiteDialog panel(&window);
        int opened = 0, dismissed = 0;
        QUrl destination;
        panel.browserAvailable = [] {
            return true;
        };
        panel.openBrowser = [&](const QUrl &url) {
            ++opened;
            destination = url;
            return true;
        };
        panel.dismissed = [&] {
            ++dismissed;
            panel.hide();
        };
        LinkDetails details;
        details.kind = LinkKind::Website;
        details.target = "https://example.com/book?q=a&b=2";
        panel.open(details.target);
        auto *yes = panel.findChild<QPushButton *>("rrLinkOpen");
        auto *no = panel.findChild<QPushButton *>("rrLinkNo");
        QVERIFY(yes && no);
        QVERIFY(yes->isEnabled());
        QVERIFY(no->isVisible());
        QCOMPARE(yes->text(), strings::get("link_panel.open_browser"));
        QCOMPARE(opened, 0);
        no->click();
        QCOMPARE(opened, 0);
        QCOMPARE(dismissed, 1);
        panel.open(details.target);
        yes->click();
        QCOMPARE(opened, 1);
        QCOMPARE(destination, QUrl(details.target));
        QCOMPARE(dismissed, 2);
        // Capability may disappear while the confirmation stays open.
        panel.open(details.target);
        panel.browserAvailable = [] {
            return false;
        };
        yes->click();
        QCOMPARE(opened, 1);
        QCOMPARE(dismissed, 2);
        panel.open(details.target);
        QVERIFY(!yes->isEnabled());
        panel.browserAvailable = [] {
            return true;
        };
        for (const auto &url : {"file:///tmp/book", "javascript:alert(1)", "https:///missing-host"}) {
            details.target = url;
            panel.open(details.target);
            QVERIFY(!yes->isEnabled());
            yes->click();
            QCOMPARE(opened, 1);
        }
        details.target = "https://example.com/" + QString(6000, 'a');
        panel.open(details.target);
        auto *scroll = panel.findChild<QScrollArea *>("rrBrowserUrlScroll");
        QVERIFY(scroll);
        QVERIFY(scroll->isVisible());
        QVERIFY(window.rect().contains(panel.card()->geometry()));
        auto *address = panel.findChild<QLabel *>("rrBrowserUrl");
        QVERIFY(address);
        QCOMPARE(address->text(), details.target);
        QVERIFY(yes->geometry().top() > scroll->geometry().bottom());
        QVERIFY(no->geometry().top() > scroll->geometry().bottom());
        panel.hide();
        LinkPanel footnote(&window);
        details.kind = LinkKind::Footnote;
        details.target = "#note";
        details.html = QString("<p>Footnote text.</p>").repeated(80);
        footnote.open(details, QRect(0, 0, 20, 20));
        auto *pager = footnote.findChild<QWidget *>("rrLinkPages");
        QVERIFY(pager);
        QVERIFY(pager->isVisible());
        auto *passage = footnote.findChild<QPushButton *>("rrLinkOpen");
        QVERIFY(passage);
        QVERIFY(passage->geometry().bottom() < pager->parentWidget()->geometry().top());
    }

    void revealLinksWithoutReflow() {
        const auto originalPreferences = storage::readJson(testPaths().preferences());
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(qEnvironmentVariable("KOBALT_FIXTURES") + "/links.epub", nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *indicator = host.window()->findChild<QPushButton *>("rrLinkIndicator");
        auto *reveal = host.window()->findChild<QPushButton *>("rrRevealLinks");
        auto *panel = host.window()->findChild<QWidget *>("rrLinkDiscovery");
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(indicator && reveal && panel && page);
        QVERIFY(!host.session()->pageLinks().isEmpty());
        QVERIFY(!indicator->isVisible());
        QVERIFY(!panel->isVisible());
        const int pages = host.session()->pages();
        const auto image = host.session()->image().cacheKey();
        const auto position = host.session()->state().value("position").toString();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(panel->isVisible());
        QCOMPARE(reveal->text(), strings::get("reader.highlight_links").arg(host.session()->pageLinks().size()));
        auto *exitLabel = host.window()->findChild<QLabel *>("rrLinkDiscoveryExit");
        QVERIFY(exitLabel);
        QVERIFY(!exitLabel->isVisible());
        // Dismissing highlights consumes the tap and leaves the toolbars closed.
        QVERIFY(click(canvas, reveal));
        QTRY_VERIFY(exitLabel->isVisible());
        QVERIFY(clickAt(canvas, panel->mapTo(canvas.surface(), panel->rect().center())));
        QTRY_VERIFY(!panel->isVisible());
        QVERIFY(!exitLabel->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrBottomNavigation")->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(reveal->isVisible());
        QVERIFY(click(canvas, reveal));
        QTRY_VERIFY(exitLabel->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(!panel->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrBottomNavigation")->isVisible());
        QCOMPARE(host.session()->state().value("position").toString(), position);
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(reveal->isVisible());
        QVERIFY(click(canvas, reveal));
        QTRY_VERIFY(exitLabel->isVisible());
        QVERIFY(!reveal->isVisible());
        // The mode label takes a reading strip and leaves the book text uncovered.
        const QRect body(page->mapTo(host.window(), QPoint(0, page->topHeight())),
                         QSize(page->width(), page->height() - page->topHeight() - page->bottomHeight()));
        QVERIFY(!QRect(panel->mapTo(host.window(), QPoint()), panel->size()).intersects(body));
        QCOMPARE(host.session()->pages(), pages);
        QCOMPARE(host.session()->image().cacheKey(), image);
        QCOMPARE(host.session()->state().value("position").toString(), position);
        QVERIFY(host.window()->grab().save("native-reader-links-revealed.png"));
        const auto rect = host.session()->pageLinks().first().rectangles.first();
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), rect.center() + QPoint(0, page->topHeight()))));
        QPushButton *dismiss = nullptr;
        QTRY_VERIFY((dismiss = host.window()->findChild<QPushButton *>("rrLinkDismiss")) && dismiss->isVisible());
        // The link opens over the page alone: the toolbars would cover a note's sheet.
        QVERIFY(!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        auto *tabs = host.window()->findChild<QWidget *>("rrBottomNavigation");
        QVERIFY(!tabs || !tabs->isVisible());
        QVERIFY(click(canvas, dismiss));
        QVERIFY(!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        // Settings save the indicator independently of publisher styles.
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QVERIFY(click(canvas, settings));
        auto *option = host.window()->findChild<QCheckBox *>("rrShowLinkIndicator");
        QVERIFY(option);
        QVERIFY(!option->isChecked());
        option->setChecked(true);
        QVERIFY(click(canvas, settings));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(indicator->isVisible());
        QCOMPARE(indicator->accessibleName(),
                 strings::get("reader.highlight_links").arg(host.session()->pageLinks().size()));
        // The indicator sits in the footer strip, not on the book text.
        if (page->showFooter)
            QVERIFY(QRect(page->mapTo(host.window(), page->footerTextRect().topLeft()), page->footerTextRect().size())
                        .contains(QRect(indicator->mapTo(host.window(), QPoint()), indicator->size())));
        QVERIFY(host.window()->grab().save("native-reader-link-indicator.png"));
        // From the indicator, leaving reveal mode returns to the page.
        QVERIFY(click(canvas, indicator));
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(exitLabel->isVisible());
        QVERIFY(!reveal->isVisible());
        QVERIFY(clickAt(canvas, panel->mapTo(canvas.surface(), panel->rect().center())));
        QTRY_VERIFY(!panel->isVisible());
        QTRY_VERIFY(indicator->isVisible());
        QVERIFY(!host.window()->findChild<QWidget *>("rrControlsTop")->isVisible());
        host.session()->go(host.session()->pages() - 1);
        QTRY_VERIFY(!host.session()->busy());
        if (host.session()->pageLinks().isEmpty())
            QVERIFY(!indicator->isVisible());
        QVERIFY(storage::readJson(testPaths().preferences()).value("show_link_indicator").toBool());
        QVERIFY(host.session()->goTo(position));
        QTRY_VERIFY(!host.session()->busy() && host.session()->state().value("position").toString() == position);
        QVERIFY(storage::writeJson(testPaths().preferences(), originalPreferences));
    }
    void followLinkAndReturn() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(qEnvironmentVariable("KOBALT_FIXTURES") + "/links.epub", nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        // Find a link to another passage on the first page.
        QRect target;
        for (const auto &link : host.session()->pageLinks()) {
            bool resolved = false;
            LinkKind kind = LinkKind::None;
            host.session()->linkDetails(link.source, [&](const LinkDetails &details) {
                kind = details.kind;
                resolved = true;
            });
            QTRY_VERIFY(resolved);
            if (kind != LinkKind::Passage)
                continue;
            for (const auto &rect : link.rectangles)
                target = target.united(rect);
            break;
        }
        QVERIFY2(!target.isEmpty(), "The links book has no passage link on its first page");
        const auto origin = host.session()->state().value("position").toString();
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        const QPoint linkPoint = page->mapTo(canvas.surface(), target.center() + QPoint(0, page->topHeight()));
        const auto holdLink = [&] {
            canvas.ensureVisible(linkPoint.x(), linkPoint.y(), 30, 30);
            QCoreApplication::processEvents();
            const auto at = canvas.surface()->mapTo(&canvas, linkPoint);
            QTest::mousePress(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at);
            QTest::qWait(ui::metrics::lookup::holdMilliseconds + 200);
            QTest::mouseRelease(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at);
        };
        holdLink();
        QPushButton *go = nullptr;
        QTRY_VERIFY((go = host.window()->findChild<QPushButton *>("rrLinkGo")) && go->isVisible());
        auto *actions = host.window()->findChild<QWidget *>("rrLinkActions");
        QVERIFY(actions);
        QVERIFY(!actions->findChild<QLabel *>("rrLinkHeading"));
        QCOMPARE(actions->findChildren<QPushButton *>().size(), 3);
        QCOMPARE(go->height(), int(ui::metrics::links::actionHeight));
        auto *pointer = actions->findChild<QWidget *>("rrAnchorPointer");
        QVERIFY(pointer && pointer->isVisible());
        QVERIFY(actions->rect().contains(pointer->geometry()));
        QCOMPARE(host.session()->state().value("position").toString(), origin);
        auto *dictionary = host.window()->findChild<QPushButton *>("rrLinkDictionary");
        QVERIFY(dictionary);
        QVERIFY(click(canvas, dictionary));
        auto *definition = host.window()->findChild<QWidget *>("rrDictionary");
        QVERIFY(definition);
        QTRY_VERIFY(definition->isVisible());
        QCOMPARE(host.session()->state().value("position").toString(), origin);
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() - 80, nativeSize.height() / 2)));
        QTRY_VERIFY(!definition->isVisible());
        holdLink();
        QTRY_VERIFY((go = host.window()->findChild<QPushButton *>("rrLinkGo")) && go->isVisible());
        QVERIFY(host.window()->grab().save("native-reader-link-actions.png"));
        QVERIFY(click(canvas, go));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state().value("position").toString() != origin, 10000);
        auto *hiddenReturn = host.window()->findChild<QPushButton *>("rrNavLinkReturn");
        QVERIFY(hiddenReturn);
        QVERIFY(!hiddenReturn->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QPushButton *back = nullptr;
        QTRY_VERIFY((back = host.window()->findChild<QPushButton *>("rrNavLinkReturn")) && back->isVisible());
        QCOMPARE(back->text(), strings::get("reader.return_page").arg(1));
        QVERIFY(static_cast<ui::CaptionButton *>(back)->caption().startsWith("You jumped from "));
        // The destination has a link of its own: the links row stacks above
        // the return row instead of covering it.
        QVERIFY(!host.session()->pageLinks().isEmpty());
        auto *discovery = host.window()->findChild<QWidget *>("rrLinkDiscovery");
        QVERIFY(discovery);
        QTRY_VERIFY(discovery->isVisible());
        const QRect discoveryBounds(discovery->mapTo(host.window(), QPoint()), discovery->size());
        const QRect returnBounds(back->mapTo(host.window(), QPoint()), back->size());
        QVERIFY(!discoveryBounds.intersects(returnBounds));
        QVERIFY(discoveryBounds.bottom() < returnBounds.top());
        QVERIFY(host.window()->grab().save("native-reader-link-return.png"));
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(!back->isVisible());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QTRY_VERIFY(back->isVisible());
        QVERIFY(click(canvas, back));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state().value("position").toString() == origin, 10000);
        QVERIFY(!back->isVisible());
        // A normal tap on the same link belongs to the reading zones.
        QVERIFY(clickAt(canvas, linkPoint));
        QTRY_VERIFY(!host.session()->busy());
        actions = host.window()->findChild<QWidget *>("rrLinkActions");
        QVERIFY(!actions || !actions->isVisible());
        auto *controls = host.window()->findChild<QWidget *>("rrControlsTop");
        if (linkPoint.x() > nativeSize.width() * .33 && linkPoint.x() < nativeSize.width() * .67)
            QTRY_VERIFY(controls->isVisible());
        else
            QTRY_COMPARE(host.session()->page(), linkPoint.x() <= nativeSize.width() * .33 ? 0 : 1);
        if (controls->isVisible())
            QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        host.session()->goTo(origin);
        QTRY_VERIFY(!host.session()->busy() && host.session()->state().value("position").toString() == origin);
        holdLink();
        QTRY_VERIFY((go = host.window()->findChild<QPushButton *>("rrLinkGo")) && go->isVisible());
        QVERIFY(click(canvas, go));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *stay = host.window()->findChild<QPushButton *>("rrNavLinkStay");
        QVERIFY(stay);
        QTRY_VERIFY(stay->isVisible());
        const auto destination = host.session()->state().value("position").toString();
        QVERIFY(click(canvas, stay));
        QTRY_VERIFY(!back->isVisible());
        QCOMPARE(host.session()->state().value("position").toString(), destination);
    }
    void dictionarySuccessChoicePersists() {
        struct LookupState {
            QString selected, word, book;
            int selections = 0, lookups = 0;
        };
        class SuccessfulDictionary final : public DictionaryService {
        public:
            explicit SuccessfulDictionary(std::shared_ptr<LookupState> state) : state(std::move(state)) {}
            QVector<DictionaryChoice> choices() const override {
                return {{"first", "First dictionary", "en"}, {"second", "Second dictionary", "en"}};
            }
            bool select(const QString &book, const QString &id, QString *) override {
                if (id != "first" && id != "second")
                    return false;
                selected = id;
                state->selected = id;
                state->book = book;
                ++state->selections;
                return true;
            }
            DictionaryResult lookup(const QString &book, const QString &word) override {
                state->word = word;
                state->book = book;
                ++state->lookups;
                DictionaryResult result;
                result.word = word;
                result.dictionaryId = selected;
                result.dictionary = selected == "first" ? "First dictionary" : "Second dictionary";
                result.received = result.restored = true;
                if (selected == "first") {
                    for (int i = 0; i < 100; ++i)
                        result.html += QString("<p><b>Entry %1</b>: A successful rich-text definition for %2.</p>")
                                           .arg(i)
                                           .arg(word.toHtmlEscaped());
                } else {
                    result.html = "<p><b>Second entry</b>: A short successful definition.</p>";
                }
                return result;
            }

        private:
            std::shared_ptr<LookupState> state;
            QString selected = "first";
        };
        QTemporaryDir root;
        QVERIFY(root.isValid());
        storage::Paths paths{root.path()};
        paths.startup = root.path() + "/startup-session";
        paths.active = root.path() + "/active-session.json";
        paths.stateReport = root.path() + "/state.json";
        paths.sessionReport = root.path() + "/session.json";
        auto lookup = std::make_shared<LookupState>();
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.configureServices = [lookup](ReaderHostServices &services) {
            services.downloadableDictionaries = false;
            services.dictionary = [lookup] {
                return std::make_unique<SuccessfulDictionary>(lookup);
            };
        };
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        {
            test::ReaderCanvas canvas(host.window(), nativeSize);
            canvas.resize(900, 900);
            canvas.show();
            canvas.setUpdatesEnabled(true);
            QVERIFY(QTest::qWaitForWindowExposed(&canvas));
            QTRY_VERIFY(!host.session()->busy());
            const int before = host.session()->page();
            const QPoint word(nativeSize.width() / 2, nativeSize.height() / 2);
            canvas.ensureVisible(word.x(), word.y(), 30, 30);
            QCoreApplication::processEvents();
            const QPoint at = canvas.surface()->mapTo(&canvas, word);
            QTest::mousePress(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at);
            QTest::qWait(ui::metrics::lookup::holdMilliseconds + 200);
            QTest::mouseRelease(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at);
            auto *panel = host.window()->findChild<QWidget *>("rrDictionary");
            QVERIFY(panel);
            QTRY_VERIFY_WITH_TIMEOUT(panel->isVisible(), 10000);
            QCOMPARE(lookup->lookups, 1);
            QVERIFY(!lookup->word.isEmpty());
            QCOMPARE(panel->accessibleName(), strings::get("dictionary_panel.definition_for").arg(lookup->word));
            auto *definition = dynamic_cast<ui::PagedText *>(panel->findChild<QWidget *>("rrDefinition"));
            QVERIFY(definition);
            QVERIFY(definition->pages() > 1);
            QCOMPARE(definition->page(), 0);
            const auto firstPage = definition->grab().toImage();
            QVERIFY(click(canvas, panel->findChild<QPushButton *>("rrDictionaryNext")));
            QCOMPARE(definition->page(), 1);
            QVERIFY(definition->grab().toImage() != firstPage);
            auto *selector = panel->findChild<QPushButton *>("rrDictionarySelect");
            QVERIFY(selector);
            QCOMPARE(selector->text(), QString("First dictionary"));
            QVERIFY(click(canvas, selector));
            auto *choices = panel->findChild<QWidget *>("rrDictionaryChoices");
            QVERIFY(choices && choices->isVisible());
            auto *first = choices->findChild<QPushButton *>("rrDictionaryChoice0");
            auto *second = choices->findChild<QPushButton *>("rrDictionaryChoice1");
            QVERIFY(first && second);
            QVERIFY(first->isChecked());
            QVERIFY(!second->isChecked());
            QVERIFY(click(canvas, second));
            QVERIFY(!choices->isVisible());
            QCOMPARE(lookup->selected, QString("second"));
            QCOMPARE(lookup->selections, 1);
            QCOMPARE(lookup->lookups, 2);
            QCOMPARE(selector->text(), QString("Second dictionary"));
            QCOMPARE(definition->pages(), 1);
            QCOMPARE(definition->page(), 0);
            QCOMPARE(storage::readJson(paths.bookState(lookup->book)).value("dictionary_id").toString(),
                     QString("second"));
            QVERIFY(click(canvas, panel->findChild<QPushButton *>("rrDictionaryClose")));
            QVERIFY(!panel->isVisible());
            QCOMPARE(host.session()->page(), before);
            host.shutdown();
            QTRY_VERIFY(host.idle());
        }
        lookup->selected.clear();
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QCOMPARE(lookup->selected, QString("second"));
        QCOMPARE(lookup->selections, 2);
    }
    void dictionaryLookupDismisses() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const int before = host.session()->page();
        // A long press on body text looks the word up. The host has no
        // dictionary, so the panel opens with its error.
        const QPoint word(nativeSize.width() / 2, nativeSize.height() / 2);
        canvas.ensureVisible(word.x(), word.y(), 30, 30);
        QCoreApplication::processEvents();
        const QPoint at = canvas.surface()->mapTo(&canvas, word);
        QTest::mousePress(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::qWait(ui::metrics::lookup::holdMilliseconds + 200);
        QTest::mouseRelease(canvas.windowHandle(), Qt::LeftButton, Qt::NoModifier, at);
        auto *panel = host.window()->findChild<QWidget *>("rrDictionary");
        QVERIFY(panel);
        QTRY_VERIFY_WITH_TIMEOUT(panel->isVisible(), 10000);
        // The next tap closes the lookup instead of turning the page.
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() - 80, nativeSize.height() / 2)));
        QTRY_VERIFY(!panel->isVisible());
        QTest::qWait(300);
        QCOMPARE(host.session()->page(), before);
    }
    void staleSearchReplyIgnored() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *search = host.window()->findChild<QPushButton *>("rrNavSearch");
        QVERIFY(search);
        QTRY_VERIFY(search->isVisible());
        QVERIFY(click(canvas, search));
        auto *query = host.window()->findChild<QLineEdit *>("rrSearchQuery");
        QVERIFY(query);
        QTRY_VERIFY(query->isVisible());
        // A book-wide search with many matches, replaced by one with none in
        // the same GUI turn, so no reply to the first can arrive in between.
        query->setText("chapter");
        emit query->returnPressed();
        query->setText("zqxjv");
        emit query->returnPressed();
        auto *status = host.window()->findChild<QLabel *>("rrSearchStatus");
        QVERIFY(status);
        const auto none = strings::get("search_panel.no_matches_found");
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), none, 10000);
        // The engine worker runs jobs in order. Once a later request answers,
        // every reply to the first search has been delivered or dropped.
        bool drained = false;
        host.session()->pageSummary(host.session()->page(), [&](QString, QString) {
            drained = true;
        });
        QTRY_VERIFY(drained);
        QCoreApplication::processEvents();
        QCOMPARE(status->text(), none);
        auto *row = host.window()->findChild<QPushButton *>("rrSearchResult0");
        QVERIFY(!row || !row->isVisible());
    }
    void searchResultThenReturnToStart() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto origin = host.session()->state().value("position").toString();
        const auto search = [&](const QString &text) {
            QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
            auto *open = host.window()->findChild<QPushButton *>("rrNavSearch");
            QVERIFY(open);
            QTRY_VERIFY(open->isVisible());
            QVERIFY(click(canvas, open));
            auto *query = host.window()->findChild<QLineEdit *>("rrSearchQuery");
            QVERIFY(query);
            QTRY_VERIFY(query->isVisible());
            query->setText(text);
            emit query->returnPressed();
            QPushButton *result = nullptr;
            QTRY_VERIFY_WITH_TIMEOUT(
                (result = host.window()->findChild<QPushButton *>("rrSearchResult0")) && result->isVisible(), 10000);
        };
        // A result on a later page moves the reader.
        search("Chapter 3");
        auto *result = host.window()->findChild<QPushButton *>("rrSearchResult0");
        QVERIFY(click(canvas, result));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state().value("position").toString() != origin, 10000);
        // Searching again offers the position where the first search started.
        search("chapter");
        QPushButton *back = nullptr;
        QTRY_VERIFY((back = host.window()->findChild<QPushButton *>("rrSearchOriginal")) && back->isVisible());
        QVERIFY(click(canvas, back));
        QTRY_VERIFY_WITH_TIMEOUT(
            !host.session()->busy() && host.session()->state().value("position").toString() == origin, 10000);
        // Returning ends that search trip. The next search starts from here
        // and has no earlier position to go back to.
        search("chapter");
        auto *again = host.window()->findChild<QPushButton *>("rrSearchOriginal");
        QVERIFY(again);
        QVERIFY(!again->isVisible());
    }
    void bookmarkToggleAndFailedSave() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        // Use a page with no bookmark from earlier tests, away from the last
        // page, where leaving the book asks whether it is finished.
        const int start = host.session()->page();
        QVERIFY(host.session()->go(host.session()->pages() / 2));
        QTRY_VERIFY(!host.session()->busy());
        const auto corner = [&] {
            return page->mapTo(canvas.surface(), page->bookmarkCorner().center());
        };
        QTRY_VERIFY(!page->bookmarked);
        QVERIFY(clickAt(canvas, corner()));
        QTRY_VERIFY(page->bookmarked);
        QVERIFY(clickAt(canvas, corner()));
        QTRY_VERIFY(!page->bookmarked);
        // A directory where bookmarks.json belongs makes every save fail, even
        // for root. The failed add must keep the page unmarked and show an error.
        // The reader names the folder from the book's identity and fingerprint.
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        const QString folder =
            testPaths().annotations() + '/' + BookmarkStore::bookKey(identity, host.session()->sourceHash());
        QVERIFY2(QFile::exists(folder + "/bookmarks.json"), "The earlier toggle did not save this book's bookmarks");
        BlockedFile blocked(folder + "/bookmarks.json");
        QVERIFY(blocked.active());
        QFile book(folder + "/book.json");
        QVERIFY(book.open(QIODevice::ReadOnly));
        const auto bookBytes = book.readAll();
        book.close();
        QVERIFY(clickAt(canvas, corner()));
        QPointer<QWidget> error;
        QTRY_VERIFY((error = host.window()->findChild<QWidget *>("rrBookmarkError")) && error->isVisible());
        QVERIFY(!page->bookmarked);
        auto *message = error->findChild<QLabel *>("rrBookmarkErrorMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("bookmarks.save_failed"));
        host.window()->pageButton(1);
        QTest::qWait(30);
        QCOMPARE(host.session()->page(), host.session()->pages() / 2);
        QVERIFY(QFileInfo(folder + "/bookmarks.json").isDir());
        QVERIFY(QDir(folder + "/bookmarks.json").isEmpty());
        QVERIFY(book.open(QIODevice::ReadOnly));
        QCOMPARE(book.readAll(), bookBytes);
        book.close();
        QVERIFY(click(canvas, error->findChild<QPushButton *>("rrBookmarkErrorConfirm")));
        QTRY_VERIFY(!error || !error->isVisible());
        QVERIFY(blocked.restore());
        QVERIFY(clickAt(canvas, corner()));
        QTRY_VERIFY(page->bookmarked);
        QVERIFY(clickAt(canvas, corner()));
        QTRY_VERIFY(!page->bookmarked);
        QVERIFY(host.session()->go(start));
        QTRY_VERIFY(!host.session()->busy());
    }
    void bookmarkLoadFailureKeepsSavedFile() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        storage::Paths paths{root.path()};
        paths.startup = root.path() + "/startup-session";
        paths.active = root.path() + "/active-session.json";
        paths.stateReport = root.path() + "/state.json";
        paths.sessionReport = root.path() + "/session.json";
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        const auto hash = host.session()->sourceHash();
        host.shutdown();
        QTRY_VERIFY(host.idle());
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        BookmarkStore store(paths.annotations(), identity, hash, "Private title");
        QVERIFY(QDir().mkpath(store.folder()));
        const auto path = store.folder() + "/bookmarks.json";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original("damaged but recoverable bookmark bytes");
        QCOMPARE(file.write(original), original.size());
        file.close();
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->bookmarkCorner().center())));
        QWidget *error = nullptr;
        QTRY_VERIFY((error = host.window()->findChild<QWidget *>("rrBookmarkError")) && error->isVisible());
        auto *message = error->findChild<QLabel *>("rrBookmarkErrorMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("bookmarks.load_failed"));
        QVERIFY(!page->bookmarked);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
    }
    void closeWithPendingBookmark() {
        // Other tests can leave bookmarks that resolve to this page after reflow.
        QTemporaryDir root;
        QVERIFY(root.isValid());
        storage::Paths paths{root.path()};
        paths.startup = root.path() + "/startup-session";
        paths.active = root.path() + "/active-session.json";
        paths.stateReport = root.path() + "/state.json";
        paths.sessionReport = root.path() + "/session.json";
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened), closed(&host, &test::ReaderHost::closed);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        // A page turn resolves the bookmark pages for this layout.
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        QTRY_VERIFY(!page->bookmarked);
        const auto corner = page->mapTo(canvas.surface(), page->bookmarkCorner().center());
        // A full add and remove proves that bookmark pages are resolved, so the
        // next tap is accepted instead of refused.
        QVERIFY(clickAt(canvas, corner));
        QTRY_VERIFY(page->bookmarked);
        QVERIFY(clickAt(canvas, corner));
        QTRY_VERIFY(!page->bookmarked);
        // Adding a bookmark waits for the engine. Closing in that window must
        // drop the reply safely.
        QVERIFY(clickAt(canvas, corner));
        QVERIFY(!page->bookmarked);
        host.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY(host.idle());
        QVERIFY(!host.window());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
    }
    void pageGestureBurstKeepsOnePendingTurn() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        QVERIFY(host.session()->pages() > 6);
        QVERIFY(host.session()->go(2));
        QTRY_VERIFY(!host.session()->busy());
        const QPoint forward(page->width() * 9 / 10, page->height() / 2);
        const QPoint center(page->width() / 2, page->height() / 2);
        // Call completed gestures in one GUI turn. The worker may finish, but
        // its callback cannot run until every input has reached the reader.
        page->tapped(forward);
        QVERIFY(host.session()->busy());
        for (int i = 0; i < 100; ++i)
            page->tapped(forward);
        QTRY_COMPARE(host.session()->page(), 4);
        QTRY_VERIFY(!host.session()->busy());
        page->swiped(1);
        page->swiped(1);
        page->swiped(-1);
        QTRY_VERIFY(!host.session()->busy() && host.session()->page() == 4);
        page->tapped(forward);
        page->tapped(forward);
        page->tapped(center);
        QTRY_COMPARE(host.session()->page(), 5);
        QTRY_VERIFY(!host.session()->busy());
        QTest::qWait(30);
        QCOMPARE(host.session()->page(), 5);
        // Opening controls discarded the buffered second turn.
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QTRY_VERIFY(settings->isVisible() && settings->isEnabled());
    }

    void finishChoiceSurvivesSleepAndRequiresActiveReader_data() {
        QTest::addColumn<bool>("markFinished");
        QTest::addColumn<bool>("exitRequested");
        QTest::newRow("still-reading") << false << false;
        QTest::newRow("mark-finished") << true << false;
        QTest::newRow("exit-still-reading") << false << true;
        QTest::newRow("exit-mark-finished") << true << true;
    }
    void nativeProgressAutosave_data() {
        QTest::addColumn<bool>("sleep");
        QTest::addColumn<QString>("outcome");
        for (const bool sleep : {false, true}) {
            for (const auto *outcome : {"saved", "newer", "failed"}) {
                const auto name = QString("%1-%2").arg(sleep ? "sleep" : "background", outcome);
                QTest::newRow(qPrintable(name)) << sleep << QString::fromLatin1(outcome);
            }
        }
    }
    void nativeProgressAutosave() {
        QFETCH(bool, sleep);
        QFETCH(QString, outcome);
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QTRY_VERIFY(host.progress->writes > 0);
        auto *timer = host.window()->findChild<QTimer *>("rrNativeProgressTimer");
        QVERIFY(timer);
        QCOMPARE(timer->interval(), 30000);
        const int openingWrites = host.progress->writes;
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QCOMPARE(host.progress->writes, openingWrites);
        QVERIFY(host.session()->go(1));
        QTRY_VERIFY(!host.session()->busy());
        const auto anchor = host.session()->state().value("position").toString();
        QString error;
        const auto expected = host.session()->savedNativePosition(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!expected.isEmpty());
        const double percent = 100.0 * host.session()->page() / host.session()->pages();
        const int writes = host.progress->writes;
        if (outcome == "newer") {
            host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true});
        }
        host.progress->failWrite = outcome == "failed";
        const auto before = host.progress->current;
        if (sleep) {
            host.window()->sleepReader();
        } else {
            QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        }
        QCOMPARE(host.progress->writes, writes + (outcome == "saved" ? 1 : 0));
        if (outcome == "saved") {
            QCOMPARE(host.progress->current.location, expected);
            QCOMPARE(host.progress->percent, percent);
            QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
            QCOMPARE(host.progress->writes, writes + 1);
        } else {
            QVERIFY(host.progress->current == before);
        }
        QCOMPARE(storage::readJson(paths.bookState(identity)).value("position").toString(), anchor);
    }
    void usbFinalSave_data() {
        QTest::addColumn<QString>("outcome");
        for (const auto &outcome : {"saved", "failed", "newer", "timeout"}) {
            QTest::newRow(outcome) << QString::fromLatin1(outcome);
        }
    }
    void usbFinalSave() {
        QFETCH(QString, outcome);
        struct ResetDelay {
            ~ResetDelay() {
                delayUsbEncoding = false;
            }
        } resetDelay;
        QTemporaryDir root;
        auto paths = testPaths();
        paths.directory = root.path();
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        host.positions.encode = usbPosition;
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        QTRY_VERIFY(!host.session()->busy());
        const int writes = host.progress->writes;
        host.progress->failWrite = outcome == "failed";
        if (outcome == "newer") {
            host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true});
        }
        const auto before = host.progress->current;
        usbEncodingReturned = false;
        delayUsbEncoding = outcome == "timeout";
        int completed = 0;
        QElapsedTimer elapsed;
        elapsed.start();
        host.window()->prepareUsb([&] {
            ++completed;
        });
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 3000);
        QVERIFY(elapsed.elapsed() < 2400);
        if (outcome == "timeout") {
            QTRY_VERIFY_WITH_TIMEOUT(usbEncodingReturned.load(), 3000);
            // Drain the late engine callback while the window remains alive.
            QTest::qWait(100);
        }
        delayUsbEncoding = false;
        QCOMPARE(completed, 1);
        QCOMPARE(host.progress->writes, writes + (outcome == "saved" ? 1 : 0));
        if (outcome != "saved") {
            QVERIFY(host.progress->current == before);
        }
        const auto local = storage::readJson(paths.bookState(identity));
        QVERIFY(!local.value("position").toString().isEmpty());
    }
    void finishChoiceSurvivesSleepAndRequiresActiveReader() {
        QFETCH(bool, markFinished);
        QFETCH(bool, exitRequested);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        bool suspended = false;
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.configureServices = [&](ReaderHostServices &services) {
            services.suspended = [&] {
                return suspended;
            };
        };
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(host.session()->pages() - 1));
        QTRY_VERIFY(!host.session()->busy());
        host.window()->pageButton(1);
        QPointer<QWidget> dialog;
        QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrFinishBook")) && dialog->isVisible());
        auto *choice = dialog->findChild<QPushButton *>(markFinished ? "rrMarkFinished" : "rrStillReading");
        QVERIFY(choice);
        if (exitRequested) {
            // Home can request close while a choice is already open.
            // Repeated close requests must keep that choice unresolved.
            QVERIFY(!host.window()->close());
            QVERIFY(!host.window()->close());
            QVERIFY(dialog && dialog->isVisible());
            QVERIFY(!host.idle());
            QCOMPARE(host.progress->current.status, 1);
        }
        suspended = true;
        choice->click();
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(host.progress->current.status, 1);
        suspended = false;
        host.window()->sleepReader();
        QVERIFY(dialog->isHidden());
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        QTRY_VERIFY(dialog && dialog->isVisible());
        choice->click();
        QTRY_VERIFY(!dialog);
        if (markFinished || exitRequested) {
            QTRY_VERIFY_WITH_TIMEOUT(host.idle(), 10000);
            QCOMPARE(host.progress->current.status, markFinished ? 2 : 1);
        } else {
            QVERIFY(!host.idle());
            QCOMPARE(host.progress->current.status, 1);
            host.window()->pageButton(1);
            QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrFinishBook")) && dialog->isVisible());
        }
    }
    void bookmarkRemovalRequiresConfirmation_data() {
        QTest::addColumn<bool>("confirm");
        QTest::newRow("cancel") << false;
        QTest::newRow("remove") << true;
    }
    void bookmarkRemovalRequiresConfirmation() {
        QFETCH(bool, confirm);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        QVERIFY(!page->bookmarked);
        QVERIFY(clickAt(canvas, page->mapTo(canvas.surface(), page->bookmarkCorner().center())));
        QTRY_VERIFY(page->bookmarked);
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        const auto file = paths.annotations() + '/' + BookmarkStore::bookKey(identity, host.session()->sourceHash()) +
                          "/bookmarks.json";
        QFile before(file);
        QVERIFY(before.open(QIODevice::ReadOnly));
        const auto original = before.readAll();
        before.close();
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *bookmarks = host.window()->findChild<QPushButton *>("rrBookmarks");
        QVERIFY(bookmarks);
        QTRY_VERIFY(bookmarks->isVisible());
        QVERIFY(click(canvas, bookmarks));
        auto *remove = host.window()->findChild<QPushButton *>("rrBookmarkRemove0");
        QVERIFY(remove);
        QTRY_VERIFY(remove->isVisible());
        QVERIFY(click(canvas, remove));
        QPointer<QWidget> dialog;
        QTRY_VERIFY((dialog = host.window()->findChild<QWidget *>("rrRemoveBookmark")) && dialog->isVisible());
        QVERIFY(before.open(QIODevice::ReadOnly));
        QCOMPARE(before.readAll(), original);
        before.close();
        auto *choice = dialog->findChild<QPushButton *>(confirm ? "rrConfirmRemoveBookmark" : "rrKeepBookmark");
        QVERIFY(choice);
        QVERIFY(click(canvas, choice));
        QTRY_VERIFY(!dialog);
        QTRY_COMPARE(page->bookmarked, !confirm);
        QVERIFY(before.open(QIODevice::ReadOnly));
        const auto after = before.readAll();
        before.close();
        if (confirm)
            QVERIFY(after != original);
        else
            QCOMPARE(after, original);
        QVERIFY(host.window()->findChild<QWidget *>("rrBookmarkList")->isVisible());
    }
    void progressChoiceKeepAndGo_data() {
        QTest::addColumn<bool>("go");
        QTest::newRow("keep") << false;
        QTest::newRow("go") << true;
    }
    void progressChoiceKeepAndGo() {
        QFETCH(bool, go);
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(0));
        QTRY_VERIFY(!host.session()->busy());
        host.window()->progressTick();
        QTest::qWait(200);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        // Another device saved a position later in the book.
        host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.8.1:7", 200, 1, true});
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        auto *message = card->findChild<QLabel *>("rrSyncedPositionMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("sync.after"));
        const int before = host.session()->page();
        auto *choice = card->findChild<QPushButton *>(go ? "rrGoToSyncedPosition" : "rrKeepPosition");
        QVERIFY(choice);
        QVERIFY(click(canvas, choice));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QTRY_VERIFY(!host.session()->busy());
        if (go)
            QVERIFY(host.session()->page() > before);
        else
            QCOMPARE(host.session()->page(), before);
        // The choice is remembered: the same saved position does not ask again.
        host.window()->progressTick();
        QTest::qWait(200);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }
    void finishedSyncWithoutPositionKeepsLocalReading() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto paths = testPaths();
        paths.directory = root.path();
        test::ReaderHost host(paths, testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const auto original = host.session()->state().value("position");
        host.progress->publish({identity, {}, 200, 2, true});
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible());
        auto *message = card->findChild<QLabel *>("rrSyncedPositionMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("sync.finished"));
        host.window()->sleepReader();
        QVERIFY(!card->isVisible());
        host.window()->sleepViewReady();
        host.window()->wakeReader();
        host.window()->progressTick();
        QTRY_VERIFY(card->isVisible());
        QCOMPARE(host.session()->state().value("position"), original);
        auto *go = card->findChild<QPushButton *>("rrGoToSyncedPosition");
        QVERIFY(go);
        QVERIFY(go->isHidden());
        auto *keep = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QVERIFY(keep->property("reader_primary_action").toBool());
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QCOMPARE(host.session()->state().value("position"), original);
        host.window()->progressTick();
        QTest::qWait(200);
        QVERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
        QCOMPARE(host.session()->state().value("position"), original);
    }
    void progressChoiceForOlderAlert() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(host.session()->go(0));
        QTRY_VERIFY(!host.session()->busy());
        host.progress->publish({identity, "OEBPS/c2.xhtml#kobo.5.1:3", 200, 1, true});
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        // A newer position arrives while the first alert is open. Keeping the
        // first one must not accept the newer one, so the reader asks again.
        // A tick while the engine is busy hides the alert until the next tick,
        // so tick only when it is idle.
        QPointer<QWidget> first(card);
        // Positions no other test has accepted, so neither counts as already chosen.
        host.progress->publish({identity, "OEBPS/c3.xhtml#kobo.12.1:3", 300, 1, true});
        QTRY_VERIFY(!host.session()->busy());
        host.window()->progressTick();
        QTRY_VERIFY(first && first->isVisible());
        auto *keep = first->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!first);
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        auto *keepNewer = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keepNewer);
        QVERIFY(click(canvas, keepNewer));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }
    void progressChoiceUnavailablePosition() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        host.progress = std::make_shared<test::HostProgress>();
        host.progress->current = {identity, "OEBPS/c1.xhtml", 100, 1, true};
        host.positions = nickel::positionCodec();
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const int before = host.session()->page();
        // A saved position the book cannot place still offers the choice, and
        // going there reports that it is unavailable instead of moving.
        host.progress->publish({identity, "OEBPS/missing.xhtml#kobo.1.1:0", 200, 1, true});
        host.window()->progressTick();
        QWidget *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((card = host.window()->findChild<QWidget *>("rrSyncedPosition")) && card->isVisible(),
                                 10000);
        auto *message = card->findChild<QLabel *>("rrSyncedPositionMessage");
        QVERIFY(message);
        QCOMPARE(message->text(), strings::get("sync.unlocated"));
        auto *go = card->findChild<QPushButton *>("rrGoToSyncedPosition");
        QVERIFY(go);
        QVERIFY(click(canvas, go));
        QTRY_COMPARE_WITH_TIMEOUT(message->text(), strings::get("sync.position_unavailable"), 10000);
        QCOMPARE(host.session()->page(), before);
        auto *keep = card->findChild<QPushButton *>("rrKeepPosition");
        QVERIFY(keep);
        QTRY_VERIFY(keep->isEnabled());
        // Only the acknowledgement is left; the failed choice is not shown disabled.
        QVERIFY(go->isHidden());
        QCOMPARE(keep->text(), strings::get("common.ok"));
        QVERIFY(keep->property("reader_primary_action").toBool());
        QVERIFY(click(canvas, keep));
        QTRY_VERIFY(!host.window()->findChild<QWidget *>("rrSyncedPosition"));
    }
    void bookmarksMergeAfterLayoutChange() {
        // Start without bookmarks from earlier tests, which would merge too.
        const auto identity = QUrl::fromLocalFile(QFileInfo(fixture()).absoluteFilePath()).toString();
        for (const auto &entry : QDir(testPaths().annotations()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (storage::readJson(entry.filePath() + "/book.json").value("identity").toString() == identity)
                QVERIFY(QDir(entry.filePath()).removeRecursively());
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        const QString file = testPaths().annotations() + '/' +
                             BookmarkStore::bookKey(identity, host.session()->sourceHash()) + "/bookmarks.json";
        const auto stored = [&] {
            return storage::readJson(file).value("bookmarks").toArray().size();
        };
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        const int start = host.session()->page();
        // Far from the bookmarks that earlier tests leave near the start.
        const int first = host.session()->pages() * 2 / 3;
        const auto corner = [&] {
            return page->mapTo(canvas.surface(), page->bookmarkCorner().center());
        };
        // Bookmark two neighbouring pages.
        for (int target : {first, first + 1}) {
            QVERIFY(host.session()->go(target));
            QTRY_VERIFY(!host.session()->busy());
            QTRY_VERIFY(!page->bookmarked);
            QVERIFY(clickAt(canvas, corner()));
            QTRY_VERIFY(page->bookmarked);
        }
        const auto before = stored();
        QCOMPARE(before, 2);
        // The smallest text fits both pages on one. The two bookmarks become one.
        const int pixels = host.session()->fontSize(), spacing = host.session()->spacing();
        const int pagesBefore = host.session()->pages();
        QVERIFY(host.session()->style(minimumFontPixels(), 100));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy() && host.session()->pages() < pagesBefore / 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(stored(), before - 1, 10000);
        QStringList positions;
        for (const auto &value : storage::readJson(file).value("bookmarks").toArray())
            positions << value.toObject().value("position").toString();
        int merged = -1;
        bool resolved = false;
        host.session()->bookmarkPages(positions, [&](QVector<int> pages) {
            for (int value : pages)
                merged = qMax(merged, value);
            resolved = true;
        });
        QTRY_VERIFY(resolved);
        QVERIFY2(merged > 0, "No page holds the merged bookmark");
        QVERIFY(host.session()->go(merged));
        QTRY_VERIFY(!host.session()->busy());
        // Removing the merged bookmark removes it for the original layout too.
        QVERIFY(clickAt(canvas, corner()));
        QTRY_VERIFY(!page->bookmarked);
        QTRY_COMPARE(stored(), before - 2);
        QVERIFY(host.session()->style(pixels, spacing));
        QTRY_VERIFY_WITH_TIMEOUT(!host.session()->busy() && host.session()->pages() == pagesBefore, 10000);
        for (int target : {first, first + 1}) {
            QVERIFY(host.session()->go(target));
            QTRY_VERIFY(!host.session()->busy());
            QTest::qWait(200);
            QVERIFY(!page->bookmarked);
        }
        QVERIFY(host.session()->go(start));
        QTRY_VERIFY(!host.session()->busy());
    }
    void exitThroughReaderAndReopen() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened), closed(&host, &test::ReaderHost::closed);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *exit = host.window()->findChild<QPushButton *>("rrReturn");
        QVERIFY(exit);
        QTRY_VERIFY(exit->isVisible() && exit->isEnabled());
        QVERIFY(click(canvas, exit));
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY(host.idle());
        QVERIFY(!host.session());
        QVERIFY(!host.window());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        QVERIFY(host.session()->savedStateMatched());
        host.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 2, 10000);
    }
    void lightGesturesRestoreAndRejectBlockedStarts_data() {
        QTest::addColumn<int>("minimum");
        QTest::addColumn<int>("maximum");
        QTest::addColumn<int>("step");
        QTest::addColumn<int>("initial");
        QTest::addColumn<int>("fortyPercent");
        QTest::addColumn<int>("sixtyPercent");
        QTest::newRow("percent") << 0 << 100 << 1 << 30 << 40 << 60;
        QTest::newRow("native-temperature") << 1000 << 6000 << 500 << 2500 << 3000 << 4000;
        QTest::newRow("closest-native-step") << 0 << 100 << 6 << 30 << 42 << 60;
    }
    void lightGesturesRestoreAndRejectBlockedStarts() {
        QFETCH(int, minimum);
        QFETCH(int, maximum);
        QFETCH(int, step);
        QFETCH(int, initial);
        QFETCH(int, fortyPercent);
        QFETCH(int, sixtyPercent);
        struct State {
            int warmth, storedWarmth, minimum, maximum, step;
            int saves = 0, brightness = 50, originalBrightness = -1;
            bool automatic = true, failSave = false, suspended = false;
        } state{initial, initial, minimum, maximum, step};
        class Light final : public LightControls {
        public:
            explicit Light(State &state) : state(state) {}
            bool initialize() override {
                return true;
            }
            bool hasWarmth() const override {
                return true;
            }
            int warmth() const override {
                return state.warmth;
            }
            int warmthMinimum() const override {
                return state.minimum;
            }
            int warmthMaximum() const override {
                return state.maximum;
            }
            int warmthStep() const override {
                return state.step;
            }
            bool automaticWarmth() const override {
                return state.automatic;
            }
            int setWarmth(int value) override {
                state.automatic = false;
                return state.warmth = qBound(state.minimum, value, state.maximum);
            }
            bool saveWarmth() override {
                ++state.saves;
                if (state.failSave)
                    return false;
                state.storedWarmth = state.warmth;
                return true;
            }
            bool setAutomaticWarmth(bool enabled) override {
                state.automatic = enabled;
                return true;
            }
            bool beginGesture() override {
                state.originalBrightness = state.brightness;
                return true;
            }
            int setGestureLevel(int value) override {
                return state.brightness = qBound(0, value, 100);
            }
            void finishGesture(bool save) override {
                if (!save)
                    state.brightness = state.originalBrightness;
                state.originalBrightness = -1;
            }
            int brightness() const override {
                return state.brightness;
            }
            bool gesturing() const override {
                return state.originalBrightness >= 0;
            }

        private:
            State &state;
        };
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        host.configureServices = [&](ReaderHostServices &services) {
            services.lightAvailable = [] {
                return true;
            };
            services.light = [&](QWidget *) {
                return std::make_unique<Light>(state);
            };
            services.suspended = [&] {
                return state.suspended;
            };
        };
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        auto *page = host.window()->findChild<ReadingPage *>("rrPage");
        auto *notice = host.window()->findChild<QWidget *>("rrBrightnessNotice");
        QVERIFY(page);
        QVERIFY(notice);
        state.suspended = true;
        QVERIFY(!page->lightStarted(true));
        QVERIFY(!page->lightStarted(false));
        QCOMPARE(state.warmth, initial);
        QCOMPARE(state.originalBrightness, -1);
        state.suspended = false;
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(!page->lightStarted(true));
        QVERIFY(!page->lightStarted(false));
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        host.window()->setProperty("reader_full_flash", false);
        QVERIFY(page->lightStarted(true));
        page->lightFinished(false);
        QVERIFY(!host.window()->property("reader_full_flash").toBool());
        QVERIFY(page->lightStarted(true));
        page->lightChanged(4.0 / 150);
        QCOMPARE(state.warmth, initial);
        page->lightChanged(6.0 / 150);
        QCOMPARE(state.warmth, fortyPercent);
        page->lightChanged(.2);
        QCOMPARE(state.warmth, sixtyPercent);
        page->lightChanged(2);
        QCOMPARE(state.warmth, maximum);
        page->lightChanged(-2);
        QCOMPARE(state.warmth, minimum);
        page->lightChanged(-6.0 / 150);
        QVERIFY(state.warmth < initial);
        QVERIFY(!host.window()->property("reader_full_flash").toBool());
        QVERIFY(!state.automatic);
        QVERIFY(notice->isVisible());
        page->lightFinished(false);
        QCOMPARE(state.warmth, initial);
        QVERIFY(state.automatic);
        QCOMPARE(state.saves, 0);
        QVERIFY(notice->isHidden());
        QVERIFY(host.window()->property("reader_full_flash").toBool());
        host.window()->setProperty("reader_full_flash", false);
        QVERIFY(page->lightStarted(false));
        page->lightChanged(4.0 / 150);
        QCOMPARE(state.brightness, 54);
        page->lightChanged(.2);
        QCOMPARE(state.brightness, 80);
        QVERIFY(!host.window()->property("reader_full_flash").toBool());
        page->lightFinished(false);
        QCOMPARE(state.brightness, 50);
        QVERIFY(notice->isHidden());
        QVERIFY(host.window()->property("reader_full_flash").toBool());
        host.window()->setProperty("reader_full_flash", false);
        state.failSave = true;
        QVERIFY(page->lightStarted(true));
        page->lightChanged(.2);
        page->lightFinished(true);
        QCOMPARE(state.saves, 1);
        QCOMPARE(state.storedWarmth, initial);
        QVERIFY(!state.automatic);
        QVERIFY(!host.window()->property("reader_full_flash").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(notice->isHidden(), 5000);
        QVERIFY(host.window()->property("reader_full_flash").toBool());
        state.failSave = false;
        QVERIFY(page->lightStarted(true));
        page->lightChanged(.1);
        page->lightFinished(true);
        QCOMPARE(state.saves, 2);
        QCOMPARE(state.storedWarmth, state.warmth);
        QTRY_VERIFY_WITH_TIMEOUT(notice->isHidden(), 5000);
        host.window()->setProperty("reader_full_flash", false);
        QVERIFY(page->lightStarted(false));
        page->lightChanged(4.0 / 150);
        QCOMPARE(state.brightness, 54);
        page->lightFinished(true);
        QVERIFY(!host.window()->property("reader_full_flash").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(notice->isHidden(), 5000);
        QVERIFY(host.window()->property("reader_full_flash").toBool());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(canvas, host.window()->findChild<QPushButton *>("rrNavLight")));
        auto *lightPanel = host.window()->findChild<QWidget *>("rrLightPanel");
        QVERIFY(lightPanel && lightPanel->isVisible());
        host.window()->setProperty("reader_full_flash", false);
        QEvent update(QEvent::UpdateRequest);
        QApplication::sendEvent(host.window(), &update);
        QCOMPARE(host.window()->property("reader_partial_controls").toRect(), lightPanel->geometry());
        const QRect lightBounds = lightPanel->geometry();
        auto *slider = lightPanel->findChild<QSlider *>("rrWarmth");
        const int inset = ui::metrics::control::sliderInset;
        const QPoint middle(inset + (slider->width() - 2 * inset) / 2, slider->height() / 2);
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, middle);
        QTest::mouseMove(slider, middle + QPoint(ui::px(30), 0));
        QVERIFY(!host.window()->property("reader_full_flash").toBool());
        QCOMPARE(lightPanel->geometry(), lightBounds);
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, middle);
        QVERIFY(!host.window()->property("reader_full_flash").toBool());
    }
    void screenRefreshChoicePersistsAndDismisses() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *settings = host.window()->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QVERIFY(click(canvas, settings));
        auto *refresh = host.window()->findChild<ui::CaptionButton *>("rrScreenRefresh");
        QVERIFY(refresh);
        const auto saved = storage::readJson(testPaths().preferences());
        const QString caption = refresh->caption();
        QVERIFY(click(canvas, refresh));
        auto *popup = host.window()->findChild<ui::ModalOverlay *>("rrScreenRefreshPopup");
        QVERIFY(popup);
        QVERIFY(popup->isVisible());
        QVERIFY(clickAt(canvas, popup->mapTo(canvas.surface(), QPoint(2, 2))));
        QTRY_VERIFY(host.window()->findChild<QWidget *>("rrScreenRefreshPopup") == nullptr);
        QCOMPARE(refresh->caption(), caption);
        QCOMPARE(storage::readJson(testPaths().preferences()), saved);
        const QStringList modes = {"smart", "chapter", "6", "10", "20", "manual"};
        const QStringList labels = {strings::get("settings.refresh_smart"),
                                    strings::get("settings.refresh_every_chapter"),
                                    strings::get("settings.refresh_every_pages").arg(6),
                                    strings::get("settings.refresh_every_pages").arg(10),
                                    strings::get("settings.refresh_every_pages").arg(20),
                                    strings::get("settings.refresh_manually")};
        for (int i = 0; i < modes.size(); ++i) {
            QVERIFY(click(canvas, refresh));
            auto *option = host.window()->findChild<QCheckBox *>("rrRefresh_" + modes[i]);
            QVERIFY(option);
            QVERIFY(click(canvas, option));
            QTRY_VERIFY(host.window()->findChild<QWidget *>("rrScreenRefreshPopup") == nullptr);
            QCOMPARE(refresh->caption(), labels[i].toUpper());
            QCOMPARE(storage::readJson(testPaths().preferences()), saved);
        }
        QVERIFY(click(canvas, settings));
        QTRY_COMPARE(storage::readJson(testPaths().preferences()).value("screen_refresh").toString(),
                     QString("manual"));
        QVERIFY(click(canvas, settings));
        QVERIFY(click(canvas, refresh));
        auto *manual = host.window()->findChild<QCheckBox *>("rrRefresh_manual");
        QVERIFY(manual);
        QVERIFY(manual->isChecked());
        QVERIFY(click(canvas, manual));
        QVERIFY(click(canvas, settings));
        host.shutdown();
        QTRY_VERIFY(host.idle());
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 30000);
        test::ReaderCanvas reopened(host.window(), nativeSize);
        reopened.resize(900, 900);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(reopened, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        QVERIFY(click(reopened, host.window()->findChild<QPushButton *>("rrSettings")));
        refresh = host.window()->findChild<ui::CaptionButton *>("rrScreenRefresh");
        QVERIFY(refresh);
        QCOMPARE(refresh->caption(), labels.last().toUpper());
        QVERIFY(click(reopened, host.window()->findChild<QPushButton *>("rrSettings")));
        auto restored = storage::readJson(testPaths().preferences());
        if (saved.contains("screen_refresh"))
            restored["screen_refresh"] = saved.value("screen_refresh");
        else
            restored.remove("screen_refresh");
        QVERIFY(storage::writeJson(testPaths().preferences(), restored));
    }
    void deviceCapabilities() {
        const bool fixtureEnabled = test::fixtureDeviceControls();
        QCOMPARE(test::hostReaderServices().lightAvailable(), fixtureEnabled);
        auto sensor = test::fixtureOrientationSensor();
        QCOMPARE(bool(sensor), fixtureEnabled);
        if (sensor) {
            QVERIFY(!sensor->isConnectedToBackend());
            QVERIFY(!sensor->isActive());
            QVERIFY(!sensor->reading());
        }
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened);
        host.open(fixture(), nativeSize);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        test::ReaderCanvas canvas(host.window(), nativeSize);
        canvas.resize(900, 900);
        canvas.show();
        canvas.setUpdatesEnabled(true);
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_VERIFY(!host.session()->busy());
        QVERIFY(clickAt(canvas, QPoint(nativeSize.width() / 2, nativeSize.height() / 2)));
        auto *layout = host.window()->findChild<QPushButton *>("rrNavLayout");
        QVERIFY(layout);
        QTRY_VERIFY(layout->isEnabled());
        QVERIFY(click(canvas, layout));
        auto *automatic = host.window()->findChild<QPushButton *>("rrAutoRotate");
        QVERIFY(automatic);
        QTRY_COMPARE(automatic->isVisible(), fixtureEnabled);
        QVERIFY(capture(canvas, host, "native-reader-capability-layout"));
        auto *light = host.window()->findChild<QPushButton *>("rrNavLight");
        QVERIFY(light);
        QTRY_COMPARE(light->isEnabled(), fixtureEnabled);
        QTRY_COMPARE(light->isHidden(), !fixtureEnabled);
        if (fixtureEnabled) {
            QVERIFY(click(canvas, light));
            auto *panel = host.window()->findChild<QWidget *>("rrLightPanel");
            QVERIFY(panel);
            QTRY_VERIFY(panel->isVisible());
            auto *slider = panel->findChild<QSlider *>("rrBrightness");
            QVERIFY(slider);
            QCOMPARE(slider->value(), 50);
            auto *more = panel->findChild<QPushButton *>("rrBrightnessMore");
            QVERIFY(more);
            QVERIFY(click(canvas, more));
            QTRY_VERIFY(slider->value() > 50);
            QVERIFY(capture(canvas, host, "native-reader-capability-light"));
            QWidget anchor;
            auto independent = test::hostReaderServices().light(&anchor);
            QVERIFY(independent->initialize());
            QCOMPARE(independent->brightness(), 50);
            QVERIFY(independent->beginGesture());
            QCOMPARE(independent->setGestureLevel(80), 80);
            independent->finishGesture(false);
            QCOMPARE(independent->brightness(), 50);
        }
        host.shutdown();
        QTRY_VERIFY(host.idle());
    }
    void failureThenReopen() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened), failed(&host, &test::ReaderHost::failed);
        host.open(testPaths().directory + "/missing.epub");
        QCOMPARE(failed.count(), 1);
        QVERIFY(host.idle());
        QVERIFY(!host.window());
        QVERIFY(!host.session());
        host.open(fixture());
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.shutdown();
        QTRY_VERIFY(host.idle());
    }
    void cancelAndReopen() {
        test::ReaderHost host(testPaths(), testFonts, testPlaceholder);
        CloseHost cleanup{host};
        QSignalSpy opened(&host, &test::ReaderHost::opened), closed(&host, &test::ReaderHost::closed);
        const QString path = qEnvironmentVariable("KOBALT_FIXTURES") + "/text.epub";
        host.open(path);
        host.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QCOMPARE(opened.count(), 0);
        host.open(path);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 30000);
        host.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 2, 10000);
    }
};
// QtTest's default main never enters QApplication::exec(). The production
// workers stop on aboutToQuit, so run qExec inside the actual event loop.
int main(int argc, char **argv) {
    test::configureHostPlatform(argc, argv, true);
    AssetBundle assets(QStringLiteral(KOBALT_ASSET_ROOT), QStringLiteral(KOBALT_VERSION));
    FontCopies fontCopies;
    testAssets = &assets;
    testFontCopies = &fontCopies;
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    NativeReaderTest test;
    QTimer::singleShot(0, &app, [&] {
        app.exit(QTest::qExec(&test, test::shardArguments(test, argc, argv)));
    });
    return app.exec();
}
#include "reader_test.moc"
