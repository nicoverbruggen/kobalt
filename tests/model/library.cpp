#include "library/arrangement.h"
#include "library/book_metadata.h"
#include "library/library_index.h"
#include "library/library_store.h"
#include "library/scanner.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtEndian>
#include <zlib.h>
#include "check.h"

using namespace kobalt::library;

namespace {
void u16(QByteArray &out, quint16 value)
{
    const auto little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), 2);
}

void u32(QByteArray &out, quint32 value)
{
    const auto little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), 4);
}

// A stored ZIP archive, enough for the publication reader.
QByteArray zip(const QVector<QPair<QString, QByteArray>> &members)
{
    QByteArray archive, directory;
    for (const auto &member : members) {
        const auto name = member.first.toUtf8();
        const auto &data = member.second;
        const quint32 offset = archive.size(),
                      crc = crc32(0, reinterpret_cast<const Bytef *>(data.constData()),
                                  data.size());
        u32(archive, 0x04034b50);
        u16(archive, 20);
        u16(archive, 0x800);
        u16(archive, 0);
        u32(archive, 0);
        u32(archive, crc);
        u32(archive, data.size());
        u32(archive, data.size());
        u16(archive, name.size());
        u16(archive, 0);
        archive += name + data;
        u32(directory, 0x02014b50);
        u16(directory, 20);
        u16(directory, 20);
        u16(directory, 0x800);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, crc);
        u32(directory, data.size());
        u32(directory, data.size());
        u16(directory, name.size());
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, offset);
        directory += name;
    }
    const quint32 offset = archive.size();
    archive += directory;
    u32(archive, 0x06054b50);
    u16(archive, 0);
    u16(archive, 0);
    u16(archive, members.size());
    u16(archive, members.size());
    u32(archive, directory.size());
    u32(archive, offset);
    u16(archive, 0);
    return archive;
}

QByteArray png(QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::darkBlue);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

const QByteArray container =
        "<?xml version=\"1.0\"?><container version=\"1.0\" "
        "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
        "<rootfile full-path=\"OEBPS/content.opf\" "
        "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

QByteArray epub(const QString &title, const QString &author, bool epub3Cover,
                QSize coverPixels = { 600, 900 }, QVector<QPair<QString, QByteArray>> extra = {},
                const QString &extraMetadata = {})
{
    const auto manifestCover = epub3Cover
            ? QString("<item id=\"c\" href=\"images/cover%20art.png\" "
                      "media-type=\"image/png\" properties=\"cover-image\"/>")
            : QString("<item id=\"cover-id\" href=\"images/cover%20art.png\" "
                      "media-type=\"image/png\"/>");
    const auto meta =
            epub3Cover ? QString() : QString("<meta name=\"cover\" content=\"cover-id\"/>");
    const auto package =
            QString("<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
                    "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>%1</dc:title>"
                    "<dc:creator>%2</dc:creator>%3</metadata><manifest>%4"
                    "<item id=\"t\" href=\"text.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
                    "<spine><itemref idref=\"t\"/></spine></package>")
                    .arg(title.toHtmlEscaped(), author.toHtmlEscaped(), meta + extraMetadata,
                         manifestCover)
                    .toUtf8();
    QVector<QPair<QString, QByteArray>> members{ { "mimetype", "application/epub+zip" },
                                                 { "META-INF/container.xml", container },
                                                 { "OEBPS/content.opf", package },
                                                 { "OEBPS/text.xhtml", "<html/>" },
                                                 { "OEBPS/images/cover art.png",
                                                   png(coverPixels) } };
    members += extra;
    return zip(members);
}

BookMetadata metadataOf(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    return readBookMetadata(buffer);
}

void write(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "Cannot write fixture");
}

Book book(const QString &key, const QString &title, const QString &author, qint64 added,
          qint64 opened = 0, bool finished = false)
{
    Book result;
    result.key = key;
    result.name = key + ".epub";
    result.title = title;
    result.author = author;
    result.added = added;
    result.opened = opened;
    result.finished = finished;
    return result;
}

QStringList titles(const QVector<const Book *> &books)
{
    QStringList result;
    for (const auto *item : books)
        result.append(item->displayTitle());
    return result;
}
} // namespace

