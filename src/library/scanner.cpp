#include "scanner.h"
#include "book_metadata.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include "support/main_thread.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QUuid>

namespace kobalt::library {
namespace {
bool hiddenPath(const QString &root, const QString &directory)
{
    for (const auto &part : QDir(root).relativeFilePath(directory).split('/'))
        if (part.startsWith('.') && part != QLatin1String("."))
            return true;
    return false;
}
} // namespace

bool isBookName(const QString &name)
{
    return name.endsWith(QLatin1String(".epub"), Qt::CaseInsensitive) && !name.startsWith('.');
}

QString saveCover(const QString &directory, const QImage &cover)
{
    if (cover.isNull())
        return {};
    const auto name = QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png";
    QSaveFile file(directory + '/' + name);
    return file.open(QIODevice::WriteOnly) && cover.save(&file, "PNG") && file.commit() ? name
                                                                                        : QString();
}

bool LocalSource::list(const Folder &folder, QVector<Entry> &files,
                       const std::atomic<bool> &cancelled)
{
    const QFileInfo root(folder.key);
    if (!root.isDir() || !root.isReadable())
        return false;
    // Hidden folders hold application data, not books. Symbolic links are not
    // followed, so a link back up the tree cannot repeat the walk.
    QDirIterator walk(folder.key, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot,
                      QDirIterator::Subdirectories);
    while (walk.hasNext() && !cancelled.load()) {
        walk.next();
        const auto info = walk.fileInfo();
        if (!isBookName(info.fileName()) || info.isSymLink()
            || hiddenPath(root.absoluteFilePath(), info.absolutePath()))
            continue;
        files.append({ info.absoluteFilePath(), folder.key, info.fileName(), info.size(),
                       info.lastModified().toMSecsSinceEpoch() });
    }
    return !cancelled.load();
}

std::unique_ptr<QIODevice> LocalSource::open(const Entry &entry)
{
    auto file = std::make_unique<QFile>(entry.key);
    if (!file->open(QIODevice::ReadOnly))
        return {};
    return file;
}

Scanner::Scanner(std::shared_ptr<Source> source, QString coverDirectory, QObject *parent)
    : QObject(parent), source(std::move(source)), covers(std::move(coverDirectory))
{
}

Scanner::~Scanner()
{
    listed = {};
    read = {};
    finished = {};
    finish();
}

void Scanner::finish()
{
    if (!worker)
        return;
    cancelled->store(true);
    mainthread::blocking("library-scan-finish");
    worker->wait();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}

void Scanner::start(const Index &index, QVector<Folder> folders, QStringList singles)
{
    if (worker)
        return;
    cancelled = std::make_shared<std::atomic<bool>>(false);
    QPointer<Scanner> guard(this);
    auto stop = cancelled;
    auto access = source;
    const auto directory = covers;
    worker = QThread::create([guard, stop, access, directory, index, folders, singles] {
        QDir().mkpath(directory);
        const auto deliver = [guard](std::function<void(Scanner &)> call) {
            QMetaObject::invokeMethod(guard.data(), [guard, call] {
                if (guard)
                    call(*guard);
            });
        };
        const auto readEntry = [&](Source &reader, const Entry &entry) {
            Book book;
            book.key = entry.key;
            book.folder = entry.folder;
            book.name = entry.name;
            book.size = entry.size;
            book.modified = entry.modified;
            auto device = reader.open(entry);
            const auto metadata = device ? readBookMetadata(*device) : BookMetadata{};
            book.hidden = metadata.result != BookMetadata::Result::Ok;
            if (entry.folder.isEmpty() && book.hidden)
                return;
            book.title = metadata.title;
            book.author = metadata.author;
            book.series = metadata.series;
            book.seriesIndex = metadata.seriesIndex;
            book.metadata = metadataVersion;
            mainthread::blocking("library-scan-cover");
            book.cover = saveCover(directory, metadata.cover);
            deliver([book](Scanner &scanner) {
                if (scanner.read)
                    scanner.read(book);
            });
        };
        LocalSource local;
        for (const auto &key : singles) {
            if (stop->load())
                return;
            const QFileInfo file(key);
            readEntry(local,
                      { key,
                        {},
                        file.fileName(),
                        file.size(),
                        file.lastModified().toMSecsSinceEpoch() });
        }
        for (const auto &folder : folders) {
            if (stop->load())
                return;
            QVector<Entry> files;
            if (!access->list(folder, files, *stop))
                continue;
            const auto plan = index.plan(folder.key, files);
            const auto removed = plan.removed;
            const auto key = folder.key;
            deliver([key, removed](Scanner &scanner) {
                if (scanner.listed)
                    scanner.listed(key, removed);
            });
            for (const auto &entry : plan.read) {
                if (stop->load())
                    return;
                readEntry(*access, entry);
            }
        }
    });
    connect(worker, &QThread::finished, this, [this] {
        worker->deleteLater();
        worker = nullptr;
        cancelled.reset();
        if (finished)
            finished();
    });
    worker->start(QThread::LowPriority);
}
} // namespace kobalt::library
