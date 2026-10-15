#include "platform/android/reader.h"
#include "host_services.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/views/reading_page.h"
#include "reading/reading_density.h"
#include "storage/reader_storage.h"
#include "support/strings.h"

#include <QLabel>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QPushButton>

using namespace kobalt;
class AndroidReaderTest : public QObject {
    Q_OBJECT
    std::unique_ptr<AssetBundle> assets;
    FontCopies copies;
    std::unique_ptr<QTemporaryDir> temporary;
    std::unique_ptr<android::ReaderController> reader;
    EngineFontInputs fonts;
    QString placeholder;
    storage::Paths paths;
    bool suspended = false;
    QString fixture(const char *name = "text.epub") const {
        return QString::fromUtf8(qgetenv("KOBALT_FIXTURES")) + '/' + QString::fromLatin1(name);
    }
private slots:
    void initTestCase() {
        QApplication::setStyle("Fusion");
        assets = std::make_unique<AssetBundle>(QStringLiteral(KOBALT_TEST_ASSET_ROOT), QStringLiteral(KOBALT_VERSION));
        QVERIFY(assets->available());
        QVERIFY(ui::initializeInterfaceFonts(*assets));
        ui::setDensity(300);
        readingPpi.store(300);
        fonts = engineFontInputs(*assets, {}, {}, {});
        placeholder = placeholderFontFamily(*assets, copies);
    }
    void init() {
        temporary = std::make_unique<QTemporaryDir>();
        QVERIFY(temporary->isValid());
        paths = storage::Paths{temporary->path() + "/config"};
        paths.stateReport = temporary->path() + "/state.json";
        suspended = false;
        reader = std::make_unique<android::ReaderController>(
            paths, fonts, placeholder,
            [this] {
                auto services = test::hostReaderServices();
                services.suspended = [this] {
                    return suspended;
                };
                return services;
            },
            [](Qt::ScreenOrientation) {});
    }
    void cleanup() {
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        reader.reset();
        temporary.reset();
    }
    void openingAndCheckpointUseSharedStorage() {
        QSignalSpy progress(reader.get(), &android::ReaderController::openingProgress);
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        QVERIFY(!progress.isEmpty());
        for (const auto &update : progress)
            QVERIFY(update.first().toInt() >= 0 && update.first().toInt() <= 100);
        const auto last = reader->lastBook();
        QCOMPARE(last.value("path").toString(), fixture());
        QVERIFY(!last.value("title").toString().isEmpty());
        QVERIFY(reader->window());
        reader->window()->checkpoint();
        const auto identity = QUrl::fromLocalFile(fixture()).toString();
        const auto saved = storage::readJson(paths.bookState(identity));
        QCOMPARE(saved.value("book").toString(), identity);
        QVERIFY(!saved.value("position").toString().isEmpty());
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 15000);
        reader->window()->checkpoint();
        QCOMPARE(storage::readJson(paths.bookState(identity)).value("position"), saved.value("position"));
    }
    void statsUsesSharedCountAfterReopening() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        for (int opening = 1; opening <= 2; ++opening) {
            reader->open(fixture("word-counts.epub"), {1264, 1680});
            QTRY_COMPARE_WITH_TIMEOUT(opened.count(), opening, 15000);
            auto *window = reader->window();
            QVERIFY(window);
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window));
            QTRY_VERIFY(window->readyForPresentation());
            auto *page = window->findChild<ReadingPage *>("rrPage");
            QVERIFY(page);
            page->tapped(page->rect().center());
            auto *stats = window->findChild<QPushButton *>("rrNavStats");
            QVERIFY(stats);
            QTRY_VERIFY(stats->isVisible());
            QTest::mouseClick(stats, Qt::LeftButton);
            auto *detail = window->findChild<QLabel *>("rrGraphBookDetail");
            QVERIFY(detail);
            QTRY_VERIFY(detail->isVisible());
            QCOMPARE(detail->text(), strings::get("stats.words").arg("40"));
            QVERIFY(window->findChild<QWidget *>("rrChapterBars")->isVisible());
            reader->close();
            QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        }
    }
    void readingTimePausesWhileInactive_data() {
        QTest::addColumn<int>("applicationState");
        QTest::newRow("inactive") << int(Qt::ApplicationInactive);
        QTest::newRow("hidden") << int(Qt::ApplicationHidden);
        QTest::newRow("suspended") << int(Qt::ApplicationSuspended);
    }
    void readingTimePausesWhileInactive() {
        QFETCH(int, applicationState);
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture(), {674, 896});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        auto *window = reader->window();
        window->show();
        QTRY_VERIFY(window->readyForPresentation());
        const auto readingTime = [&] {
            window->checkpoint();
            return storage::readJson(paths.stateReport).value("session_reading_ms").toDouble(-1);
        };
        QTRY_VERIFY(readingTime() > 0);
        suspended = true;
        qApp->applicationStateChanged(Qt::ApplicationState(applicationState));
        const auto paused = readingTime();
        // Android can stop event delivery while the reader widget remains visible.
        QTest::qSleep(80);
        QCOMPARE(readingTime(), paused);
        // A late layout or show event must not restart the background timer.
        window->hide();
        window->show();
        QTRY_VERIFY(window->readyForPresentation());
        QTest::qWait(80);
        QCOMPARE(readingTime(), paused);
        suspended = false;
        qApp->applicationStateChanged(Qt::ApplicationActive);
        const auto resumed = readingTime();
        QTest::qSleep(80);
        QVERIFY(readingTime() > resumed);
        window->hide();
        const auto hidden = readingTime();
        qApp->applicationStateChanged(Qt::ApplicationActive);
        QTest::qSleep(80);
        QCOMPARE(readingTime(), hidden);
    }
    void openingWhileInactiveDoesNotStartReadingTime() {
        suspended = true;
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture(), {674, 896});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        auto *window = reader->window();
        window->show();
        QTRY_VERIFY(window->readyForPresentation());
        QTest::qWait(80);
        window->checkpoint();
        QCOMPARE(storage::readJson(paths.stateReport).value("session_reading_ms").toDouble(-1), 0.0);
        suspended = false;
        qApp->applicationStateChanged(Qt::ApplicationActive);
        QTest::qSleep(80);
        window->checkpoint();
        QVERIFY(storage::readJson(paths.stateReport).value("session_reading_ms").toDouble() > 0);
    }
    void logicalPageButtonsKeepRtlOrderAndInputGuards() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture("rtl.epub"), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        auto *window = reader->window();
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto statePath = paths.bookState(QUrl::fromLocalFile(fixture("rtl.epub")).toString());
        const auto state = [&] {
            window->checkpoint();
            return storage::readJson(statePath);
        };
        QTRY_VERIFY(window->readyForPresentation());
        QVERIFY(state().value("page_progression_rtl").toBool());
        QCOMPARE(state().value("page").toInt(-1), 0);
        // The Android callback supplies +1 for Volume Down and Page Down.
        window->pageButton(1);
        QTRY_COMPARE(state().value("page").toInt(-1), 1);
        window->pageButton(-1);
        QTRY_COMPARE(state().value("page").toInt(-1), 0);
        auto *page = window->findChild<ReadingPage *>("rrPage");
        QVERIFY(page);
        page->swiped(-1);
        QTRY_COMPARE(state().value("page").toInt(-1), 1);
        window->setEnabled(false);
        window->pageButton(1);
        QTest::qWait(30);
        QCOMPARE(state().value("page").toInt(-1), 1);
        window->setEnabled(true);
        window->hide();
        window->pageButton(1);
        window->show();
        QTest::qWait(30);
        QCOMPARE(state().value("page").toInt(-1), 1);
        page->tapped(QPoint(page->width() / 2, page->height() / 2));
        auto *settings = window->findChild<QPushButton *>("rrSettings");
        QVERIFY(settings);
        QTRY_VERIFY(settings->isVisible());
        settings->click();
        auto *settingsPages = window->findChild<QWidget *>("rrSettingsPages");
        QVERIFY(settingsPages);
        QTRY_VERIFY(settingsPages->isVisible());
        window->pageButton(1);
        QTest::qWait(30);
        QCOMPARE(state().value("page").toInt(-1), 1);
    }
    void oversizedMetadataKeepsLastBookReadable() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture("oversized-metadata.epub"), {674, 896});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        const auto last = reader->lastBook();
        QCOMPARE(last.value("path").toString(), fixture("oversized-metadata.epub"));
        QCOMPARE(last.value("title").toString(), QString(4095, 'T'));
        QCOMPARE(last.value("author").toString(), QString(4095, 'A'));
        QVERIFY(QFileInfo(paths.directory + "/last-book.json").size() <= 256 * 1024);
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        QCOMPARE(reader->lastBook(), last);
    }
    void newestDocumentWinsDuringOpening() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture(), {1264, 1680});
        reader->open(fixture("color.epub"), {1264, 1680});
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        QCOMPARE(reader->path(), fixture());
    }
    void newestDocumentWinsDuringRetirement_data() {
        QTest::addColumn<bool>("explicitClose");
        QTest::newRow("replace-reading") << false;
        QTest::newRow("replace-closing") << true;
    }
    void newestDocumentWinsDuringRetirement() {
        QFETCH(bool, explicitClose);
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        QSignalSpy failed(reader.get(), &android::ReaderController::failed);
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        QPointer<ReaderWindow> previous = reader->window();
        QVERIFY(previous);
        previous->show();
        QVERIFY(QTest::qWaitForWindowExposed(previous));
        QTRY_VERIFY(previous->readyForPresentation());
        const auto originalState = paths.bookState(QUrl::fromLocalFile(fixture()).toString());
        previous->pageButton(1);
        QTRY_VERIFY(([&] {
            previous->checkpoint();
            return storage::readJson(originalState).value("page").toInt(-1) == 1;
        })());
        const auto savedPosition = storage::readJson(originalState).value("position");
        QVERIFY(!savedPosition.toString().isEmpty());
        if (explicitClose)
            reader->close();
        reader->open(fixture("color.epub"), {1072, 1448});
        reader->open(fixture("dictionary.epub"), {674, 896});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 15000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(previous.isNull());
        QCOMPARE(reader->path(), fixture("dictionary.epub"));
        QVERIFY(reader->window());
        QCOMPARE(reader->window()->size(), QSize(674, 896));
        QCOMPARE(reader->lastBook().value("path").toString(), fixture("dictionary.epub"));
        QCOMPARE(storage::readJson(originalState).value("position"), savedPosition);
        QVERIFY(!QFile::exists(paths.bookState(QUrl::fromLocalFile(fixture("color.epub")).toString())));
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 3, 15000);
        reader->window()->checkpoint();
        QCOMPARE(storage::readJson(originalState).value("position"), savedPosition);
    }
    void closeCancelsPendingDocument() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture(), {1264, 1680});
        reader->open(fixture("color.epub"), {1264, 1680});
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        QCOMPARE(opened.count(), 0);
        QVERIFY(!reader->window());
        QVERIFY(reader->lastBook().isEmpty());
    }
    void closeDuringRetirementDropsPendingDocument() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        QSignalSpy closed(reader.get(), &android::ReaderController::closed);
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        reader->window()->checkpoint();
        const auto identity = QUrl::fromLocalFile(fixture()).toString();
        const auto saved = storage::readJson(paths.bookState(identity));
        QVERIFY(!saved.value("position").toString().isEmpty());
        reader->open(fixture("color.epub"), {1072, 1448});
        reader->open(fixture("dictionary.epub"), {674, 896});
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        QCOMPARE(closed.count(), 1);
        QCOMPARE(opened.count(), 1);
        QVERIFY(!reader->window());
        QVERIFY(reader->path().isEmpty());
        QCOMPARE(reader->lastBook().value("path").toString(), fixture());
        QCOMPARE(storage::readJson(paths.bookState(identity)).value("position"), saved.value("position"));
        for (const auto *name : {"color.epub", "dictionary.epub"})
            QVERIFY(!QFile::exists(paths.bookState(QUrl::fromLocalFile(fixture(name)).toString())));
    }
    void failureCanBeFollowedByAValidBook() {
        QSignalSpy failed(reader.get(), &android::ReaderController::failed);
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        const auto invalid = temporary->path() + "/invalid.epub";
        QFile file(invalid);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not an epub");
        file.close();
        reader->open(invalid, {1264, 1680});
        QTRY_COMPARE(failed.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        QVERIFY(!QFile::exists(paths.bookState(QUrl::fromLocalFile(invalid).toString())));
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
    }
    void coverMetadataSurvivesClosing_data() {
        QTest::addColumn<QString>("book");
        QTest::newRow("png") << QString("cover.epub");
        QTest::newRow("deferred-png") << QString("deferred-cover.epub");
        QTest::newRow("gif") << QString("gif.epub");
    }
    void coverMetadataSurvivesClosing() {
        QFETCH(QString, book);
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture(book.toUtf8().constData()), {674, 896});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        const auto last = reader->lastBook();
        QCOMPARE(last.value("author").toString(), QString("Kobalt"));
        const auto cover = QImage::fromData(QByteArray::fromBase64(last.value("cover").toString().toLatin1()), "PNG");
        QCOMPARE(cover.size(), QSize(120, 60));
        QCOMPARE(cover.pixelColor(30, 10), QColor(0, 192, 0));
        QCOMPARE(cover.pixelColor(90, 10), QColor(0, 0, 255));
    }
    void forgettingLastBookKeepsFileAndReadingState() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture(), {674, 896});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        reader->window()->checkpoint();
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        const auto statePath = paths.bookState(QUrl::fromLocalFile(fixture()).toString());
        const auto state = storage::readJson(statePath);
        QVERIFY(!state.isEmpty());
        QVERIFY(!reader->lastBook().isEmpty());
        QVERIFY(reader->forgetLastBook());
        QVERIFY(reader->lastBook().isEmpty());
        QVERIFY(QFileInfo(fixture()).isFile());
        QCOMPARE(storage::readJson(statePath), state);
        QVERIFY(reader->forgetLastBook());
    }
    void missingLastBookIsHidden() {
        QVERIFY(reader->lastBook().isEmpty());
        QDir().mkpath(paths.directory);
        QVERIFY(
            storage::writeJson(paths.directory + "/last-book.json", {{"path", temporary->path() + "/missing.epub"}}));
        QVERIFY(reader->lastBook().isEmpty());
    }
    void failedOpeningPreservesLastBook() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        QSignalSpy failed(reader.get(), &android::ReaderController::failed);
        reader->open(fixture(), {674, 896});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        const auto last = reader->lastBook();
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        reader->open(temporary->path() + "/missing.epub", {674, 896});
        QTRY_COMPARE(failed.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        QCOMPARE(reader->lastBook(), last);
    }
    void protectedArchiveNeverCreatesAReader() {
        QFile source(fixture());
        QVERIFY(source.open(QIODevice::ReadOnly));
        auto bytes = source.readAll();
        const auto directory = bytes.indexOf(QByteArray::fromHex("504b0102"));
        QVERIFY(directory >= 0);
        QVERIFY(directory + 8 < bytes.size());
        bytes[directory + 8] = char(uchar(bytes[directory + 8]) | 1);
        const auto protectedPath = temporary->path() + "/protected.epub";
        QFile output(protectedPath);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(bytes), qint64(bytes.size()));
        output.close();
        QSignalSpy failed(reader.get(), &android::ReaderController::failed);
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(protectedPath, {1264, 1680});
        QTRY_COMPARE(failed.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        QCOMPARE(opened.count(), 0);
        QVERIFY(!reader->window());
        QVERIFY(!QFile::exists(paths.bookState(QUrl::fromLocalFile(protectedPath).toString())));
    }
    void prototypeCheckpointIsMigratedByIdentity() {
        QSignalSpy opened(reader.get(), &android::ReaderController::opened);
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 15000);
        reader->window()->checkpoint();
        reader->close();
        QTRY_VERIFY_WITH_TIMEOUT(reader->idle(), 15000);
        const auto identity = QUrl::fromLocalFile(fixture()).toString();
        const auto saved = storage::readJson(paths.bookState(identity));
        QVERIFY(QDir().mkpath(temporary->path() + "/state"));
        QVERIFY(storage::writeJson(temporary->path() + "/state/text.json", saved));
        QVERIFY(QFile::remove(paths.bookState(identity)));
        reader->open(fixture(), {1264, 1680});
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 15000);
        reader->window()->checkpoint();
        QCOMPARE(storage::readJson(paths.bookState(identity)).value("position"), saved.value("position"));
    }
};
// Reader workers shut down on aboutToQuit. QtTest must run inside the
// application's event loop, as it does in the native reader integration suite.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AndroidReaderTest test;
    QTimer::singleShot(0, &app, [&] {
        app.exit(QTest::qExec(&test, argc, argv));
    });
    return app.exec();
}
#include "android_reader_test.moc"