// Checks the Library catalogue: reading books and covers, series, the saved index, folder scans and finished status.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // Metadata from EPUB 3 and EPUB 2 packages, with a percent-encoded cover path.
    {
        const auto three = metadataOf(epub("Persuasion", "Jane Austen", true));
        require(three.result == BookMetadata::Result::Ok, "An EPUB 3 book was not read");
        require(three.title == "Persuasion" && three.author == "Jane Austen",
                "EPUB 3 title or author is wrong");
        require(three.cover.size() == QSize(240, 360),
                "The cover was not scaled to the stored size");
        const auto two = metadataOf(epub("  Middle\n march ", "George Eliot", false, { 100, 150 }));
        require(two.title == "Middle march", "Title white space was not simplified");
        require(two.cover.size() == QSize(100, 150), "A small cover was enlarged");
        const auto noCover = metadataOf(epub("A", "B", true, { 0, 0 }));
        require(noCover.result == BookMetadata::Result::Ok && noCover.cover.isNull(),
                "A book with an unreadable cover was not kept without one");
        const auto lcp =
                metadataOf(epub("A", "B", true, { 10, 10 }, { { "META-INF/license.lcpl", "{}" } }));
        require(lcp.result == BookMetadata::Result::Protected, "A protected book was not reported");
        require(metadataOf("not a zip").result == BookMetadata::Result::Unreadable,
                "A broken file was not reported as unreadable");
        const auto epub3Series =
                metadataOf(epub("Guards! Guards!", "Terry Pratchett", true, { 10, 10 }, {},
                                "<meta property=\"belongs-to-collection\" id=\"s\">Discworld</meta>"
                                "<meta refines=\"#s\" property=\"collection-type\">series</meta>"
                                "<meta refines=\"#s\" property=\"group-position\">8</meta>"));
        require(epub3Series.series == "Discworld" && epub3Series.seriesIndex == 8,
                "An EPUB 3 series was not read");
        const auto set =
                metadataOf(epub("A", "B", true, { 10, 10 }, {},
                                "<meta property=\"belongs-to-collection\" id=\"s\">Classics</meta>"
                                "<meta refines=\"#s\" property=\"collection-type\">set</meta>"));
        require(set.series.isEmpty(), "A collection marked as a set was read as a series");
        const auto calibre =
                metadataOf(epub("A", "B", false, { 10, 10 }, {},
                                "<meta name=\"calibre:series\" content=\"The Expanse\"/>"
                                "<meta name=\"calibre:series_index\" content=\"3.0\"/>"));
        require(calibre.series == "The Expanse" && calibre.seriesIndex == 3,
                "A Calibre series was not read");
        const auto entity = epub("Tom & Jerry", "B", true);
        require(metadataOf(entity).title == "Tom & Jerry", "An escaped title was not decoded");
    }

    // The index saves and loads, plans scans and removes folders.
    QTemporaryDir root;
    require(root.isValid(), "No temporary folder");
    {
        Index index;
        index.addFolder({ "/books", "Books" });
        require(!index.addFolder({ "/books", "Renamed" })
                        && index.folders.first().label == "Renamed",
                "Adding a known folder did not relabel it");
        auto first = book("/books/a.epub", "A", "Ann Author", 10, 20, true);
        first.folder = "/books";
        first.size = 5;
        first.modified = 6;
        first.percent = 40;
        first.mark = Book::Mark::Unread;
        first.cover = "abc.png";
        first.metadata = metadataVersion;
        first.series = "Novels";
        first.seriesIndex = 2.5;
        index.books.insert(first.key, first);
        auto loose = book("/elsewhere/b.epub", "B", "", 11);
        index.books.insert(loose.key, loose);
        index.settings.view = View::List;
        index.settings.sort = Sort::Author;
        index.settings.hideFinished = true;
        index.settings.coverSize = CoverSize::Small;
        index.settings.checkAtStart = false;
        const auto path = root.filePath("library.json");
        require(index.save(path), "The index was not saved");
        Index loaded;
        require(loaded.load(path), "The index was not loaded");
        const auto again = loaded.books.value(first.key);
        require(again.title == "A" && again.opened == 20 && again.percent == 40 && again.finished
                        && again.mark == Book::Mark::Unread && again.cover == "abc.png"
                        && again.folder == "/books" && again.series == "Novels"
                        && again.seriesIndex == 2.5,
                "A book did not survive saving");
        require(again.status() == Status::New,
                "Unread did not win over the reader's finished position");
        require(loaded.books.contains(loose.key), "A book opened on its own was lost");
        require(Settings{}.view == View::Grid, "The library does not start as a cover grid");
        require(loaded.settings.view == View::List && loaded.settings.sort == Sort::Author
                        && loaded.settings.hideFinished && !loaded.settings.checkAtStart
                        && loaded.settings.coverSize == CoverSize::Small
                        && Settings{}.coverSize == CoverSize::Medium,
                "Settings did not survive saving");
        Index missing;
        require(missing.load(root.filePath("none.json")) && missing.books.isEmpty(),
                "A missing index was not an empty library");
        write(root.filePath("broken.json"), "{");
        require(!missing.load(root.filePath("broken.json")), "A damaged index was accepted");

        const QVector<Entry> listing{ { "/books/a.epub", "/books", "a.epub", 5, 6 },
                                      { "/books/new.epub", "/books", "new.epub", 1, 1 } };
        auto plan = loaded.plan("/books", listing);
        require(plan.read.size() == 1 && plan.read.first().key == "/books/new.epub"
                        && plan.removed.isEmpty(),
                "An unchanged book was read again, or a new one was missed");
        auto outdated = loaded;
        outdated.books["/books/a.epub"].metadata = metadataVersion - 1;
        require(outdated.plan("/books", { { "/books/a.epub", "/books", "a.epub", 5, 6 } })
                                .read.size()
                        == 1,
                "A book read by an older metadata version was not read again");
        plan = loaded.plan("/books", { { "/books/a.epub", "/books", "a.epub", 5, 7 } });
        require(plan.read.size() == 1, "A changed book was not read again");
        plan = loaded.plan("/books", {});
        require(plan.removed == QStringList{ "/books/a.epub" },
                "A deleted file stayed in the library");
        require(!plan.removed.contains(loose.key), "A book outside the folder was removed");

        require(loaded.removeFolder("/books") == QStringList{ "abc.png" },
                "Removing a folder lost its covers");
        require(loaded.folders.isEmpty() && loaded.books.size() == 1,
                "Removing a folder kept its books");
        auto hiddenBook = book("/x.epub", "X", "", 1);
        hiddenBook.hidden = true;
        loaded.books.insert(hiddenBook.key, hiddenBook);
        require(loaded.visibleCount() == 1, "A hidden book was counted");
    }

    // Sorting, searching and shelves.
    {
        Index index;
        for (const auto &item :
             { book("1", "Émile", "Jean-Jacques Rousseau", 50),
               book("2", "Persuasion", "Jane Austen", 40, 300),
               book("3", "Emma", "Jane Austen", 30, 100, true),
               book("4", "Dracula", "Bram Stoker", 60, 200), book("5", "Book 10", "", 20),
               book("6", "Book 9", "", 10), book("7", "1984", "George Orwell", 5) })
            index.books.insert(item.key, item);
        auto hidden = book("8", "Hidden", "", 99);
        hidden.hidden = true;
        index.books.insert(hidden.key, hidden);

        auto opened = arrange(index, { Sort::Opened, false, {} });
        require(titles(opened)
                        == QStringList({ "Persuasion", "Dracula", "Emma", "Émile", "Book 10",
                                         "Book 9", "1984" }),
                "Recently opened did not put opened books first, then the newest additions");
        require(titles(arrange(index, { Sort::Title, false, {} }))
                        == QStringList({ "1984", "Book 9", "Book 10", "Dracula", "Émile", "Emma",
                                         "Persuasion" }),
                "Titles did not sort with numbers and accents");
        require(titles(arrange(index, { Sort::Author, false, {} }))
                        == QStringList({ "Emma", "Persuasion", "1984", "Émile", "Dracula", "Book 9",
                                         "Book 10" }),
                "Authors did not sort by surname, with unknown authors last");
        require(titles(arrange(index, { Sort::Opened, false, "emile" })) == QStringList{ "Émile" },
                "Search did not ignore accents");
        require(titles(arrange(index, { Sort::Opened, false, "AUSTEN" })).size() == 2,
                "Search did not match authors");
        require(!titles(arrange(index, { Sort::Opened, true, {} })).contains("Emma"),
                "Hide finished kept a finished book");

        auto series = index;
        series.books["2"].series = "Austen";
        series.books["2"].seriesIndex = 2;
        series.books["3"].series = "Austen";
        series.books["3"].seriesIndex = 10;
        series.books["4"].series = "Gothic";
        require(titles(arrange(series, { Sort::Series, false, {} })).mid(0, 3)
                        == QStringList({ "Persuasion", "Emma", "Dracula" }),
                "Series did not sort by name, then by position");
        require(titles(arrange(series, { Sort::Opened, false, "gothic" }))
                        == QStringList{ "Dracula" },
                "Search did not match series");

        auto pair = book("9", "Good Omens", "Neil Gaiman, Terry Pratchett", 1);
        require(firstAuthor(pair) == "Neil Gaiman", "Several authors did not file under the first");
        auto inverted = book("10", "X", "Austen, Jane", 1);
        require(firstAuthor(inverted) == "Austen, Jane", "An inverted name was split");
    }

    // The local source and the scanner read a folder and its changes.
    {
        const auto folder = root.filePath("shelf");
        write(folder + "/one.epub", epub("One", "First Author", true));
        write(folder + "/sub/two.EPUB", epub("Two", "Second Author", false));
        write(folder + "/.hidden/three.epub", epub("Three", "", true));
        write(folder + "/notes.txt", "x");
        write(folder + "/broken.epub", "x");
        LocalSource local;
        QVector<Entry> files;
        std::atomic<bool> cancelled{ false };
        require(local.list({ folder, "Shelf" }, files, cancelled) && files.size() == 3,
                "The folder listing included hidden folders or other files");
        require(!local.list({ root.filePath("absent"), "" }, files, cancelled),
                "A missing folder was listed");

        Index index;
        index.addFolder({ folder, "Shelf" });
        Scanner scanner(std::make_shared<LocalSource>(), root.filePath("covers"));
        QStringList removed;
        int read = 0;
        bool done = false;
        scanner.listed = [&](const QString &, const QStringList &gone) { removed += gone; };
        scanner.read = [&](const Book &found) {
            ++read;
            index.books.insert(found.key, found);
        };
        scanner.finished = [&] { done = true; };
        const auto run = [&] {
            done = false;
            scanner.start(index, index.folders);
            QElapsedTimer clock;
            clock.start();
            while (!done && clock.elapsed() < 10000)
                QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
            require(done, "The scan did not finish");
        };
        run();
        require(read == 3 && index.visibleCount() == 2,
                "The scan did not read every book, or showed a broken one");
        const auto one = index.books.value(QFileInfo(folder + "/one.epub").absoluteFilePath());
        require(one.title == "One" && !one.cover.isEmpty()
                        && QFile::exists(root.filePath("covers/" + one.cover)),
                "The scan did not keep the title and cover");
        read = 0;
        run();
        require(read == 0 && removed.isEmpty(), "A second scan read unchanged books again");
        QFile::remove(folder + "/sub/two.EPUB");
        run();
        require(removed.size() == 1 && removed.first().endsWith("two.EPUB"),
                "A deleted book was not reported");
    }
    // The store adds single books, keeps the reader's marks and survives a damaged index.
    {
        const auto folder = root.filePath("store");
        QDir().mkpath(folder);
        const auto damaged = root.filePath("damaged-library");
        QDir().mkpath(damaged);
        write(damaged + "/library.json", "{ damaged");
        {
            Store blocked(damaged, std::make_shared<LocalSource>());
            require(blocked.index().books.isEmpty() && QFile::exists(damaged + "/library.json"),
                    "A damaged catalogue was removed");
            blocked.save();
            require(QFile::exists(damaged + "/library.json"),
                    "A failed import replaced its source");
        }
        const auto bookPath = root.filePath("single/emma.epub");
        write(bookPath, epub("Emma", "Jane Austen", true));
        {
            Store store(folder, std::make_shared<LocalSource>());
            require(store.index().books.isEmpty(), "A new catalogue was not empty");
            require(store.addBook(bookPath), "A single book was not added");
            require(!store.addBook(root.filePath("missing.epub")), "A missing file was added");
            const auto key = QFileInfo(bookPath).absoluteFilePath();
            const auto added = store.index().books.value(key);
            require(added.title == "Emma" && added.folder.isEmpty()
                            && QFile::exists(store.coverPath(added.cover)),
                    "A single book lost its metadata or cover");
            store.setOpened(key);
            store.setProgress(key, 140, true);
            require(store.index().books.value(key).percent == 100
                            && store.index().books.value(key).status() == Status::Finished,
                    "Progress was not bounded, or a finished book was not finished");
            store.setOpened(key);
            require(store.index().books.value(key).status() == Status::Reading,
                    "Opening a finished book did not clear its finished status");
            store.setMark(key, Book::Mark::Finished);
            store.setOpened(key);
            require(store.index().books.value(key).status() == Status::Reading,
                    "Opening an explicitly finished book did not clear its mark");
            store.setMark(key, Book::Mark::Unread);
            require(store.index().books.value(key).status() == Status::New,
                    "Mark as unread did not apply");
            const auto unread = store.index().books.value(key);
            require(unread.percent == 0 && !unread.finished && unread.opened == 0,
                    "Mark as unread retained progress or its last-opened date");
            store.setOpened(key);
            require(store.index().books.value(key).mark == Book::Mark::None,
                    "Opening an unread book did not clear the mark");
            store.save();
        }
        Store reopened(folder, std::make_shared<LocalSource>());
        require(reopened.index().books.size() == 1, "The store did not save its books");
        QFile::remove(bookPath);
        reopened.scan();
        require(reopened.index().books.isEmpty(),
                "A single book whose file is gone stayed in the library");
        const auto progress = savedProgress(
                QJsonObject{ { "percent", 42 }, { "finished_position", "/body/p[3]" } });
        require(progress.percent == 42 && progress.finished, "Saved reader progress was not read");
    }
    std::puts("library checks passed");
    return 0;
}
