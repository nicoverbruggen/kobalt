#include "cover_thumbnails.h"
#include "book_metadata.h"
#include "publication/publication.h"
#include "storage/preview_store.h"
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
    QString directory;
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
            bool missing = book.second.isEmpty() || !file.exists();
            if (!missing && file.open(QIODevice::ReadOnly)) {
                Publication publication;
                QString error;
                if (publication.inspect(file, &error) && !publication.protectedBook()) {
                    const auto fingerprint = publication.fingerprint();
                    if (book.first.startsWith("file-")
                        && book.first.mid(5).toLatin1() != fingerprint) {
                        missing = true;
                    } else {
                        const auto cache = "activity-cover:" + fingerprint + ':'
                                + QByteArray::number(size.width()) + 'x'
                                + QByteArray::number(size.height());
                        cover = storage::readPreview(directory, cache);
                        if (cover.isNull()) {
                            const auto source = readBookMetadata(file, publication).cover;
                            if (!source.isNull()) {
                                cover = source.scaled(size, Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation);
                                storage::writePreview(directory, cache, cover,
                                                      storage::PreviewStore::storageLimit);
                            }
                        }
                    }
                }
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
      directory(std::move(cacheDirectory)),
      generation(std::make_shared<std::atomic<quint64>>(0))
{
}

CoverThumbnails::~CoverThumbnails()
{
    ++*generation;
}

void CoverThumbnails::request(const QVector<QPair<QString, QString>> &books, QSize size,
                              Ready ready)
{
    const quint64 ticket = ++*generation;
    if (books.isEmpty())
        return;
    QPointer<CoverThumbnails> guard(this);
    auto *reader = new CoverReader;
    reader->books = books;
    reader->size = size;
    reader->directory = directory;
    reader->ticket = ticket;
    reader->generation = generation;
    reader->done = [guard, ticket, ready](const QString &key, const QImage &image, bool missing) {
        qt::invoke(
                QCoreApplication::instance(),
                [guard, ticket, ready, key, image, missing] {
                    if (guard && guard->generation->load() == ticket)
                        ready(key, image, missing);
                },
                Qt::QueuedConnection);
    };
    coverWorker().start(reader);
}
} // namespace kobalt::library
