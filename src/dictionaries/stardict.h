#pragma once
#include "platform/api/dictionary_service.h"
#include <memory>
#include <functional>

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
} // namespace kobalt
