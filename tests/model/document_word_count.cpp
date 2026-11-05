#include "engine/crengine/engine_document.h"

#include "check.h"
#include "reader_inputs.h"
#include "support/qt_compat.h"

#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

namespace {
void check(const kobalt::DocumentWordCounts &counts)
{
    require(counts.total == 40, qPrintable(QString("Wrong logical total: %1").arg(counts.total)));
    require(counts.chapters.size() == 5, "Duplicate chapter destinations disappeared");
    const qint64 expected[]{ 23, 0, 6, 0, 9 };
    for (int i = 0; i < counts.chapters.size(); ++i) {
        require(counts.chapters[i].words == expected[i],
                qPrintable(QString("Wrong interval %1: %2").arg(i).arg(counts.chapters[i].words)));
    }
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(argc == 3, "Pass the logical-word and long cancellation EPUBs");
    QFile file(QString::fromLocal8Bit(argv[1]));
    require(file.open(QIODevice::ReadOnly), "Cannot open word fixture");
    const auto bytes = file.readAll();
    QTemporaryDir cache;
    require(cache.isValid(), "Cannot create the document cache");
    const auto key = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    QString error;
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    model.setDocumentCache(cache.path(), key, "word-count-test", 1000000);
    require(model.open(bytes, QSize(1072, 1328), {}, &error), qPrintable(error));
    const auto anchor = model.position();
    int calls = 0;
    require(model.wordCounts([&] { return ++calls < 2; }).total == -1,
            "Cancelled scan published a partial count");
    const auto cold = model.wordCounts();
    require(!cold.cached, "Cancelled scan left completed cache metadata");
    check(cold);
    require(model.totalWords() == cold.total && model.chapterGraph().size() == cold.chapters.size(),
            "Shared count did not populate the Stats metadata");
    for (int i = 0; i < cold.chapters.size(); ++i) {
        require(model.chapterGraph()[i].words == cold.chapters[i].words
                        && model.chapterGraph()[i].position == cold.chapters[i].position,
                "Stats graph changed a counted source interval");
    }
    check(model.wordCounts());
    require(model.wordCounts().cached && model.position() == anchor,
            "Cached count scanned or moved the reader");
    require(model.applyAppearance(
                    kobalt::qt::object({ { "font_pixels", 60 }, { "hyphenation", "on" } }),
                    QSize(600, 800)),
            "Cannot reflow the fixture");
    check(model.wordCounts());
    require(model.wordCounts().cached, "Reflow discarded logical counts");
    require(model.saveDocumentCache(), "Cannot persist the counted document");
    kobalt::EngineDocument reopened(kobalt::test::readerFonts());
    reopened.setDocumentCache(cache.path(), key, "word-count-test", 1000000);
    require(reopened.open(bytes, QSize(600, 800), model.state(), &error), qPrintable(error));
    require(reopened.openedFromCache(), "Document did not reopen from cache");
    calls = 0;
    require(reopened.wordCounts([&] { return ++calls < 2; }).total == -1 && calls == 2,
            "Cached count ignored cancellation after initial permission");
    require(reopened.totalWords() == -1 && reopened.chapterGraph().isEmpty(),
            "Cancelled cached count published Stats metadata");
    const auto restored = reopened.wordCounts();
    require(restored.cached, "Cached reopening lost completed word counts");
    check(restored);
    require(reopened.totalWords() == 40 && reopened.chapterGraph().last().words == 9,
            "Cached reopening did not restore Stats metadata");
    require(reopened.open(bytes, QSize(600, 800), {}, &error), qPrintable(error));
    require(reopened.totalWords() == -1 && reopened.chapterGraph().isEmpty(),
            "Opening a new document retained the previous count before counting");
    QFile longFile(QString::fromLocal8Bit(argv[2]));
    require(longFile.open(QIODevice::ReadOnly), "Cannot open cancellation fixture");
    const auto longBytes = longFile.readAll();
    kobalt::EngineDocument interrupted(kobalt::test::readerFonts());
    require(interrupted.open(longBytes, QSize(600, 800), {}, &error), qPrintable(error));
    calls = 0;
    require(interrupted.wordCounts([&] { return ++calls < 2; }).total == -1 && calls == 2,
            "Long scan ignored cancellation at its traversal checkpoint");
    require(interrupted.totalWords() == -1 && interrupted.chapterGraph().isEmpty(),
            "Interrupted traversal published partial Stats metadata");
    const auto retried = interrupted.wordCounts();
    require(!retried.cached && retried.total == 10533,
            "Interrupted traversal retained partial completed metadata or changed the retry");
    puts("PASS: source intervals, duplicate and inline anchors, hidden text, Unicode, cancellation, reflow and cache");
}
