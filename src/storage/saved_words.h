#pragma once
#include "record_writer.h"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

class QSqlDatabase;

namespace kobalt {
// A word from a dictionary lookup, saved in the profile database. The book
// fields are a snapshot from the lookup, so a word stays when its book leaves
// the Library. `book` is the annotation book key: a sideloaded book is known
// by its content, so a moved or renamed file keeps its words.
struct SavedWord
{
    QString book, word, identity, fingerprint, sourceKey, title, author;
    QString chapter, context, position, dictionary, definition;
    // UTC milliseconds of the first and the latest lookup.
    qint64 created = 0, lookedUp = 0;
    int lookups = 1;
    // Lookups of "Type" and "type" are the same saved word.
    static QString key(const QString &word);
    // The plain text of the definition's first lines, for a list row. Empty
    // for a word the reader saved without a dictionary entry.
    QString gloss() const;
};

// Every call runs on the profile writer thread, in the order it was made.
// Results return to the thread that owns the store. The interface never waits.
class SavedWords final : public QObject
{
public:
    static constexpr int maxContext = 1024, maxDefinition = 16 * 1024, maxText = 4096;
    explicit SavedWords(QString directory, QObject *parent = nullptr);
    ~SavedWords() override;
    using Loaded = std::function<void(bool ok, QVector<SavedWord> words)>;
    // A word already saved for this book counts one more lookup. It keeps its
    // first sentence and position and takes the latest definition, but an
    // empty definition never replaces a saved one.
    void record(SavedWord word, std::function<void(bool)> finished = {});
    void remove(const QString &book, const QString &word, std::function<void(bool)> finished = {});
    // An opened book updates the path and title its saved words point to.
    void relocate(const QString &book, const QString &sourceKey, const QString &title,
                  const QString &author);
    // An empty book lists every book's words, newest lookup first.
    void load(const QString &book, Loaded loaded);
    // Whether this book has the word. False when the read fails.
    void contains(const QString &book, const QString &word, std::function<void(bool)> answer);
    // After each successful change.
    std::function<void()> changed;

private:
    struct Queue;
    void enqueue(std::function<bool(QSqlDatabase &)> operation, std::function<void(bool)> done);
    QString directory;
    std::shared_ptr<Queue> queue;
    storage::RecordWriter writer;
};
} // namespace kobalt
