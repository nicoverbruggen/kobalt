#include "scanner.h"
#include "book_metadata.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>

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

QString coverName(const QString &key)
{
    return QString::fromLatin1(
                   QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex())
            + ".png";
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
    cancel();
    if (worker) {
        worker->wait();
        delete worker;
    }
}

void Scanner::cancel()
{
    if (cancelled)
        cancelled->store(true);
}

void Scanner::start(const Index &index, QVector<Folder> folders)
{
    if (worker)
        return;
    cancelled = std::make_shared<std::atomic<bool>>(false);
    QPointer<Scanner> guard(this);
    auto stop = cancelled;
    auto access = source;
    const auto directory = covers;
    worker = QThread::create([guard, stop, access, directory, index, folders] {
        QDir().mkpath(directory);
        const auto deliver = [guard](std::function<void(Scanner &)> call) {
            QMetaObject::invokeMethod(guard.data(), [guard, call] {
                if (guard)
                    call(*guard);
            });
        };
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
                Book book;
                book.key = entry.key;
                book.folder = entry.folder;
                book.name = entry.name;
                book.size = entry.size;
                book.modified = entry.modified;
                auto device = access->open(entry);
                const auto metadata = device ? readBookMetadata(*device) : BookMetadata{};
                book.hidden = metadata.result != BookMetadata::Result::Ok;
                book.title = metadata.title;
                book.author = metadata.author;
                book.series = metadata.series;
                book.seriesIndex = metadata.seriesIndex;
                book.metadata = metadataVersion;
                const auto name = coverName(entry.key), path = directory + '/' + name;
                if (!metadata.cover.isNull() && metadata.cover.save(path, "PNG"))
                    book.cover = name;
                else
                    QFile::remove(path);
                deliver([book](Scanner &scanner) {
                    if (scanner.read)
                        scanner.read(book);
                });
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
