#include "library_index.h"
#include "book_metadata.h"
#include "storage/reader_storage.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

namespace kobalt::library {
namespace {
// About 2 KiB per book leaves room for several thousand books.
constexpr qint64 indexLimit = 16 * 1024 * 1024;
const char *const views[] = { "list", "grid", "home" };
const char *const sorts[] = { "opened", "added", "title", "author", "series" };
const char *const marks[] = { "", "finished", "unread" };
const char *const coverSizes[] = { "small", "medium", "large" };

template <typename T, size_t N>
T named(const QString &name, const char *const (&names)[N], T fallback)
{
    for (size_t i = 0; i < N; ++i)
        if (name == QLatin1String(names[i]))
            return T(i);
    return fallback;
}
} // namespace

QString Book::displayTitle() const
{
    if (!title.isEmpty())
        return title;
    auto base = name;
    for (const auto *suffix : { ".kepub.epub", ".epub" })
        if (base.endsWith(QLatin1String(suffix), Qt::CaseInsensitive))
            return base.left(base.size() - int(qstrlen(suffix)));
    return base;
}

Status Book::status() const
{
    if (mark == Mark::Unread)
        return Status::New;
    if (mark == Mark::Finished || (mark == Mark::None && finished))
        return Status::Finished;
    return opened > 0 ? Status::Reading : Status::New;
}

QJsonObject Book::json() const
{
    QJsonObject value{ { "key", key },
                       { "name", name },
                       { "size", double(size) },
                       { "modified", double(modified) },
                       { "added", double(added) } };
    const auto text = [&value](const char *field, const QString &content) {
        if (!content.isEmpty())
            value[field] = content;
    };
    text("folder", folder);
    text("title", title);
    text("author", author);
    text("cover", cover);
    text("series", series);
    if (seriesIndex > 0)
        value["series_index"] = seriesIndex;
    if (metadata)
        value["metadata"] = metadata;
    if (opened)
        value["opened"] = double(opened);
    if (percent)
        value["percent"] = percent;
    if (finished)
        value["finished"] = true;
    if (mark != Mark::None)
        value["mark"] = QLatin1String(marks[int(mark)]);
    if (hidden)
        value["hidden"] = true;
    return value;
}

Book Book::fromJson(const QJsonObject &value)
{
    Book book;
    book.key = value.value("key").toString();
    book.folder = value.value("folder").toString();
    book.name = value.value("name").toString();
    book.title = value.value("title").toString();
    book.author = value.value("author").toString();
    book.cover = value.value("cover").toString();
    book.series = value.value("series").toString();
    book.seriesIndex = qMax(0.0, value.value("series_index").toDouble());
    book.metadata = value.value("metadata").toInt();
    book.size = qint64(value.value("size").toDouble());
    book.modified = qint64(value.value("modified").toDouble());
    book.added = qint64(value.value("added").toDouble());
    book.opened = qint64(value.value("opened").toDouble());
    book.percent = qBound(0, value.value("percent").toInt(), 100);
    book.finished = value.value("finished").toBool();
    book.mark = named(value.value("mark").toString(), marks, Mark::None);
    book.hidden = value.value("hidden").toBool();
    // A cover is a file name in the cover folder, never a path.
    if (book.cover.contains('/') || book.cover.contains('\\') || book.cover.startsWith('.'))
        book.cover.clear();
    return book;
}

bool Index::load(const QString &path)
{
    const auto read = storage::readJsonFile(path, indexLimit);
    if (read.status == storage::ReadStatus::Missing)
        return true;
    if (read.status != storage::ReadStatus::Ok)
        return false;
    return restore(read.object);
}

bool Index::restore(const QJsonObject &root)
{
    if (root.value("schema") != QJsonValue(1) || !root.value("folders").isArray()
        || !root.value("books").isArray())
        return false;
    folders.clear();
    books.clear();
    for (const auto &item : root.value("folders").toArray()) {
        const auto object = item.toObject();
        const Folder folder{ object.value("key").toString(), object.value("label").toString() };
        if (!item.isObject() || folder.key.isEmpty() || hasFolder(folder.key))
            return false;
        folders.append(folder);
    }
    for (const auto &item : root.value("books").toArray()) {
        auto book = Book::fromJson(item.toObject());
        if (!item.isObject() || book.key.isEmpty() || books.contains(book.key)
            || (!book.folder.isEmpty() && !hasFolder(book.folder)))
            return false;
        books.insert(book.key, book);
    }
    const auto options = root.value("settings").toObject();
    settings.view = named(options.value("view").toString(), views, View::Grid);
    settings.sort = named(options.value("sort").toString(), sorts, Sort::Opened);
    settings.hideFinished = options.value("hide_finished").toBool(false);
    settings.coverSize =
            named(options.value("cover_size").toString(), coverSizes, CoverSize::Medium);
    settings.checkAtStart = options.value("check_at_start").toBool(true);
    return true;
}

bool Index::save(const QString &path) const
{
    QJsonArray folderList, bookList;
    for (const auto &folder : folders)
        folderList.append(QJsonObject{ { "key", folder.key }, { "label", folder.label } });
    for (const auto &book : books)
        bookList.append(book.json());
    const QJsonObject options{ { "view", QLatin1String(views[int(settings.view)]) },
                               { "sort", QLatin1String(sorts[int(settings.sort)]) },
                               { "hide_finished", settings.hideFinished },
                               { "cover_size", QLatin1String(coverSizes[int(settings.coverSize)]) },
                               { "check_at_start", settings.checkAtStart } };
    return storage::writeJson(path,
                              QJsonObject{ { "schema", 1 },
                                           { "folders", folderList },
                                           { "books", bookList },
                                           { "settings", options } });
}

bool Index::hasFolder(const QString &key) const
{
    for (const auto &folder : folders)
        if (folder.key == key)
            return true;
    return false;
}

bool Index::addFolder(const Folder &folder)
{
    for (auto &existing : folders)
        if (existing.key == folder.key) {
            existing.label = folder.label;
            return false;
        }
    folders.append(folder);
    return true;
}

QStringList Index::removeFolder(const QString &key)
{
    QStringList covers;
    for (int i = folders.size() - 1; i >= 0; --i)
        if (folders[i].key == key)
            folders.removeAt(i);
    for (auto it = books.begin(); it != books.end();) {
        if (it->folder == key) {
            if (!it->cover.isEmpty())
                covers.append(it->cover);
            it = books.erase(it);
        } else
            ++it;
    }
    return covers;
}

QString Index::removeBook(const QString &key)
{
    const auto cover = books.value(key).cover;
    books.remove(key);
    return cover;
}

int Index::visibleCount() const
{
    int count = 0;
    for (const auto &book : books)
        count += !book.hidden;
    return count;
}

int Index::folderBooks(const QString &folder) const
{
    int count = 0;
    for (const auto &book : books)
        count += !book.hidden && book.folder == folder;
    return count;
}

Index::Plan Index::plan(const QString &folder, const QVector<Entry> &files) const
{
    Plan result;
    QSet<QString> present;
    for (const auto &file : files) {
        present.insert(file.key);
        const auto known = books.constFind(file.key);
        if (known == books.constEnd() || known->size != file.size
            || known->modified != file.modified || known->metadata < metadataVersion)
            result.read.append(file);
    }
    for (const auto &book : books)
        if (book.folder == folder && !present.contains(book.key))
            result.removed.append(book.key);
    return result;
}
} // namespace kobalt::library
