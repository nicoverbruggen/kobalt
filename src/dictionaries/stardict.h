#pragma once
#include "platform/api/dictionary_service.h"
#include <memory>
#include <functional>
#include <QHash>

namespace kobalt {
// File-backed indexes and dictzip chunks keep large dictionaries off the heap.
class StarDict
{
public:
    StarDict();
    ~StarDict();
    bool open(const QString &directory, const QString &id, QString *error);
    DictionaryResult lookup(const QString &word);
    static bool prepare(const QString &directory, const QString &id, QString *error,
                        const std::function<bool()> &cancelled = {});

private:
    struct Data;
    std::unique_ptr<Data> data;
};

// User-supplied StarDict files, separate from downloaded dictionaries.
// Preparation writes derived indexes beside the files, on a worker.
class FolderDictionaries final : public DictionaryService
{
public:
    static void prepare(const QString &directory, const std::function<bool()> &cancelled = {});
    explicit FolderDictionaries(QString directory,
                                std::unique_ptr<DictionaryService> fallback = {});

    QVector<DictionaryChoice> choices() const override;

    bool select(const QString &book, const QString &id, QString *error) override;
    DictionaryResult lookup(const QString &book, const QString &word) override;

private:
    QString directory, activeId;
    QVector<DictionaryChoice> entries;
    QHash<QString, QString> selected;
    std::unique_ptr<StarDict> active;
    std::unique_ptr<DictionaryService> fallback;
};
} // namespace kobalt
