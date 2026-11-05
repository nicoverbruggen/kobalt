#include "library_store.h"
#include "book_metadata.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

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
      scanner(std::move(source), coverDirectory)
{
    ready = data.loadDatabase(directory);
    if (!ready)
        diagnostic("library-storage-unavailable", {}, kobalt::DiagnosticSeverity::Warning);
    saving.setSingleShot(true);
    saving.setInterval(500);
    connect(&saving, &QTimer::timeout, this, &Store::save);
    scanner.listed = [this](const QString &, const QStringList &removed) {
        for (const auto &key : removed)
            QFile::remove(coverPath(data.removeBook(key)));
        if (!removed.isEmpty())
            touch();
    };
    scanner.read = [this](const Book &found) {
        auto book = found;
        const auto known = data.books.constFind(book.key);
        if (known != data.books.constEnd()) {
            // A changed file keeps its place in the library and its progress.
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
    scanner.cancel();
    if (saving.isActive())
        save();
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
    readCount = 0;
    bool removed = false;
    QStringList outdated;
    for (auto it = data.books.begin(); it != data.books.end();) {
        if (it->folder.isEmpty() && !QFileInfo(it->key).isFile()) {
            QFile::remove(coverPath(it->cover));
            it = data.books.erase(it);
            removed = true;
        } else {
            if (it->folder.isEmpty() && it->metadata < metadataVersion)
                outdated.append(it->key);
            ++it;
        }
    }
    // Single books are local files; read them again here, not on the scan thread.
    for (const auto &key : outdated)
        addBook(key);
    if (removed)
        touch();
    if (data.folders.isEmpty())
        return;
    scanner.start(data, data.folders);
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
    for (const auto &cover : data.removeFolder(key))
        QFile::remove(coverPath(cover));
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
    const auto name = coverName(key);
    book.cover = !metadata.cover.isNull() && metadata.cover.save(coverPath(name), "PNG")
            ? name
            : QString();
    data.books.insert(key, book);
    touch();
    return true;
}

void Store::removeBook(const QString &key)
{
    if (!ready)
        return;
    QFile::remove(coverPath(data.removeBook(key)));
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
    // Known exception: the Android and desktop library still saves on the
    // main thread, also while a book is open.
    mainthread::AllowBlocking allow("library save");
    saving.stop();
    QDir().mkpath(directory);
    if (ready && !data.saveDatabase(directory))
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
