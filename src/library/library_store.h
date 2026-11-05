#pragma once
#include "scanner.h"
#include <QTimer>

namespace kobalt::library {
// The library a host shows: its index file, the cover folder and the scanner.
// Every change saves the index shortly afterwards and calls `changed`.
class Store final : public QObject
{
public:
    Store(QString directory, std::shared_ptr<Source> source, QObject *parent = nullptr);
    ~Store() override;

    const Index &index() const { return data; }

    QString coverPath(const QString &name) const;
    bool scanning() const;

    bool available() const { return ready; }

    // Books read since the current scan started.
    int found() const { return readCount; }

    // Lists every folder and reads new and changed books.
    void scan();
    void addFolder(const Folder &folder);
    void removeFolder(const QString &key);
    // Adds a local file opened on its own. Returns false for a file that is
    // not an unprotected EPUB.
    bool addBook(const QString &path);
    void removeBook(const QString &key);
    void setMark(const QString &key, Book::Mark mark);
    void setOpened(const QString &key);
    void setProgress(const QString &key, int percent, bool finished);
    void setSettings(const Settings &settings);
    void save();
    std::function<void()> changed;

private:
    QString directory, coverDirectory;
    Index data;
    Scanner scanner;
    QTimer saving;
    int readCount = 0;
    bool rescan = false;
    bool ready = false;
    void touch();
};

// The reading position the reader saved for a book, as a percentage, and
// whether the reader saved it as finished.
struct SavedProgress
{
    int percent = 0;
    bool finished = false;
};

SavedProgress savedProgress(const QJsonObject &state);
} // namespace kobalt::library
