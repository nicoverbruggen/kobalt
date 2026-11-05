#pragma once
#include <QString>
#include <QUrl>
#include <QVector>

namespace kobalt {
struct DictionaryChoice
{
    QString id, name;
    QString language = {};
    bool fallback = false;
};

struct DictionaryResult
{
    QString word, html, source, dictionary, dictionaryId, language, error;
    QUrl baseUrl;
    qint64 elapsedMs = 0;
    bool received = false, restored = false;
};

// Calls run synchronously on the GUI thread without pumping events. An async
// provider needs a separate contract; it must not nest an event loop here.
class DictionaryService
{
public:
    virtual ~DictionaryService() = default;
    virtual QVector<DictionaryChoice> choices() const = 0;
    virtual bool select(const QString &book, const QString &id, QString *error) = 0;
    virtual DictionaryResult lookup(const QString &book, const QString &word) = 0;
};
} // namespace kobalt
