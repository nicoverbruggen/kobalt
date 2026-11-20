#include "platforms/android/library_open.h"
#include "library/arrangement.h"
#include "library/book_metadata.h"
#include "library/library_index.h"
#include "library/library_store.h"
#include "library/scanner.h"
#include "storage/record_writer.h"
#include "storage/profile_database.h"
#include <QSqlQuery>
#include <QSemaphore>
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

    // The Android Library applies a passage only after its own provider copy.
    {
        kobalt::android::LibraryOpen pending;
        const auto request = pending.begin("book-a", "/body/p[2]");
        const auto match = pending.complete(request);
        require(match.key == "book-a" && match.position == "/body/p[2]",
                "Matching provider result lost its passage");
        require(pending.complete(request).key.isEmpty(), "Provider result was delivered twice");

        const auto older = pending.begin("book-a", "/body/p[4]");
        const auto current = pending.begin("book-a", "/body/p[6]");
        require(pending.complete(older).key.isEmpty(),
                "A late copy for the same book superseded a newer passage");
        require(pending.complete(current).position == "/body/p[6]",
                "An obsolete copy consumed the newer passage");

        const auto cancelled = pending.begin("book-a", "/body/p[8]");
        pending.cancel();
        require(pending.complete(cancelled).key.isEmpty(),
                "An external open left the cancelled provider request active");
        const auto ordinary = pending.begin("book-b", {});
        const auto book = pending.complete(ordinary);
        require(book.key == "book-b" && book.position.isEmpty(),
                "An ordinary book open inherited a cancelled passage");

        const auto failed = pending.begin("book-a", "/body/p[10]");
        pending.complete(failed);
        require(pending.complete(failed).key.isEmpty(),
                "A failed provider copy left a pending passage");
    }

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
    // An in-flight scan must not put back a folder the user removed.
    {
        const auto profile = root.filePath("removed-during-scan");
        const auto path = root.filePath("late-folder");
        write(path + "/book.epub", epub("Late entry", "", true));
        Store store(profile, std::make_shared<LocalSource>());
        store.addFolder({ path, "Late" });
        store.removeFolder(path);
        QElapsedTimer elapsed;
        elapsed.start();
        while (store.scanning() && elapsed.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
        store.save();
        require(!store.scanning() && store.index().books.isEmpty()
                        && QDir(profile + "/library-covers").entryList(QDir::Files).isEmpty(),
                "A late scan restored a removed folder's book or cover");
    }
    // Imported books can share a cover name. Keep it until the last reference goes.
    {
        const auto profile = root.filePath("shared-legacy-cover");
        Index seed;
        require(seed.loadDatabase(profile), "Could not open the shared cover catalogue");
        for (const auto &key : { "one", "two" }) {
            auto entry = book(key, key, "", 1);
            entry.cover = "shared.png";
            seed.books.insert(key, entry);
        }
        require(seed.saveDatabase(profile), "Could not seed books with the same cover");
        Store store(profile, std::make_shared<LocalSource>());
        const auto path = store.coverPath("shared.png");
        write(path, "shared");
        store.removeBook("one");
        store.save();
        require(QFile::exists(path), "Removing one book deleted another book's cover");
        store.removeBook("two");
        store.save();
        require(!QFile::exists(path), "The final book left its shared cover behind");
    }
    for (const bool escaped : { false, true }) {
        const auto directory = root.filePath(escaped ? "escaped-book-keys" : "literal-book-keys");
        const QString plain = "book.epub";
        const QString marked = QChar(0xfeff) + plain;
        const QByteArray marker = escaped ? QByteArray("\\ufeff") : QByteArray::fromHex("efbbbf");
        write(directory + "/library.json",
              R"({"schema":1,"folders":[],"books":[{"key":"book.epub"},{"key":")" + marker
                      + R"(book.epub"}]})");
        Index imported;
        require(imported.loadDatabase(directory), "Could not import the legacy Library");
        require(imported.books.contains(marked) && imported.books.contains(plain),
                "Import changed the keys");
        imported.removeBook(marked);
        require(imported.saveDatabase(directory), "Could not save the book removal");
        Index reopened;
        require(reopened.loadDatabase(directory) && reopened.books.size() == 1
                        && reopened.books.contains(plain),
                "A removed book with a leading FEFF returned");
    }
    {
        const auto folder = root.filePath("folder-writes");
        Index index;
        require(index.loadDatabase(folder), "Could not open Library folder fixture");
        index.addFolder({ "/keep", "Keep" });
        index.addFolder({ "/remove", "Remove" });
        index.addFolder({ "/rename", "Old label" });
        const auto entry = book("/keep/a.epub", "A", "Author", 1);
        index.books.insert(entry.key, entry);
        require(index.saveDatabase(folder), "Could not seed Library folder fixture");
        require(kobalt::storage::profileTransaction(
                        folder,
                        [](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            for (const auto &operation :
                                 QStringList{ "DELETE", "INSERT", "UPDATE" }) {
                                const auto row = operation == "INSERT" ? "NEW" : "OLD";
                                if (!query.exec(
                                            "CREATE TEMP TRIGGER guard_" + operation + " BEFORE "
                                            + operation + " ON library_folders WHEN " + row
                                            + ".key='/keep' BEGIN "
                                              "SELECT RAISE(ABORT,'unchanged folder written'); END"))
                                    return false;
                            }
                            return true;
                        }),
                "Could not guard the unchanged folder");
        index.books[entry.key].percent = 35;
        require(index.saveDatabase(folder), "A progress-only save wrote an unchanged folder");
        index.removeFolder("/remove");
        index.addFolder({ "/rename", "New label" });
        index.addFolder({ "/new", "" });
        require(index.saveDatabase(folder), "A folder edit rewrote an unaffected folder");
        Index saved;
        require(saved.loadDatabase(folder) && saved.folders.size() == 3
                        && saved.folders[0].key == "/keep" && saved.folders[1].key == "/rename"
                        && saved.folders[1].label == "New label" && saved.folders[2].key == "/new"
                        && saved.folders[2].label.isEmpty() && saved.books[entry.key].percent == 35,
                "Library save lost folder order, labels or book progress");
    }
    // A slow profile writer must not hold up input when the Library save timer fires.
    {
        const auto folder = root.filePath("background-save");
        Store store(folder, std::make_shared<LocalSource>());
        QSemaphore started, release;
        kobalt::storage::RecordWriter blocker;
        const bool queued = blocker.tryPost([&] {
            started.release();
            require(release.tryAcquire(1, 4000),
                    "Library save timer blocked the event loop on the profile writer");
            return true;
        });
        require(queued && started.tryAcquire(1, 4000), "Could not hold the profile writer busy");
        // Queue the next event from the actual save timer, after its timeout handler.
        auto *saveTimer = store.findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        require(saveTimer, "Missing Library save timer");
        QEventLoop nextEvent;
        QObject::connect(saveTimer, &QTimer::timeout, &nextEvent, &QEventLoop::quit,
                         Qt::QueuedConnection);
        Settings settings;
        settings.view = View::List;
        store.setSettings(settings);
        nextEvent.exec();
        release.release();
        require(blocker.writeNow([] { return true; }), "Could not drain the queued Library save");
        Index saved;
        require(saved.loadDatabase(folder) && saved.settings.view == View::List,
                "The timer did not persist the Library settings while the store was open");
        settings.view = View::Home;
        store.setSettings(settings);
    }
    {
        Store reopened(root.filePath("background-save"), std::make_shared<LocalSource>());
        require(reopened.index().settings.view == View::Home,
                "Closing lost the latest Library settings");
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
            write(bookPath, epub("Emma", "Jane Austen", true, { 0, 0 }));
            require(store.addBook(bookPath) && store.index().books.value(key).cover.isEmpty(),
                    "A revised book with no readable cover stayed linked to the old image");
            store.save();
            require(!QFile::exists(store.coverPath(added.cover)),
                    "Replacing a single book left its obsolete cover on disk");
            write(bookPath, epub("Emma", "Jane Austen", true));
            require(store.addBook(bookPath), "The replacement cover could not be read");
            const auto previous = store.index().books.value(key).cover;
            require(store.addBook(bookPath), "The second replacement cover could not be read");
            store.save();
            require(QFile::exists(store.coverPath(store.index().books.value(key).cover)),
                    "Cleaning up the previous cover removed its replacement");
            require(!QFile::exists(store.coverPath(previous)),
                    "The replaced cover was not removed");
        }
        Index stale;
        require(stale.loadDatabase(folder), "Could not open the single book catalogue");
        auto &oldBook = stale.books[QFileInfo(bookPath).absoluteFilePath()];
        oldBook.metadata = metadataVersion - 1;
        require(stale.saveDatabase(folder), "Could not prepare outdated metadata");
        write(bookPath, epub("Emma revised", "Jane Austen", true, { 0, 0 }));
        {
            Store refresh(folder, std::make_shared<LocalSource>());
            refresh.scan();
            require(refresh.scanning() && refresh.index().books.value(oldBook.key).title == "Emma",
                    "Refreshing an outdated single book blocked the caller");
            QElapsedTimer elapsed;
            elapsed.start();
            while (refresh.scanning() && elapsed.elapsed() < 10000)
                QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
            const auto revised = refresh.index().books.value(oldBook.key);
            require(!refresh.scanning() && revised.title == "Emma revised"
                            && revised.cover.isEmpty() && revised.opened == oldBook.opened
                            && revised.metadata == metadataVersion,
                    "The background scan failed to refresh the single book or lost its progress");
            refresh.save();
            require(!QFile::exists(refresh.coverPath(oldBook.cover)),
                    "The background scan kept a cover no longer in the catalogue");
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
