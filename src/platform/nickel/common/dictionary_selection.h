#pragma once

#include "platform/api/dictionary_service.h"

namespace kobalt::nickel {
// Preserve saved standard suffixes. Custom entries need their path as identity
// because Nickel can assign the same suffix to standard and custom dictionaries.
template <class Dictionary, class Api> QString dictionaryId(const Dictionary &dictionary, const Api &api) {
    return api.custom(&dictionary) ? QStringLiteral("custom:") + api.path(&dictionary) : api.suffix(&dictionary);
}

template <class Dictionaries, class Api>
QVector<DictionaryChoice> dictionaryChoices(const Dictionaries &dictionaries, const Api &api) {
    QVector<DictionaryChoice> result;
    for (const auto &dictionary : dictionaries) {
        if (!api.valid(&dictionary) || !api.installed(&dictionary)) {
            continue;
        }
        const auto id = dictionaryId(dictionary, api);
        bool duplicate = false;
        for (const auto &choice : result) {
            duplicate |= choice.id == id;
        }
        if (!duplicate) {
            result.append({id, api.name(&dictionary), api.language(&dictionary), true});
        }
    }
    return result;
}

// The returned pointer belongs to dictionaries; keep that list alive during lookup.
template <class Dictionaries, class Api>
const typename Dictionaries::value_type *installedDictionary(const Dictionaries &dictionaries, const QString &id,
                                                             const Api &api) {
    for (const auto &dictionary : dictionaries) {
        if (api.valid(&dictionary) && api.installed(&dictionary) && dictionaryId(dictionary, api) == id) {
            return &dictionary;
        }
    }
    // Automatic and legacy selections contain only a suffix. A custom entry
    // can supply it when the standard dictionary is not installed.
    if (!id.startsWith("custom:")) {
        for (const auto &dictionary : dictionaries) {
            if (api.valid(&dictionary) && api.installed(&dictionary) && api.suffix(&dictionary) == id) {
                return &dictionary;
            }
        }
    }
    return nullptr;
}
}
