#pragma once
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace kobalt::library {
// A folder the user added. Its key is what the host source understands: a
// local path on the desktop, a granted document tree on Android.
struct Folder
{
    QString key, label;
};

// A file as a source lists it, before its contents are read.
struct Entry
{
    QString key, folder, name;
    qint64 size = 0, modified = 0;
};
enum class Status { New, Reading, Finished };
// Home shows only the books in progress; List and Grid show every book.
enum class View { List, Grid, Home };
enum class Sort { Opened, Added, Title, Author, Series };
// The cover grid's target cover width.
enum class CoverSize { Small, Medium, Large };

struct Book
{
    // `key` identifies the file for its source. `folder` is empty for a book
    // opened on its own, which stays in the library until it is removed.
    QString key, folder, name, title, author, cover, series;
    double seriesIndex = 0;
    qint64 size = 0, modified = 0, added = 0, opened = 0;
    // The metadataVersion that read this book.
    int metadata = 0;
    int percent = 0;
    // The reader saved a finished position. The library's own choice wins.
    bool finished = false;
    // Unread lasts until the book is opened again.
    enum class Mark { None, Finished, Unread };
    Mark mark = Mark::None;
    // Protected and unreadable files stay in the index so a quick check does
    // not read them again, but the library does not show them.
    bool hidden = false;
    QString displayTitle() const;
    Status status() const;
    QJsonObject json() const;
    static Book fromJson(const QJsonObject &value);
};

struct Settings
{
    View view = View::Grid;
    Sort sort = Sort::Opened;
    CoverSize coverSize = CoverSize::Medium;
    bool hideFinished = false;
    bool checkAtStart = true;
};

class Index
{
public:
    QVector<Folder> folders;
    QMap<QString, Book> books;
    Settings settings;

    bool load(const QString &path);
    bool restore(const QJsonObject &legacy);
    bool save(const QString &path) const;
    bool loadDatabase(const QString &directory);
    bool saveDatabase(const QString &directory) const;
    bool hasFolder(const QString &key) const;
    // Adds or relabels a folder. Returns false when it was already present.
    bool addFolder(const Folder &folder);
    // Removes the folder and its books. Returns their cover file names.
    QStringList removeFolder(const QString &key);
    // Removes one book. Returns its cover file name, if any.
    QString removeBook(const QString &key);
    int visibleCount() const;
    int folderBooks(const QString &folder) const;

    // Compares a folder's current listing with the index. Unchanged books keep
    // their metadata, new or changed files and books read by an older
    // metadata version must be read, and books whose file is gone leave the
    // library.
    struct Plan
    {
        QVector<Entry> read;
        QStringList removed;
    };

    Plan plan(const QString &folder, const QVector<Entry> &files) const;
};
} // namespace kobalt::library
