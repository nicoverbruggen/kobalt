#pragma once
#include "library_index.h"
#include <QIODevice>
#include <QObject>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

namespace kobalt::library {
// A host's access to its folders. Both calls run on the scan thread.
class Source
{
public:
    virtual ~Source() = default;
    // Lists the EPUB files below the folder, including subfolders. Returns
    // false when the folder cannot be read, so its books are kept.
    virtual bool list(const Folder &folder, QVector<Entry> &files,
                      const std::atomic<bool> &cancelled) = 0;
    virtual std::unique_ptr<QIODevice> open(const Entry &entry) = 0;
};

// Lists files on the desktop, where a folder key is a local path.
class LocalSource final : public Source
{
public:
    bool list(const Folder &folder, QVector<Entry> &files,
              const std::atomic<bool> &cancelled) override;
    std::unique_ptr<QIODevice> open(const Entry &entry) override;
};

bool isBookName(const QString &name);
// The cover file name for a book key.
QString coverName(const QString &key);

// Scans on its own thread and reports on the thread that owns it. Results
// arrive in order: each folder's listing, then the books read from it.
class Scanner final : public QObject
{
public:
    Scanner(std::shared_ptr<Source> source, QString coverDirectory, QObject *parent = nullptr);
    ~Scanner() override;
    // `index` is a snapshot: it only decides which files need reading.
    void start(const Index &index, QVector<Folder> folders);
    void cancel();

    bool running() const { return worker != nullptr; }

    // A folder was listed. Books under `removed` are gone from it.
    std::function<void(const QString &folder, const QStringList &removed)> listed;
    // A new or changed file was read. Library state such as the opened date
    // is not set; the owner keeps its own.
    std::function<void(const Book &book)> read;
    std::function<void()> finished;

private:
    std::shared_ptr<Source> source;
    QString covers;
    QThread *worker = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelled;
};
} // namespace kobalt::library
