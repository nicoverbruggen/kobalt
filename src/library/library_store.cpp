#include "library_store.h"
#include "book_metadata.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>

namespace kobalt::library {
namespace {
qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}
} // namespace

Store::Store(QString directory, std::shared_ptr<Source> source, QObject *parent)
    : QObject(parent),
      directory(directory),
      coverDirectory(directory + "/library-covers"),
      scanner(std::move(source), coverDirectory),
      saving(this)
{
    ready = data.loadDatabase(directory);
    if (!ready)
        diagnostic("library-storage-unavailable", {}, kobalt::DiagnosticSeverity::Warning);
    saving.setSingleShot(true);
    saving.setInterval(500);
    connect(&saving, &QTimer::timeout, this, &Store::queueSave);
    scanner.listed = [this](const QString &folder, const QStringList &removed) {
        if (removedFolders.contains(folder))
            return;
        for (const auto &key : removed) {
            if (!editedBooks.contains(key))
                retireCover(data.removeBook(key));
        }
        if (!removed.isEmpty())
            touch();
    };
    scanner.read = [this](const Book &found) {
        auto book = found;
        const auto known = data.books.constFind(book.key);
        if (editedBooks.contains(book.key) || removedFolders.contains(book.folder)) {
            if (!book.cover.isEmpty()) {
                obsoleteCovers.insert(book.cover);
                saving.start();
            }
            return;
        }
        if (known != data.books.constEnd()) {
            // A changed file keeps its place in the library and its progress.
            retireCover(known->cover);
            book.added = known->added;
            book.opened = known->opened;
            book.percent = known->percent;
            book.finished = known->finished;
            book.mark = known->mark;
        } else
            book.added = now();
        data.books.insert(book.key, book);
        readCount += !book.hidden;
        touch();
    };
    scanner.finished = [this] {
        if (std::exchange(rescan, false))
            scan();
        else if (changed)
            changed();
    };
}

Store::~Store()
{
    mainthread::AllowBlocking allow("library save"); // The Library is closing.
    changed = {};
    scanner.finished = {};
    scanner.finish();
    if (saving.isActive())
        queueSave();
    if (!writer.idle())
        writer.flush();
}

QString Store::coverPath(const QString &name) const
{
    return name.isEmpty() ? QString() : coverDirectory + '/' + name;
}

bool Store::scanning() const
{
    return scanner.running();
}

void Store::scan()
{
    if (!ready)
        return;
    if (scanner.running()) {
        // A folder added during a scan is listed by the next one.
        rescan = true;
        return;
    }
    editedBooks.clear();
    removedFolders.clear();
    readCount = 0;
    bool removed = false;
    QStringList outdated;
    for (auto it = data.books.begin(); it != data.books.end();) {
        if (it->folder.isEmpty() && !QFileInfo(it->key).isFile()) {
            retireCover(it->cover);
            it = data.books.erase(it);
            removed = true;
        } else {
            if (it->folder.isEmpty() && it->metadata < metadataVersion)
                outdated.append(it->key);
            ++it;
        }
    }
    if (removed)
        touch();
    if (data.folders.isEmpty() && outdated.isEmpty())
        return;
    scanner.start(data, data.folders, outdated);
    if (changed)
        changed();
}

void Store::addFolder(const Folder &folder)
{
    if (!ready)
        return;
    data.addFolder(folder);
    touch();
    scan();
}

void Store::removeFolder(const QString &key)
{
    if (!ready)
        return;
    if (scanning())
        removedFolders.insert(key);
    for (const auto &cover : data.removeFolder(key))
        retireCover(cover);
    touch();
}

bool Store::addBook(const QString &path)
{
    if (!ready)
        return false;
    const QFileInfo info(path);
    const auto key = info.absoluteFilePath();
    QFile file(key);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const auto metadata = readBookMetadata(file);
    if (metadata.result != BookMetadata::Result::Ok)
        return false;
    // A file below a library folder already belongs to it.
    auto book = data.books.value(key);
    const bool known = !book.key.isEmpty();
    book.key = key;
    book.name = info.fileName();
    book.size = info.size();
    book.modified = info.lastModified().toMSecsSinceEpoch();
    book.title = metadata.title;
    book.author = metadata.author;
    book.series = metadata.series;
    book.seriesIndex = metadata.seriesIndex;
    book.metadata = metadataVersion;
    book.hidden = false;
    if (!known)
        book.added = now();
    QDir().mkpath(coverDirectory);
    if (scanning())
        editedBooks.insert(key);
    retireCover(book.cover);
    book.cover = saveCover(coverDirectory, metadata.cover);
    data.books.insert(key, book);
    touch();
    return true;
}

void Store::removeBook(const QString &key)
{
    if (!ready)
        return;
    if (scanning())
        editedBooks.insert(key);
    retireCover(data.removeBook(key));
    touch();
}

void Store::setMark(const QString &key, Book::Mark mark)
{
    if (!data.books.contains(key))
        return;
    auto &book = data.books[key];
    book.mark = mark;
    if (mark == Book::Mark::Unread) {
        book.percent = 0;
        book.finished = false;
        book.opened = 0;
    }
    touch();
}

void Store::setOpened(const QString &key)
{
    if (!data.books.contains(key))
        return;
    auto &book = data.books[key];
    book.opened = now();
    book.mark = Book::Mark::None;
    book.finished = false;
    touch();
}

void Store::setProgress(const QString &key, int percent, bool finished)
{
    if (!data.books.contains(key))
        return;
    auto &book = data.books[key];
    book.percent = qBound(0, percent, 100);
    book.finished = finished;
    touch();
}

void Store::setSettings(const Settings &settings)
{
    data.settings = settings;
    touch();
}

void Store::save()
{
    saving.stop();
    queueSave();
    // Explicit saves return after the current index reaches storage.
    mainthread::AllowBlocking allow("library save");
    writer.flush();
}

void Store::retireCover(const QString &name)
{
    if (!name.isEmpty() && QFileInfo(name).fileName() == name)
        obsoleteCovers.insert(name);
}

void Store::queueSave()
{
    if (!ready)
        return;
    const bool queued =
            writer.tryPost([guard = QPointer<Store>(this), directory = directory, index = data,
                            covers = obsoleteCovers, coverDirectory = coverDirectory]() mutable {
                const bool ok = index.saveDatabase(directory);
                if (!ok)
                    diagnostic("library-save-failed", {}, kobalt::DiagnosticSeverity::Error);
                if (ok && !covers.isEmpty()) {
                    for (const auto &book : std::as_const(index.books))
                        covers.remove(book.cover);
                    QSet<QString> removed;
                    mainthread::blocking("library-cover-cleanup");
                    for (const auto &name : covers) {
                        const QString path = coverDirectory + '/' + name;
                        if (QFile::remove(path) || !QFile::exists(path))
                            removed.insert(name);
                    }
                    QMetaObject::invokeMethod(guard.data(), [guard, removed] {
                        if (guard)
                            guard->obsoleteCovers.subtract(removed);
                    });
                }
                return ok;
            });
    if (!queued)
        diagnostic("library-save-failed", {}, kobalt::DiagnosticSeverity::Error);
}

void Store::touch()
{
    saving.start();
    if (changed)
        changed();
}

SavedProgress savedProgress(const QJsonObject &state)
{
    return { qBound(0, state.value("percent").toInt(), 100),
             !state.value("finished_position").toString().isEmpty() };
}
} // namespace kobalt::library
