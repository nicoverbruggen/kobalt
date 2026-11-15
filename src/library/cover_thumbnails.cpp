#include "cover_thumbnails.h"
#include "book_metadata.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QFile>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

namespace kobalt::library {
namespace {
// Reads each book's cover in turn and hands it back to the owner. A newer
// request makes the rest of the list unnecessary.
class CoverReader final : public QRunnable
{
public:
    using Done = std::function<void(const QString &key, const QImage &cover, bool missing)>;
    QVector<QPair<QString, QString>> books;
    QSize size;
    quint64 ticket = 0;
    std::shared_ptr<std::atomic<quint64>> generation;
    Done done;

    void run() override
    {
        for (const auto &book : books) {
            if (generation->load() != ticket)
                return;
            QImage cover;
            QFile file(book.second);
            const bool missing = book.second.isEmpty() || !file.exists();
            if (!missing && file.open(QIODevice::ReadOnly)) {
                const auto metadata = readBookMetadata(file);
                if (!metadata.cover.isNull())
                    cover = metadata.cover.scaled(size, Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation);
            }
            done(book.first, cover, missing);
        }
    }
};

// One worker reads covers for every owner, one book at a time. An owner that
// goes away never waits for it; the application joins it on exit.
QThreadPool &coverWorker()
{
    struct Pool
    {
        QThreadPool threads;

        Pool()
        {
            threads.setMaxThreadCount(1);
            QObject::connect(
                    QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &threads,
                    [this] { threads.waitForDone(); }, Qt::DirectConnection);
        }
    };

    static Pool pool;
    return pool.threads;
}
} // namespace

CoverThumbnails::CoverThumbnails(QString cacheDirectory, QObject *parent)
    : QObject(parent),
      store(std::move(cacheDirectory)),
      generation(std::make_shared<std::atomic<quint64>>(0))
{
}

CoverThumbnails::~CoverThumbnails()
{
    ++*generation;
}

QByteArray CoverThumbnails::cacheKey(const QString &file, QSize size)
{
    return "activity-cover:" + file.toUtf8() + ':' + QByteArray::number(size.width()) + 'x'
            + QByteArray::number(size.height());
}

void CoverThumbnails::request(const QVector<QPair<QString, QString>> &books, QSize size,
                              Ready ready)
{
    const quint64 ticket = ++*generation;
    QVector<QPair<QString, QString>> unread;
    QVector<QByteArray> keys;
    for (const auto &book : books) {
        QImage cover;
        if (book.second.isEmpty()) {
            ready(book.first, QImage(), true);
        } else if (store.find(cacheKey(book.second, size), &cover)) {
            ready(book.first, cover, false);
        } else {
            unread.append(book);
            keys.append(cacheKey(book.second, size));
        }
    }
    if (unread.isEmpty())
        return;
    QPointer<CoverThumbnails> guard(this);
    store.load(keys, this, [guard, unread, size, ticket, ready] {
        if (!guard || guard->generation->load() != ticket)
            return;
        auto *reader = new CoverReader;
        for (const auto &book : unread) {
            QImage cover;
            if (guard->store.find(cacheKey(book.second, size), &cover))
                ready(book.first, cover, false);
            else
                reader->books.append(book);
        }
        if (reader->books.isEmpty()) {
            delete reader;
            return;
        }
        reader->size = size;
        reader->ticket = ticket;
        reader->generation = guard->generation;
        QHash<QString, QString> files;
        for (const auto &book : reader->books)
            files.insert(book.first, book.second);
        // The worker may finish after the owner is gone, so the result goes
        // through the application and the guard is checked on its thread.
        reader->done = [guard, files, size, ready](const QString &key, const QImage &cover,
                                                   bool missing) {
            qt::invoke(
                    QCoreApplication::instance(),
                    [guard, key, cover, missing, file = files.value(key), size, ready] {
                        if (!guard)
                            return;
                        // A book without a readable cover is not cached, so a
                        // later fix to the file shows.
                        if (!cover.isNull())
                            guard->store.insert(cacheKey(file, size), cover);
                        ready(key, cover, missing);
                    },
                    Qt::QueuedConnection);
        };
        coverWorker().start(reader);
    });
}
} // namespace kobalt::library
