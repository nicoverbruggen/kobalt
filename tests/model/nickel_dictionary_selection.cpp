#include "platform/nickel/common/dictionary_selection.h"
#include "check.h"

#include <QVector>

namespace {
struct Dictionary {
    QString suffix, path, name;
    bool valid = true;
    bool installed = true;
    bool custom = false;
};
struct Api {
    bool valid(const Dictionary *dictionary) const {
        return dictionary->valid;
    }
    bool installed(const Dictionary *dictionary) const {
        return dictionary->installed;
    }
    bool custom(const Dictionary *dictionary) const {
        return dictionary->custom;
    }
    QString suffix(const Dictionary *dictionary) const {
        return dictionary->suffix;
    }
    QString path(const Dictionary *dictionary) const {
        return dictionary->path;
    }
    QString name(const Dictionary *dictionary) const {
        return dictionary->name;
    }
    QString language(const Dictionary *) const {
        return "es";
    }
};
}

int main() {
    const Api api;
    const Dictionary standard{"-es-en", "/dict/dicthtml-es-en.zip", "Spanish - English", true, false};
    const Dictionary custom{"-es-en", "/custom-dict/dicthtml-es-en.zip", "Spanish - English (Custom)", true, true,
                            true};
    QVector<Dictionary> dictionaries{standard, custom};
    const auto customId = kobalt::nickel::dictionaryId(custom, api);
    const auto selected = [&](const QString &id) {
        return kobalt::nickel::installedDictionary(dictionaries, id, api);
    };
    auto choices = kobalt::nickel::dictionaryChoices(dictionaries, api);
    require(choices.size() == 1 && choices.first().id == customId,
            "An uninstalled standard dictionary must not hide an installed custom dictionary");
    require(selected(customId) == &dictionaries.at(1), "A custom choice must resolve to that installed entry");
    require(selected("-es-en") == &dictionaries.at(1),
            "A legacy suffix must resolve when only its custom entry is installed");
    require(selected("-fr-en") == nullptr, "A missing suffix must not select another language");
    dictionaries[0].installed = true;
    choices = kobalt::nickel::dictionaryChoices(dictionaries, api);
    require(choices.size() == 2 && choices.at(0).id == "-es-en" && choices.at(1).id == customId,
            "Standard and custom dictionaries sharing a suffix need separate selector entries");
    require(choices.at(0).name == standard.name && choices.at(1).name == custom.name,
            "The selector must keep Nickel's standard and custom dictionary names");
    require(choices.at(1).language == "es" && choices.at(1).fallback, "Custom choices must keep platform metadata");
    require(selected("-es-en") == &dictionaries.at(0), "Saved standard suffixes must retain their original meaning");
    require(selected(customId) == &dictionaries.at(1),
            "Installing the standard dictionary must not redirect a custom choice");
    dictionaries.append({"-es-en", "/custom-dict/dicthtml-es-en", "Another custom dictionary", true, true, true});
    const auto otherId = kobalt::nickel::dictionaryId(dictionaries.at(2), api);
    require(otherId != customId && selected(otherId) == &dictionaries.at(2),
            "Custom entries sharing a suffix need distinct path identities");
    dictionaries.append(custom);
    require(kobalt::nickel::dictionaryChoices(dictionaries, api).size() == 3,
            "Repeated catalogue entries for the same dictionary must appear once");
    dictionaries.removeLast();
    dictionaries[1].installed = false;
    require(selected(customId) == nullptr,
            "Removing a custom dictionary must not redirect its saved ID to the standard entry");
    require(selected("-es-en") == &dictionaries.at(0),
            "Removing a custom dictionary must keep the standard entry available");
    dictionaries[0].valid = false;
    require(selected("-es-en") == &dictionaries.at(2),
            "An invalid standard entry must not hide an installed custom dictionary");
    dictionaries[2].installed = false;
    require(kobalt::nickel::dictionaryChoices(dictionaries, api).isEmpty() && !selected("-es-en"),
            "Invalid and uninstalled dictionaries must not be selected or listed");
    dictionaries.clear();
    require(selected(customId) == nullptr, "An empty catalogue must have no installed dictionary");
}
